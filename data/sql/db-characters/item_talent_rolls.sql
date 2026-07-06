-- Item Talents (mod-item-talents) — per-slot perk rolls (characters DB).
-- Design: .claude/item-talents/DESIGN.md section "Роллы перков и качество"
-- and .claude/item-talents/PERKS.md section "Роллы и качество" (2026-07-06):
-- each row's menu was widened to ~8-10 variants, but an item only ever rolls
-- 3 of them (lazily, on first access, keyed by item GUID) — the in-game
-- choice stays "pick 1 of 3". `item_talents.rowN` keeps storing the chosen
-- SLOT (1..3), not the underlying choice id; this table is the slot ->
-- choice/quality lookup that backs it.
-- `quality` is the roll-quality multiplier tier: 0 = Обычный x1.0 (70%),
-- 1 = Отличный x1.25 (25%), 2 = Совершенный x1.5 (5%) — rolled
-- independently per slot (ItemTalents.QualityWeights / QualityMults).
-- No seed data — rows are created/updated by the module at runtime
-- (EnsureState lazy roll), so this file only ensures the schema exists.
-- Idempotent: CREATE TABLE IF NOT EXISTS; safe to re-apply.

CREATE TABLE IF NOT EXISTS `item_talent_rolls` (
  `item_guid` INT UNSIGNED NOT NULL,
  `row`       TINYINT UNSIGNED NOT NULL,
  `slot`      TINYINT UNSIGNED NOT NULL,  -- 1..3
  `choice`    TINYINT UNSIGNED NOT NULL,  -- id of the option in item_talent_def
  `quality`   TINYINT UNSIGNED NOT NULL DEFAULT 0, -- 0 обычный, 1 отличный, 2 совершенный
  PRIMARY KEY (`item_guid`,`row`,`slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
