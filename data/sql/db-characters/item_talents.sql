-- Item Talents (mod-item-talents) — per-item talent state (characters DB).
-- Design: .claude/item-talents/DESIGN.md, section 6 "Техническая реализация".
-- item_guid is the item's persistent GUID (PK); rowN = 0 (unselected) or 1..3
-- (chosen slot); level = awakening level 0..5 (decision 2026-07-07: level-up
-- RESETS the kill counter, thresholds are per-level segments); kills = kill
-- counter WITHIN the current level, flushed on save/logout (not per kill)
-- per DESIGN.md "Опыт предмета (kills)". No seed data — rows are
-- created/updated by the module at runtime, so this file only ensures the
-- schema exists (idempotent via CREATE TABLE IF NOT EXISTS). Existing
-- installs get `level` via item_talents_level.sql (gated ALTER + backfill).

CREATE TABLE IF NOT EXISTS `item_talents` (
  `item_guid`  INT UNSIGNED NOT NULL,
  `owner_guid` INT UNSIGNED NOT NULL,
  `item_entry` INT UNSIGNED NOT NULL DEFAULT 0,
  `row1` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `row2` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `row3` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `row4` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `row5` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `kills` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`item_guid`),
  KEY `idx_owner` (`owner_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
