-- mod-item-talents: уровни пробуждения как СЕГМЕНТЫ (решение 2026-07-07).
-- Пороги ItemTalents.PointThresholds ("50,150,400,1000,2500") теперь
-- трактуются КАК ЕСТЬ - за уровень: уровень 1 = 50 убийств, затем счётчик
-- обнуляется и уровень 2 = ещё 150, и т.д. Уровень больше не выводится из
-- накопительных kills - хранится в новой колонке item_talents.level, а
-- kills становится счётчиком убийств ВНУТРИ текущего уровня.
--
-- Backfill существующих строк: уровень по СТАРЫМ кумулятивным порогам
-- 50/200/600/1600/4100 (текущие дефолты, захардкожены осознанно), остаток
-- убийств сверх взятого порога идёт в счёт следующего уровня.
--
-- Идемпотентность (паттерн mod_item_talents_subclass.sql): ALTER и backfill
-- выполняются ТОЛЬКО если колонки level ещё нет (@need_migrate вычисляется
-- ДО ALTER) - повторное применение не тронет уже мигрированные данные.
-- Гейт на существование таблицы: на свежей БД файл может примениться раньше
-- mod_item_talents_table.sql (тот создаёт таблицу сразу с level).
--
-- ВНИМАНИЕ: применять ДО старта бинарника с поддержкой level - модуль
-- отключает себя, если колонки нет (см. ItemTalentsMgr::LoadDefinitions).

SET @has_table := (SELECT COUNT(*) FROM information_schema.tables
    WHERE table_schema = DATABASE() AND table_name = 'item_talents');
SET @has_level := (SELECT COUNT(*) FROM information_schema.columns
    WHERE table_schema = DATABASE() AND table_name = 'item_talents'
      AND column_name = 'level');
SET @need_migrate := (@has_table = 1 AND @has_level = 0);

SET @ddl := IF(@need_migrate,
    'ALTER TABLE `item_talents` ADD COLUMN `level` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `row5`',
    'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- level присваивается ПЕРВЫМ (MySQL исполняет SET слева направо): выражение
-- для kills ещё видит исходное накопительное значение.
SET @backfill := IF(@need_migrate,
    'UPDATE `item_talents` SET
       `level` = CASE
         WHEN `kills` >= 4100 THEN 5
         WHEN `kills` >= 1600 THEN 4
         WHEN `kills` >=  600 THEN 3
         WHEN `kills` >=  200 THEN 2
         WHEN `kills` >=   50 THEN 1
         ELSE 0 END,
       `kills` = `kills` - CASE
         WHEN `kills` >= 4100 THEN 4100
         WHEN `kills` >= 1600 THEN 1600
         WHEN `kills` >=  600 THEN 600
         WHEN `kills` >=  200 THEN 200
         WHEN `kills` >=   50 THEN 50
         ELSE 0 END',
    'SELECT 1');
PREPARE stmt FROM @backfill;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
