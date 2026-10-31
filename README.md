# NitroSystem II for Forza Horizon 2

A **Fast & Furious style nitro** for **Forza Horizon 2** (Xbox One build on PC, via the Azimuth launcher), built on the **FH2 Mod Platform** — and in NitroSystem II you have to **buy it**: nitrous is a real garage upgrade, with three stages, fitted per car.

Hold **LB** (or any button you pick) to burn nitro: your car gets a strong push, blue flames shoot from the tailpipes, the speedometer gets a draining NOS gauge, the camera shakes, the field of view widens, a blue vignette pulses at the screen edges, speed lines streak past, the nitro roars and the controller rumbles.

![Nitrous Kit in the garage](docs/images/nitrous-kit-menu.jpg)
![Stage 1 Nitrous, owned](docs/images/nitrous-kit-stage1.jpg)
![Nitro in action](docs/images/nitro-in-game.jpg)

## What is new compared to the original

NitroSystem II is a continuation of **[NitroSystem](https://github.com/qiangqiang101/NitroSystem)** by qiangqiang101 (the mod calls itself *FnFNitro* internally, which is why its DLL and ini are named that; see [Credits](#credits-and-thanks)). What this version adds:

- **The Nitrous Kit is an upgrade you buy.** **Garage > Upgrades > Custom upgrade > Engine > Nitrous Kit**: *Stock* (no nitrous), *Stage 1*, *Stage 2*, *Stage 3*. Prices default to 3 000 / 6 000 / 12 000 CR and are configurable.
- **Stored per owned car.** Fitting stage 2 on one car does not give nitro to your other cars, even to other cars of the same model. No kit, no nitro.
- **Each stage pushes harder**: 1.2x / 1.4x / 1.6x boost by default (configurable per stage, 1.0x to 3.0x).
- **The NOS gauge is only shown on cars that have the kit** (the empty frame is hidden, optional setting).
- **Custom upgrade icons** in the game's own style, shipped inside the DLL ([docs/ICON_GUIDE.md](docs/ICON_GUIDE.md)).
- **Extra effects while nitro burns**: wider field of view, a pulsing blue vignette and NFS Unbound style speed lines along the screen edges (each can be switched off).
- Shorter tank (5 seconds by default, configurable) to suit nitro as a purchasable boost.

## Features

- **Nitro on LB, or any button you choose.** Any connected controller; remap it to another button, d-pad direction or trigger (see [Nitro Button](#settings)). There is no keyboard key.
- **A real tank.** Nitro drains while you hold the button and recharges when you let go. Run it dry and it locks out until you release the button and have recharged about 2 seconds.
- **Buildup curve.** The push ramps up over a short time instead of snapping on.
- **Only your car.** AI racers and traffic are never boosted.
- **Stays on the road.** The push follows the horizontal direction of the car's nose, so a pitched-up nose does not launch you into the sky, and nothing is applied while the car is airborne (or for a moment after landing).
- **Only while driving.** Nitro works only while the game simulates your car and the speedometer HUD runs, so it stays off in the pause menu, the map and loading screens. The tank does not drain or recharge while the game is paused.
- **NOS gauge on the speedometer.** The **analog** and the **digital** speedometers both get a four-segment NOS bar that drains as you use nitro, with a glowing tip. Two HUD themes: the original yellow bar, or the Fast & Furious cyan HUD.
- **Exhaust flames.** Blue flames come out of your car's real tailpipes while nitro is held (the mod drives the game's own backfire effect). Which tailpipes your car has is learned from the game's own backfires, so flames start after the first backfire of a drive or after you switch cars, normally within a few seconds.
- **Sound, rumble, camera shake.** A boost sound that kicks in and holds for as long as the tank lasts, and a stop sound (built into the DLL; put your own `nos_boost.wav` and `nos_stop.wav` into a `NitroSystemII` folder next to `azimuth.exe` to replace them), a rumble that ramps up with the boost, and a camera shake in the driver, hood, bumper and chase views.
- **Refill and rewind.** The tank refills when a race starts, you quit one, you change car or you fast travel, and rewinding gives back the nitro you used since the rewind point (both can be switched off).
- **In-game settings** on **Pause > Settings > MODS**.

## Requirements

| What | Version |
|---|---|
| Forza Horizon 2 on PC with the **Azimuth** launcher | game build 1.0.0.64 (the one the platform targets) |
| **[FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform)** | **2.8.0 or newer** (tested with 2.8.0, API 9). The code only needs API 3 (platform 2.2.0) for the upgrade and API 4/5 (2.3.0 / 2.4.0) for the MODS page and its sliders, but nothing older than 2.8.0 has been tested, and the platform shows a notice on the title screen if yours is older than 2.8.0. |

The platform is the `DWrite.dll` (and `d3dcompiler_47.dll`) next to `azimuth.exe`. If you use any other platform-based mod you already have it; otherwise download `DWrite.dll` from the platform's [releases page](https://github.com/Trinculo54/fh2-mod-platform/releases) and read its README for the file your Azimuth version loads.

On platform versions before 2.7.3, the files this mod hands to the game (HUD textures, flame effect, icons, the database copy with the upgrade) are only read if **[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions)** (which makes the game load `mediaoverride`) is installed. From 2.7.3 the platform does that itself.

## Installation

1. Close the game.
2. Download `NitroSystemII.dll` from the [latest release](../../releases/latest).
3. Copy it to `<your Azimuth folder>\fh2-mods\` (the folder next to `azimuth.exe`; it exists if the platform is installed).
4. Start the game through Azimuth. Open a car in the garage: **Upgrades > Custom upgrade > Engine > Nitrous Kit**, buy a stage, go driving and hold **LB**.

> [!IMPORTANT]
> `NitroSystemII.dll` goes into **`fh2-mods`**, the platform's folder. It is **not** an Extra Options mod: do not put it into Extra Options' `mods` folder, it would not be loaded.

To check that it loaded, open `fh2-mods\platform.log` and look for lines of `NitroSystem II`, for example `NitroSystem II initializing ...`, `upgrade icons handed to the game: 04/04`, and later `Player car located` and `Vertical axis learned`, which mean the mod found your car.

### Updating from NitroSystem / FnFNitro 1.x

- **Delete `FnFNitro.dll`** from `fh2-mods`. Two nitro mods must not run together.
- To keep your settings and the stages already fitted to your cars, rename `fh2-mods\FnFNitro.ini` to `NitroSystemII.ini` (the file is named after the mod). If you used your own sounds, rename the `FnFNitro` folder next to `azimuth.exe` to `NitroSystemII`.
- From versions 1.2 and older: those had you copy HUD files into `Mount\mediaoverride` by hand. NitroSystem II removes the old copies it recognises (only files identical to the old release, byte for byte) the first time it starts, and logs `Removed N old HUD/flame file(s)`.

## Settings

Everything is on **Pause > Settings > MODS**; the values are saved to `fh2-mods\NitroSystemII.ini`.

| Setting | Range | Default | What it does |
|---|---|---|---|
| Nitro System | On / Off | On | Turns nitro on or off. A Nitrous Kit is still needed on the car. |
| Nitro Button | 1 - 16 | 1 (LB) | Which button burns nitro (numbers below). Applies right away. |
| Nitro Debug Overlay | On / Off | Off | Draws debug info over the game (borderless or windowed mode only). |
| HUD Theme | 1 - 2 | 2 | 1 is the original HUD with a yellow NOS bar, 2 the Fast & Furious HUD (cyan NOS bar, gears and ABS/TCS/STM icons). **Needs a game restart.** |
| Hide Nitro Gauge Without Kit | On / Off | On | Hides the NOS gauge frame on cars with no Nitrous Kit. **Needs a game restart.** |
| Nitro Tank Capacity (sec) | 5 - 60 | 5 | Seconds of nitro in a full tank. |
| Nitro Buildup Time (sec) | 0.1 - 2.0 | 0.5 | How long the push takes to ramp up. |
| Nitro Recharge Rate | 0.0 - 1.0 | 0.2 | Seconds of nitro restored per second while not boosting. |
| Nitro Sound Volume | 0 - 100 | 50 | Volume of the boost and stop sounds (the game's own volume sliders do not change them). |
| Nitro Camera Shake | 0 - 100 | 65 | Camera shake while nitro burns. 0 turns it off. |
| Nitro Wider FOV | 0 - 30 | 10 | How much wider the field of view gets while nitro burns. 0 turns it off. |
| Nitro Blue Vignette | On / Off | On | A pulsing blue vignette at the screen edges while nitro burns. |
| Nitro Speed Lines | On / Off | On | Thin blue streaks along the screen edges, in step with the car's speed. |
| Backfire on All Cars | On / Off | On | Lets every car backfire, which the exhaust flames need. **Needs a game restart.** |
| Refill Nitro on Race Start | On / Off | On | Refills the tank at race start, quitting a race, changing car or fast travel. |
| Refund Nitro on Rewind | On / Off | On | Rewinding gives back the nitro used since the rewind point. |
| Nitro Controller Rumble | On / Off | On | Rumbles the controller while nitro burns. |

The upgrade itself is configured in the ini only (written with its defaults the first time, then editable; a game restart applies it):

```ini
[Upgrade]
Area=1        ; where the Nitrous Kit is listed: 1 Engine (default), 4 Tires and rims, 8 Conversion
Price1=3000   ; price of stage 1 / 2 / 3, in CR
Price2=6000
Price3=12000
Boost1=1.2    ; push of stage 1 / 2 / 3 (1.0 - 3.0)
Boost2=1.4
Boost3=1.6
```

`[GarageNitro]` and `[GarageCar]` in the same file hold the stage fitted to each owned car (`<garage id>=<stage>`); the mod writes them.

**Nitro Button numbers** (the Mods page can only show numbers; in the ini you write the name, for example `NitroButton=LB`):

| # | Name | Button | # | Name | Button |
|---|---|---|---|---|---|
| 1 | `LB` | Left bumper | 9 | `DPadUp` | D-pad up |
| 2 | `RB` | Right bumper | 10 | `DPadDown` | D-pad down |
| 3 | `A` | A | 11 | `DPadLeft` | D-pad left |
| 4 | `B` | B | 12 | `DPadRight` | D-pad right |
| 5 | `X` | X | 13 | `Back` | Back / View |
| 6 | `Y` | Y | 14 | `Start` | Start / Menu |
| 7 | `LS` | Left stick click | 15 | `LT` | Left trigger |
| 8 | `RS` | Right stick click | 16 | `RT` | Right trigger |

Pick a button the game does not already use while driving, or it will do both at once.

Notes on the ini: `HudTheme` is `FF` (Fast & Furious, default) or `Original`. `BackfireOnAllCars=1` makes the mod change the game database at startup so every car can backfire, which the flames depend on; turn it off to leave the game's own backfire settings alone (flames then only work on cars the game already lets backfire).

### Debug overlay

**Nitro Debug Overlay** draws a small panel in the top-left corner: nitro state and fuel, whether the mod thinks you are driving, whether your car was found, the learned exhaust tips and the flame counters. It is a transparent window laid over the game, like an FPS counter, so the game must run in borderless or windowed mode.

## Uninstalling

Close the game first. Do not remove anything else from `mediaoverride`: the Mods page screens, string tables, car zips and `DefaultCache.bin` there belong to the platform and your other mods.

To switch the mod off without uninstalling it, open **Pause > Settings > MODS** and turn **Nitro System** off.

1. Delete `fh2-mods\NitroSystemII.dll`, `fh2-mods\NitroSystemII.ini` (your settings and the fitted stages) and the `NitroSystemII` folder next to `azimuth.exe` if you made one for your own sounds.
2. Start the game once. The files the mod handed to the game (the NOS speedometer, the flame effect, the icons, the database copy with the upgrade) are deleted by the platform shortly after startup once no installed mod uses them, and the speedometer, flames and backfires are back to stock.

Nitrous Kits you bought are recorded in your save as an extra part type (enum 47) that only this mod understands; without the mod those cars simply have no nitro. This has not been tested at length, so keep a backup of your save if you care about it.

## Known limitations

- **Flames need a first backfire.** After you start driving or switch cars, there are no nitro flames until the game has fired one of its own backfires for that car.
- Only the player's car gets nitro.
- The speedometer's NOS gauge replaces the game's HUD speedometer file while the mod is active (through the platform's override mechanism, nothing is changed on disk), so another mod that replaces the same HUD file will conflict; the platform logs it when two mods claim the same file.

## Building from source

You need [Zig 0.15.2](https://ziglang.org/download/) (used as a C++ cross compiler; nothing else).

```bat
BUILD_WINDOWS.cmd
```

(or drop `zig.exe` onto the file) produces `dist\NitroSystemII.dll`. The HUD files, the flame effect, the sounds and the icons are compiled into the DLL from the generated `src\hud_data.h` and `src\icon_data.h`, so there is no resource compiler and no other file to ship.

- `scripts/gen_hud_data.py` rebuilds `src/hud_data.h` from `hud/`, `particles/` and `sounds/` (listed in `src/hud.rc`).
- `scripts/gen_icons.py --prefix nitro` rebuilds `src/icon_data.h` from the PNGs in `icons/` (see [docs/ICON_GUIDE.md](docs/ICON_GUIDE.md)).
- `scripts/gen_speedlines.py` regenerates the speed lines in `hud/HUDSpeedometerControl.xaml`.
- Host tests (g++, any OS): `sh tests/run_tests.sh`.

How the upgrade menu entry is added without touching any game file is written up in **[docs/RUNTIME_DB_GUIDE.md](docs/RUNTIME_DB_GUIDE.md)**.

## Credits and thanks

See [CREDITS.md](CREDITS.md) for the full list. In short:

- **[qiangqiang101](https://github.com/qiangqiang101)**, author of **[NitroSystem](https://github.com/qiangqiang101/NitroSystem)** (internally *FnFNitro*), the Forza Horizon 2 nitro mod on the FH2 Mod Platform that NitroSystem II is built on: boost, flames, HUD gauge, sounds and settings page all come from it (MIT).
- **[Trinculo54](https://github.com/Trinculo54)**, for the **[FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform)** (MIT).
- **[RokkuDayo](https://github.com/RokkuDayo)**, for **[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions)**.
- **Levilente**, the Nitrous Kit upgrade, per-car stages, HUD hiding, field-of-view, vignette and speed-line effects, custom icons and the documentation.

## License

**MIT**, see [LICENSE](LICENSE) (the copyright notice of the upstream NitroSystem licence, which names Bartholomew Ho, is kept; modifications are by Levilente). `include/fh2mod.h` is the platform's header, MIT (Trinculo54), see [LICENSE-fh2mod-MIT.txt](LICENSE-fh2mod-MIT.txt). The HUD files, particle file and sounds are covered in [CREDITS.md](CREDITS.md).

This is an unofficial fan modification. Forza Horizon 2 is a trademark of Microsoft / Playground Games.
