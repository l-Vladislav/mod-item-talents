-- mod-item-talents: item_entry-штамп против переиспользования GUID.
-- GUID предметов в WoW переиспользуются: уничтоженный предмет освобождает
-- low-GUID, новый предмет может его получить и унаследовать чужую строку
-- item_talents (килы/уровень/выборы). Штампуем entry предмета; модуль на
-- загрузке сверяет entry и при несовпадении считает строку чужой и
-- сбрасывает. Плюс разовая чистка уже осиротевших строк обеих таблиц.
-- Идемпотентно: ALTER + backfill только при отсутствии колонки.

SET @have_col := (SELECT COUNT(*) FROM information_schema.columns
    WHERE table_schema = DATABASE() AND table_name = 'item_talents'
      AND column_name = 'item_entry');

SET @ddl := IF(@have_col = 0,
    'ALTER TABLE `item_talents` ADD COLUMN `item_entry` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `owner_guid`',
    'SELECT 1');
PREPARE s FROM @ddl; EXECUTE s; DEALLOCATE PREPARE s;

-- Backfill: проставить текущий entry валидным строкам (у которых guid ещё
-- существует) - только при первом применении, чтобы не трогать уже
-- прожитые сбросы/штампы.
SET @bf := IF(@have_col = 0,
    'UPDATE `item_talents` t JOIN `item_instance` ii ON ii.guid = t.item_guid SET t.item_entry = ii.itemEntry',
    'SELECT 1');
PREPARE s FROM @bf; EXECUTE s; DEALLOCATE PREPARE s;

-- Разовая чистка осиротевших строк (предмет уничтожен - guid нет в
-- item_instance). Безопасно и идемпотентно (повторный прогон - no-op).
DELETE t FROM `item_talents` t
    LEFT JOIN `item_instance` ii ON ii.guid = t.item_guid
    WHERE ii.guid IS NULL;
DELETE r FROM `item_talent_rolls` r
    LEFT JOIN `item_instance` ii ON ii.guid = r.item_guid
    WHERE ii.guid IS NULL;
