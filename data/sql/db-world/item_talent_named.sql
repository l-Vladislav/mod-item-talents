-- mod-item-talents: именные наборы перков ряда 5 (item_talent_named).
-- Первый именной набор - Посох Джордана (entry 873, классический эпик,
-- двуручный посох). Спека: .claude/item-talents/PERKS.md "Именное
-- пробуждение". Ряд 5 именного предмета: меню = 3 именных перка, роллятся
-- ВСЕ 3 БЕЗ качества (quality всегда 0); v1-ТЕСТОВОЕ послабление - ряд 5
-- открыт уже на эпике (финальное правило фазы 2 - легендарка).
-- proc_chance = 0 -> пассив; {N}/{chance} подставляет модуль.
-- JORDAN_LIGHTNING визуал - NPC-спелл 9532 "Lightning Bolt" (см. модуль).
-- Идемпотентно: CREATE TABLE IF NOT EXISTS + DELETE + INSERT.

CREATE TABLE IF NOT EXISTS `item_talent_named` (
  `item_entry`  INT UNSIGNED NOT NULL,
  `choice`      TINYINT UNSIGNED NOT NULL,           -- 1..3
  `name_ru`     VARCHAR(64) NOT NULL,
  `desc_ru`     VARCHAR(255) NOT NULL,
  `effect`      VARCHAR(32) NOT NULL,
  `base`        FLOAT NOT NULL DEFAULT 0,
  `per_ilvl`    FLOAT NOT NULL DEFAULT 0,
  `proc_chance` TINYINT UNSIGNED NOT NULL DEFAULT 0, -- 0 = пассив
  `icd_secs`    INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`item_entry`,`choice`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DELETE FROM `item_talent_named` WHERE `item_entry` = 873;

INSERT INTO `item_talent_named`
  (`item_entry`,`choice`,`name_ru`,`desc_ru`,`effect`,`base`,`per_ilvl`,`proc_chance`,`icd_secs`) VALUES
(873,1,'Гнев Джордана','Заклинания даруют шанс {chance}% поразить цель молнией ({N} урона природой)','JORDAN_LIGHTNING',0,1.2,15,8),
(873,2,'Цена Джордана','Убийства с шансом {chance}% чеканят владельцу {N} меди','JORDAN_COIN',0,5.0,20,20),
(873,3,'Щедрость Джордана','+{N} к максимуму маны','MANA_FLAT',0,3.0,0,0);
