# mod-item-talents — «Пробуждение снаряжения»

Модуль AzerothCore (WotLK 3.3.5a): ветка талантов у каждого предмета
(оружие/броня) — 5 рядов, выбор 1 из 3. Качество предмета открывает ряды,
опыт предмета (убийства, пока вещь надета) даёт очки талантов. Прокачка
привязана к конкретному предмету (GUID), а не к игроку.

## Установка

1. Клонировать в `modules/mod-item-talents`.
2. Применить `core-hook.patch` к ядру (один новый хук, 4 файла, ~15 строк).
3. Применить SQL из `data/sql/`: `db-world/item_talent_def.sql` (acore_world),
   `db-characters/item_talents.sql` (acore_characters).
4. Пересобрать worldserver; конфиг — `conf/mod_item_talents.conf.dist`.

Лицензия: GNU GPL v2 (как AzerothCore).

## v1 (эта итерация)

- Пулы A–H по (class, subclass); ряды 1–2 рабочие (флэт-статы и рейтинги,
  применяются напрямую как статы предмета, без spell_dbc), ряды 3–5
  показываются, но выбор запрещён (`ItemTalents.MaxImplementedRow = 2`).
- Опыт: +1 всем надетым подходящим предметам за убийство, дающее честь/опыт
  (`Player::isHonorOrXPTarget`); PvP не считается; плейерботы не копят
  (`ItemTalents.IgnoreBots`). Флаш пачкой при сохранении/логауте.
- Выбор/сброс — только рядом с мастером оружия (`ItemTalents.MasterEntries`).
  Первый вложенный талант привязывает предмет.
- Команда `.itemtalent` (протокол ITALENT: для аддона ItemTalentUI):
  - `info <bag> <slot>` / `info inv <slot>` — HDR/ROW/OPT/END
    (клиентские координаты: bag 0..4 со слотами с 1; inv 1..19).
  - `choose <itemGuidLow> <row> <choice>` — выбрать (OK + свежий info).
  - `reset <itemGuidLow> <row>` — сброс ряда (OK + свежий info).
  - `list` — надетые предметы системы (ITEM-строки + END).
  - Коды ошибок: DISABLED, BAD_ARGS, NO_ITEM, NOT_EQUIPPED, NO_POOL, BROKEN,
    ROW_LOCKED, ROW_SOON, BAD_CHOICE, ALREADY_CHOSEN, NO_POINTS, NO_MASTER,
    NOT_CHOSEN.

## Данные

- `acore_world.item_talent_def` — определения талантов (data-driven: пулы
  A–H x ряды x 3 выбора; значения `ceil(base + per_ilvl * ItemLevel)`).
  Баланс тюнится SQL-ом без пересборки.
- `acore_characters.item_talents` — выборы и kills по item_guid.

## Патч ядра

Один новый хук `OnPlayerApplyItemMods(Player*, Item*, slot, apply)` в конце
`Player::_ApplyItemMods` (симметричен на equip/unequip/login/починку):
PlayerScript.h / PlayerScript.cpp / ScriptMgr.h / Player.cpp.

## Следующие итерации

Госсип у мастеров, эффекты рядов 3–4 (утилити/синергия), перенос строки
item_talents при GA-апгрейде, ряд 5 (проки, фаза 2).
