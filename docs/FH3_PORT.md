# NitroSystem II on Forza Horizon 3

Built against the FH3 Mod Platform (`include/fh3mod.h`), for the FH3 Xbox One build 1.0.125.0 (exe of 5 Jun 2018) run through Azimuth.
The DLL goes into `fh3-mods` next to `azimuth.exe`; its settings file is `fh3-mods\NitroSystemII.ini`.

## Phase 1 (this build): the push
| | FH2 | FH3 |
|---|---|---|
| CCarDynamics per-tick update (vtable slot 16) | 0x12C1EA0 `(this, ?, ?, ?, float dt)` | 0x1D1E300 `(this, ?, float dt)` |
| position / forward / velocity | +0x10 / +0x40 / +0x50 | +0x30 / +0x90 / +0x10 (speed +0x12D4) |
| CWorldCollObjMan::Update, player position | 0x384C00, mgr+0x4F0 | 0x425D40, mgr+0x520 |

Found by lining up both exes: CCarDynamics' secondary vtable (FH2 381 slots at +0x3A0, FH3 474 at +0x4B0) has the same run of
getters/setters (GetPosition, SetPosition, GetVelocity, SetVelocity + speed, ...), 30 slots further on in FH3; the world manager update
starts with the same code (`[rcx+0x270]`/`[rcx+0x540]` in FH3 vs `0x260`/`0x510`).


## Status (1.0.0)
| Feature | FH3 |
|---|---|
| Push, buildup, tank, lockout (no 33% lock), airborne cut | done |
| Nitrous Kit upgrade, per owned car (garage id) | done, see below |
| Exhaust flames | done (fh3.35) |
| Camera shake, wider FOV | done |
| NOS gauge, analog (one arc + glowing tip) and digital (4 segments) | done; hidden on cars without a kit |
| HUD theme (`HudTheme=FF` cyan / `Original` yellow) | done (bar colour only; FH3 keeps its own gear / assist icons) |
| Blue vignette, speed lines | done (speed lines: 3 tiers instead of 6, the free HUD items) |
| Refill on race start, refund on rewind, rumble, sounds | shared code, as FH2 |
| Backfire on all cars (`UPDATE BackfireLevels`) | done |
| MODS page | done (platform 1.0.0), settings also in `fh3-mods\NitroSystemII.ini` |

Diagnostics: `[FH3] ResearchLog=1` brings back the SQL / ownership log and the 2-second "FH3 check" lines.
`[FH3] ForceStage=1..3` gives every car that stage (testing; 0 = the shop decides).

## Nitrous Kit on FH3
> **Historical (fh3.53 to fh3.82).** Since 1.0.0 the kit is registered through the platform's upgrade bridge (API 11, see the platform's `docs/api.md` and `docs/fh3-game-notes.md`); the mod no longer touches the shop, the SQL or the purchase flow. The notes below describe how the game's upgrade shop works and why the first approach was replaced.
- FH3 maps a part enum to a garage column through a table in the exe (32-byte entries: list table, column, ...).
  Enums 0..44 are all taken; 47 (the FH2 choice) is past the table, so the shop shows it but A does nothing.
  44 is the aspiration conversion page (its own query on `List_Aspiration`). The kit uses **26, the Quad Turbo slot**:
  no `UpgradeTypes` row in FH3 uses it (quad turbos come through the aspiration page).
- Rows: `UpgradeTypes` 7452 (enum 26, area 1 Engine), `Upgrades` 74520..74523, view rows PartId 900470..900473
  joined into `Profile0_View_UpgradeParts`. No rows in `List_UpgradeEngineTurboQuad`: a real row with a made-up
  engine crashed the car load (game+0x19FA6EF).
- Install: the game writes the whole garage row (`QuadTurbo=<id>, ..., AspirationTypeId=4`) or `SET QuadTurbo = <id>`.
  The mod reads the stage from the id, stores it (`[GarageNitro]`), writes `QuadTurbo=-1` and keeps the car's aspiration.
  `SET AspirationTypeId=4` right after a kit install is rewritten too. A save that still has one of our ids is cleaned
  before the garage table is first read (through the game's `sqlite3_exec`, game+0x421D0).
- Questions answered by the mod: the per-engine level query (`SELECT Level ... EngineId=`), the manufacturer (-1).
- Ownership: the game forgets a part once another one of its type is installed. The mod keeps the bought stages per car
  (`[GarageOwned]`, bit n = stage n) and inserts the matching `Profile0_Career_PurchasedParts` rows (manufacturer -1,
  price 0) before the part list or an ownership check, so stock and bought stages show as owned. Answering the stock
  check with a bare `SELECT 1` crashed the setup apply (game+0x382149): a real row is needed.

## Exhaust flames (research)
- CCarPresentationDeferred (vt 0x468B058) slot 93 = 0x1DFD9C0(deferred, tips[0x280]): queues the backfire to the presentation thread (handler 0x1DC9D60).
- CarPresentation (vt 0x468E848 / 0x46902E0) slot 93 = 0x1E741B0(pres, tips): copies to pres+0x1090 (8 x 0x50, fire byte +0x40), spawns "BackFireInstance%d"/"car_backfire" where pres+0xF08+0x10*N is empty; timer object pres+0x1520.
- The mod posts its own backfire through the deferred slot on the game's posting thread while nitro burns.

## Exhaust flames (done, fh3.35)
The game builds its backfire message in `Car` (vtable game+0x462D160 slot 19, game+0x18B8D20): eight weak pointers to
CarLocators at Car+0x88 (obj, ctrl block; use count at ctrl+8), matrix from the locator's vtable slot 2, posted to the
presentation at Car+0x73A0; Car+0x71E8 == CCarDynamics+0x4B0 identifies the player's Car. The locators are resolved lazily by
Car slot 78 (game+0x18BBE20), which the mod calls itself before the first backfire. No stack scans or locator-table searches.

## HUD (research, FH3)
- Speedometer XAML: UI.zip `Controls/HUDSpeedometerControl.xaml` (53 KB, MPH digits, gear, ABS/TCR/STM, `RPMGaugeNumbers[0..20]` with
  XPos/YPos, GaugeFillAmount ...). FH2's is a different file (hud/ is FH2's, can't be reused as is).
- HUDStatusSpeedometer vtable game+0x44A4318 (21 slots). XPos property getter game+0x11042F0, YPos getter game+0x1104400,
  RPMGaugeNumbers getter game+0x1102650. Still to find: the tick (rpm), the items array path, the property-changed raise function.
- Plan: feed the NOS value through unused RPMGaugeNumbers items' XPos/YPos like FH2, with an XAML patch injected at runtime.
