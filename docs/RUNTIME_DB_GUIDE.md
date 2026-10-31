# Adding upgrade parts to the garage at runtime (database, hooks, texts)

This is the technical write-up of how **NitroSystem II** adds new entries to Forza Horizon 2's **Garage > Upgrades** menu **without changing any file of the game**. The same method works for any new upgrade type or option: it is the part that was the hard one to figure out, because the game's upgrade system has no extension point and the menu is driven by SQL views and a few fixed native functions.

Audience: people who write mods on the [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform). You should know what `add_sql`, `hook_function`, `set_file` and `wait_for_code` are (see the platform's `docs/api.md`). The working code is in this repository: the SQL text work is in `src/nu_sql.h` (plain string functions with no Windows calls, unit-tested on any machine) and the hooks are in `src/nu_upgrade.h`.

Everything below was found by reading the game's behaviour (its SQL statements, which the mods log) and its executable (`forza_durango_release_final.exe` of the Anthem 1.0.0.64 package). **Addresses are specific to that build**; every hook checks the first bytes of the function before it hooks it, and switches itself off with a log line if they differ.

1. [The big picture](#1-the-big-picture)
2. [Step 1: the static rows (`add_sql`)](#2-step-1-the-static-rows-add_sql)
3. [Step 2: making the type appear for a car (the per-car view)](#3-step-2-making-the-type-appear-for-a-car-the-per-car-view)
4. [Step 3: which option is installed](#4-step-3-which-option-is-installed)
5. [Step 4: buying and installing](#5-step-4-buying-and-installing)
6. [Step 5: names and descriptions (string references)](#6-step-5-names-and-descriptions-string-references)
7. [Step 6: icons](#7-step-6-icons)
8. [Step 7: remembering the choice per car](#8-step-7-remembering-the-choice-per-car)
9. [Step 8: using the choice in the mod](#9-step-8-using-the-choice-in-the-mod)
10. [Hooks and addresses used](#10-hooks-and-addresses-used)
11. [ID registry (please avoid these)](#11-id-registry-please-avoid-these)
12. [Pitfalls we hit](#12-pitfalls-we-hit)
13. [Testing](#13-testing)
14. [A minimal example](#14-a-minimal-example)

## 1. The big picture

The garage builds its menus from the game's SQLite database (`media\Stripped\gamedbRC.slt`). The relevant pieces:

| Object | What it is |
|---|---|
| `UpgradeAreas` | the top-level groups (1 = Engine, 4 = Tires and rims and 8 = Conversion are the ones this project placed types in) |
| `UpgradeTypes` | one row per upgrade **type** ("Front Rim Size", "Tire Compound", ...), with the area it belongs to, its `PartEnum`, display order, name/description strings and icon paths |
| `Upgrades` | one row per **option** of a type (levels: stock, +1, +2, ...) |
| `List_Upgrade*` tables | the actual part data per category (for example `List_UpgradeMotorParts`: price, mass, ...) |
| `View_UpgradeParts` | a **per-car view**, rebuilt with `CREATE VIEW` every time the garage opens a car (`... WHERE Ordinal = <car ordinal>`): it `UNION`s the parts that exist for that car |
| `View_UpgradeTypes` | the types the menu shows: |

```sql
CREATE VIEW %sView_UpgradeTypes AS SELECT UpgradeTypes.* FROM UpgradeTypes
  JOIN %sView_UpgradeParts AS PartTable ON PartTable.PartEnum = UpgradeTypes.PartEnum AND (PartTable.IsException = UpgradeTypes.IsException)
  GROUP BY id, UpgradeTypes.PartEnum, AreaId, UpgradeTypes.IsException, Name, Description, IconPath, ImagePath, DisplayOrder
  HAVING COUNT(*) > 1 OR (COUNT(*) = 1 AND PartTable.IsStock = 0)
  ORDER BY UpgradeTypes.DisplayOrder
```

So a type is listed for a car only if `View_UpgradeParts` contains **at least two rows of its `PartEnum`** (or one non-stock row). Adding an upgrade type therefore needs two things: the static type/option rows, and our rows inside the per-car part view. The rest (installing, showing the installed option, texts, icons) are details around those two.

The platform gives us two tools:

- **`add_sql`** runs statements against a *copy* of the database at startup (the original is untouched; the copy goes to `mediaoverride`). Good for the static rows.
- **`hook_function`** on the game's database query function lets us edit the SQL text of single statements **in memory** just before the game runs them. Good for the per-car view, the installed marker and the install write.

## 2. Step 1: the static rows (`add_sql`)

Queue the rows in `fh2mod_init` (statements added there are applied for you). Three tables:

```sql
-- the type: AreaId puts it in a menu group; IsException = 0; DisplayOrder sorts inside the group
INSERT OR REPLACE INTO UpgradeTypes
  (id, PartEnum, AreaId, IsException, DisplayOrder, EverymanUpgradeDisplayOrder, Name, Description, IconPath, ImagePath)
VALUES (7452, 47, 1, 0, 15, -1, '_&<name ref>', '_&<description ref>',
        'GAME:\Media\UI\Textures\Data_Bound\Upgrade_Parts\...\x_Icon.swatchbin',
        'GAME:\Media\UI\Textures\Data_Bound\Upgrade_Parts\...\x_LG_Icon.swatchbin');

-- one row per option: Level 0 is "stock"
INSERT OR REPLACE INTO Upgrades (id, TypeId, IsException, Name, Level, Description, IconPath, ImagePath)
VALUES (74521, '7452', 0, '_&<ref>', 1, '_&<ref>', '<icon>', '<large icon>');

-- the price (see step 4 for why it lives in this table)
INSERT OR REPLACE INTO List_UpgradeMotorParts
  (Id, MotorID, Level, IsStock, ManufacturerID, MassDiff, WeightDistDiff, Price, TorqueScale)
VALUES (900471, -7452, 1, 0, 0, 0.0, 0.0, 100, 1.0);
```

Notes:

- `PartEnum` is the game's number for "what kind of part". The game's own types use 0 to 44; **new types need a new number** (these mods use 45, 46 and 47, see the [registry](#11-id-registry-please-avoid-these)).
- `EverymanUpgradeDisplayOrder = -1` keeps the type out of the simplified "Everyman" upgrade menu.
- `Name` / `Description` are not text but **string references** (`_&<number>`), see step 5.
- `IconPath` / `ImagePath` are game paths of `.swatchbin` icons, see step 6 and the [icon guide](ICON_GUIDE.md).
- `INSERT OR REPLACE` makes the statements safe to run again. The platform runs each mod's statements in one transaction and rolls the whole mod back if one fails (it logs the SQLite error).

## 3. Step 2: making the type appear for a car (the per-car view)

When the garage opens a car, the game runs a statement of this shape (shortened):

```sql
CREATE VIEW ...View_UpgradeParts AS SELECT 0 AS PartEnum, ... WHERE Ordinal = 2049
  UNION SELECT ... UNION SELECT ...
```

In the query hook, we recognise it by its first characters (`CREATE VIEW`) and its first 160 bytes (`View_UpgradeParts AS SELECT 0 AS PartEnum`), take the car's ordinal from `WHERE Ordinal = N`, and insert our own rows in front of the first ` UNION SELECT `:

```sql
 UNION SELECT 47 AS PartEnum, 900471 AS PartId, 2049 AS Ordinal, 1 AS IsStock, 0 AS Level,
              NULL AS ManufacturerID, 0 AS Price, 0 AS IsException
 UNION SELECT 47 AS PartEnum, 900472 AS PartId, 2049 AS Ordinal, 0 AS IsStock, 1 AS Level,
              NULL AS ManufacturerID, 100 AS Price, 0 AS IsException
```

One row per option. The stock option (`IsStock = 1`, price 0) must be there, and the `PartId` must be unique and map back to the option (we use `base + level`).

`tw_view()` / `nu_view()` in `src/nu_sql.h` is this transformation as a pure function (it returns 0 and leaves the statement alone if it does not recognise it, and also if our rows are already in it, so it is idempotent). It also remembers the car ordinal for the later steps.

## 4. Step 3: which option is installed

When the menu opens a type, the game asks for the parts of the type and which is installed:

```sql
SELECT PartTable.PartId AS id, ... WHERE PartTable.PartEnum = 47 ... purchasedParts.GarageId = <garage id> ...
  ... CASE WHEN PartTable.PartId = <installed part id> THEN '1' ELSE '0' END ...
```

The game does not know where our choice is stored, so it marks the wrong row (or none). In the hook, we recognise the statement (`SELECT PartTable.PartId AS id` plus `WHERE PartTable.PartEnum = <our enum>`), read the garage id out of it, look up our stored choice for that car, and replace the number after `WHEN PartTable.PartId = ` so the right row is marked installed. (`-1` may come there when "nothing is installed"; the parser accepts it.)

## 5. Step 4: buying and installing

The game's upgrade code has fixed storage for the parts it knows. For a part type it does **not** know, it falls back to its *motor part* handling, which is why:

- the **price** is read with `SELECT Price FROM List_UpgradeMotorParts WHERE Id = <part id>`. Without a row there, the purchase silently does not go through. That is why step 1 inserts price rows into `List_UpgradeMotorParts`. Use a `MotorID` that belongs to no motor (we use a negative number) so nothing else ever sees these rows.
- installing a part writes `UPDATE <career garage table> SET MotorParts = <part id> WHERE Id = <garage id>`. We **swallow** that statement: the hook parses the part id and the garage id out of it (and so learns which option the player picked for which car), stores the choice itself (step 7), and runs a harmless rewrite instead, `SET MotorParts = MotorParts`. The game's own save data is not changed by it.
- before installing, the game checks whether the part is already bought: `SELECT 1 From Profile0_Career_PurchasedParts WHERE GarageId = g AND PartEnum = 47 AND PartId = 900470 ...`. For our **stock** option the answer must always be "yes", so the hook returns `SELECT 1`. If it said no, the game would look up a part object that does not exist for our type and **crash** (it dereferences null in `UpgradeBasket::InstallPart`). That was the one crash we found.

Purchases of non-stock options go into the save's purchased-parts table through the game's normal path, so the game itself remembers what you own (and does not charge twice).

## 6. Step 5: names and descriptions (string references)

The game takes texts from string tables, and a database column holds a reference of the form `_&<N>` where `N = (hash16 << 16) | key`. New keys cannot be added to the game's tables without replacing them. Instead:

1. Use a hash16 that belongs to no real table (we use `0x7E57`). The game then falls back to a tiny table that has none of our keys, and the lookup returns an empty string.
2. Hook the game's **string lookup** function `(manager, table, key) -> UTF-16 text`. Call the original; if the key is in our range and the original returned nothing, return our own static UTF-16 buffer. Everything else passes through untouched.

The references are built like this:

```c
// reference text for the database:  _&<decimal of (0x7E57 << 16) | key>
```

Keep the keys in a range of your own (see the registry) and the texts short (we use at most 99 characters).

## 7. Step 6: icons

Covered in [ICON_GUIDE.md](ICON_GUIDE.md): the PNGs are converted to the game's swatchbin format, carried inside the DLL, and handed to the game with `set_file` under `Upgrade_Parts.zip\Custom\`, and the `IconPath` / `ImagePath` columns above point at them (with a stock icon as a fallback when a slot has no picture).

## 8. Step 7: remembering the choice per car

Because we swallow the game's write (step 4), the mod keeps the choice itself, in its ini, **per garage id** (one number per owned car):

```ini
[GarageNitro]
30001=2     ; garage id = stage (0..3)
[GarageCar]
30001=2049   ; garage id = car ordinal
[Upgrade]
Area=1       ; 1 Engine (default), 4 Tires and rims, 8 Conversion
Price1=3000
Price2=6000
Price3=12000
Boost1=1.2   ; 1.0 .. 3.0
Boost2=1.4
Boost3=1.6
```

The garage id of the car being driven comes from the game's own getter for the save value `Main/CareerCar` (see the registry of hooks). The getter is hooked and its return value recorded; it is **never called from the scene/update thread**, only where the game itself reads its save data (an earlier version did, and we suspect it contributed to a freeze in the pause menu). The car ordinal is stored next to the choice so that a stale entry is ignored if a garage id ever points at another model.

## 9. Step 8: using the choice in the mod

The rest of the mod reads one number, `nu_level()`: 0 = no nitrous on the car being driven, 1 to 3 = the stage. It is refreshed whenever the garage id of the driven car changes or the player installs a stage, and it decides whether nitro works at all, how strong the push is (the stage's boost multiplier from the ini) and whether the NOS gauge is shown (the gauge frame is hidden on cars without a kit).

The area the type appears in (`Area=` in the ini) and the prices and boost factors are plain ini values, written with their defaults the first time so they can be edited; the SQL for the rows is built from them at startup, so a changed price needs a game restart.

## 10. Hooks and addresses used

All RVAs are for `forza_durango_release_final.exe` (Anthem 1.0.0.64). `code_matches()` compares the first bytes of the function with the expected ones before `hook_function` is called (a jump written by another mod at the start still counts as the game's code); on a mismatch the part is skipped and the log says so.

| What | RVA | Signature / meaning |
|---|---|---|
| Database object (global) | `0x034B3640` | pointer to the DB object; **virtual slot 8** (vtable offset `0x40`) is the query function `(self, out, const char *sql)`. We read the vtable entry at runtime and hook that function. |
| String lookup | `0x0123D780` | `(manager, table, key) -> const uint16_t *` UTF-16 text |
| Garage id getter | `0x002DCAB0` | `(holder) -> garage id` of the current car (save value `Main/CareerCar`), reached from the profile root `0x034B3E18` through its virtual slots `0x48` then `0xD8` |
| Profile root (global) | `0x034B3E18` | object whose virtual `+0x48` then `+0xD8` give the save-data holder |

Wait for the game's code to be ready first: `game = api->wait_for_code(0xFFFFFFFF)`, and the DB object exists a moment after startup, so poll for it (we poll every 500 ms for up to 10 minutes).

## 11. ID registry (please avoid these)

If you add your own types, pick numbers outside these ranges, so mods do not collide:

| Thing | Universal Wheel Spacers | NitroSystem II |
|---|---|---|
| `PartEnum` | 45 (front), 46 (rear) | 47 |
| `UpgradeTypes.id` | 7450, 7451 | 7452 |
| `Upgrades.id` | 74500..74505 (front), 74510..74515 (rear) | 74520..74523 |
| part id (`PartId`, `List_UpgradeMotorParts.Id`) | 900450..900455, 900460..900465 | 900470..900473 |
| `MotorID` of the price rows | -7450 | -7452 |
| string keys (hash16 `0x7E57`) | `0xA470`..`0xA4AF` | `0xA4C0`..`0xA4DF` |

The game's own part enums run from 0 to 44 in this build. Use 48 and up for new ones.

## 12. Pitfalls we hit

- **No price row, no purchase.** Unknown part types are priced from `List_UpgradeMotorParts` by part id. A missing row makes the purchase fail without any message.
- **Stock option crash.** The game asks whether the stock entry is "bought"; answering no makes it look up a part object that does not exist and crash. Answer yes for your stock entries.
- **A type needs two rows in the per-car view** (see the `HAVING COUNT(*) > 1` above): the stock row plus at least one option.
- **Statements get long.** The rewritten `CREATE VIEW` is much longer than the original; allocate the output buffer with generous room (we use `len + 4096`) and fail by returning the original statement, never by truncating.
- **Do not leave stale buffers.** The query function takes a `const char *`; the rewritten statement must stay alive until the game has run it. We keep a small ring of buffers.
- **Hooks of two mods on the same function.** Both mods here hook the same query and lookup functions. Accept code that already starts with another mod's jump (the platform checks the real bytes), and always call the original so the chain keeps working. Use different string key ranges and part ids.
- **Threads.** Only read the game's objects from the thread the game itself uses for the same data (the database thread). Reading the garage id from the scene update thread is a race with the pause menu.
- **Camera and other native logic keyed on the part enum** know nothing about your enum (see section 9).

## 13. Testing

- The SQL transformations are pure functions on `char *` in and `char *` out, so they are tested on any machine without the game (`tests/`, built with plain gcc/g++ and sanitizers): feed a recorded statement, compare the output. The tests also run the whole mod against a **fake platform** (mock `add_sql`, `hook_function`, `set_file`, a fake database object in fake game memory), checking the hook sites, the byte checks and the logs.
- In the game, the mod logs the first few statements of each kind it rewrites (`fh2-mods\platform.log`), for example the part list and the installed id. If a statement is not recognised, the log line says `NOT recognised` and shows the text; the game's SQL is the ground truth, record it from there.
- Run the tests: `sh tests/run_tests.sh`.

## 14. A minimal example

A new type `Foo` in Tires and rims with two options (stock and +1), numbers picked from outside the registry:

```c
#define FOO_ENUM    48
#define FOO_TYPE    7460
#define FOO_STOCK   900480L           /* part ids */
#define FOO_ONE     900481L

/* init, once */
api->add_sql(mod,
  "INSERT OR REPLACE INTO UpgradeTypes (id, PartEnum, AreaId, IsException, DisplayOrder, EverymanUpgradeDisplayOrder,"
  " Name, Description, IconPath, ImagePath) VALUES (7460, 48, 4, 0, 20, -1, '_&<name ref>', '_&<desc ref>', '<icon>', '<large icon>');"
  "INSERT OR REPLACE INTO Upgrades (id, TypeId, IsException, Name, Level, Description, IconPath, ImagePath)"
  " VALUES (74600, '7460', 0, '_&<ref>', 0, '_&<ref>', '<icon>', '<large icon>');"
  "INSERT OR REPLACE INTO Upgrades (id, TypeId, IsException, Name, Level, Description, IconPath, ImagePath)"
  " VALUES (74601, '7460', 0, '_&<ref>', 1, '_&<ref>', '<icon>', '<large icon>');"
  "INSERT OR REPLACE INTO List_UpgradeMotorParts (Id, MotorID, Level, IsStock, ManufacturerID, MassDiff, WeightDistDiff, Price, TorqueScale)"
  " VALUES (900480, -7460, 0, 1, 0, 0.0, 0.0, 0, 1.0), (900481, -7460, 1, 0, 0, 0.0, 0.0, 100, 1.0);");

/* in the query hook, for  CREATE VIEW ... View_UpgradeParts AS SELECT 0 AS PartEnum ... WHERE Ordinal = N :
   insert before the first " UNION SELECT " :
     " UNION SELECT 48 AS PartEnum, 900480 AS PartId, N AS Ordinal, 1 AS IsStock, 0 AS Level, NULL AS ManufacturerID, 0 AS Price, 0 AS IsException"
     " UNION SELECT 48 AS PartEnum, 900481 AS PartId, N AS Ordinal, 0 AS IsStock, 1 AS Level, NULL AS ManufacturerID, 100 AS Price, 0 AS IsException"
   for  SELECT PartTable.PartId AS id ... WHERE PartTable.PartEnum = 48 ... :  replace the id after "WHEN PartTable.PartId = "
   for  UPDATE ... SET MotorParts = 900481 WHERE Id = <garage> :  read the id and the garage, store them, run SET MotorParts = MotorParts
   for  SELECT 1 From Profile0_Career_PurchasedParts ... PartEnum = 48 AND PartId = 900480 ... :  return "SELECT 1"
   and hook the string lookup for the '_&' references */
```

The working version of every one of these transformations is in `src/nu_sql.h`; copy the functions and change the constants.
