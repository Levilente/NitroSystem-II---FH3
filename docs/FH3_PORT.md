# NitroSystem II on Forza Horizon 3 (work in progress)

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
Every car gets the stage in `[FH3] Stage` (default 2). The log prints an "FH3 check" line every 5 s for two minutes to confirm the offsets.

## Still to port
Garage Nitrous Kit (database + upgrade UI), exhaust flames (CarPresentation11 backfire), camera shake / FOV, NOS gauge + HUD theme
(HUDStatusSpeedometer and FH3's own speedometer XAML), settings on the MODS page.

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
