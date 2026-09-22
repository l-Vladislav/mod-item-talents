# mod-item-talents — «Пробуждение снаряжения»

Модуль AzerothCore (WotLK 3.3.5a): ветка талантов у каждого предмета
(оружие/броня) — 5 рядов, выбор 1 из 3 слотов. Меню ряда держит до ~10
вариантов; предмет при первом обращении лениво роллит 3 случайных с
качеством (Обычный x1.0 / Отличный x1.25 / Совершенный x1.5, веса 70/25/5).
Качество предмета открывает ряды, опыт предмета (убийства, пока вещь
надета) даёт очки талантов. Прокачка привязана к конкретному предмету
(GUID), а не к игроку.

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
    OPT — по роллам предмета:
    `OPT:<row>:<slot 1..3>:<effect>:<value>:<perkQuality 0..2>:<name_ru>`.
  - `choose <itemGuidLow> <row> <slot>` — выбрать слот (OK + свежий info).
  - `reset <itemGuidLow> <row>` — сброс ряда (OK + свежий info).
  - `list` — надетые предметы системы (ITEM-строки + END).
  - Коды ошибок: DISABLED, BAD_ARGS, NO_ITEM, NOT_EQUIPPED, NO_POOL, BROKEN,
    ROW_LOCKED, ROW_SOON, BAD_CHOICE, ALREADY_CHOSEN, NO_POINTS, NO_MASTER,
    NOT_CHOSEN.

## Данные

- `acore_world.item_talent_def` — определения талантов (data-driven: пулы
  A–H x ряды x choice 1..N (до 15); значения
  `ceil(ceil(base + per_ilvl * ItemLevel) * qualityMult)`).
  Баланс тюнится SQL-ом без пересборки.
- `acore_characters.item_talents` — выборы (rowN = слот 1..3), уровень
  пробуждения (level 0..5) и kills ВНУТРИ текущего уровня по item_guid
  (пороги - сегменты за уровень, взятие уровня обнуляет счётчик).
- `acore_characters.item_talent_rolls` — роллы слотов: (item_guid, row,
  slot 1..3) -> (choice, quality 0..2).

### V3: конфигурация из админ-панели (2026-08-19)

Миграция `pending_db_world/mod_item_talents_v3_admin.sql` вынесла в БД то,
что было зашито в C++ и в conf. Все таблицы ОПЦИОНАЛЬНЫ: без них модуль
работает как раньше (жёсткая карта классов + `ItemTalents.PointThresholds`).

- `item_talent_category` / `item_talent_category_rule` — категории вместо
  букв A..H и правила «какой предмет в какую категорию» (замена
  `ItemTalentsMgr::GetPool`). Матч по (class, subclass, InvType, качество,
  диапазон entry), побеждает наибольший `priority`.
- `item_talent_row_cfg` — по (категория, ряд): `roll_count` (сколько
  вариантов меню выпадает в 3 слота UI) и `quality_enabled`. Меню может быть
  до 30 вариантов: 10 вариантов и `roll_count` 3 = «3 случайных из 10».
- `item_talent_item_def` / `item_talent_item_cfg` — ПЕРСОНАЛЬНЫЙ пул
  конкретного предмета; перекрывает меню категории. Обобщение
  `item_talent_named`: ровно 3 варианта = именной набор (выпадают всегда).
- `item_talent_kill_curve` — пороги убийств по (качество, ilvl): предмет
  ilvl 10 больше не требует столько же убийств, сколько ilvl 264.
- `item_talent_perk_library` — заготовки перков, ядро её не читает.

Правки применяются командой `.itemtalent reload` (SEC_ADMINISTRATOR,
доступна и по SOAP) — рестарт не нужен. Уже разданные роллы она не трогает:
предмет с выбором, которого больше нет в меню, чинится `.itemtalent reroll`.
Редактор — `/italents.html` в tools/admin-panel.

## Патч ядра

Один новый хук `OnPlayerApplyItemMods(Player*, Item*, slot, apply)` в конце
`Player::_ApplyItemMods` (симметричен на equip/unequip/login/починку):
PlayerScript.h / PlayerScript.cpp / ScriptMgr.h / Player.cpp.

## Следующие итерации

Госсип у мастеров, эффекты рядов 3–4 (утилити/синергия), перенос строки
item_talents при GA-апгрейде, ряд 5 (проки, фаза 2).
