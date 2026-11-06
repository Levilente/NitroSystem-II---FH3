# Changelog

## 1.0.0

First release for **Forza Horizon 3**, on the [FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform) 1.0.0 (API 12). A port of NitroSystem II for FH2 (2.0.0), itself a continuation of [qiangqiang101's NitroSystem](https://github.com/qiangqiang101/NitroSystem).

- Nitro is a garage upgrade: **Garage > Upgrades > Engine > Nitrous Kit** (Stock, Stage 1, Stage 2, Stage 3), bought per owned car, through the platform's upgrade bridge. No kit, no nitro.
- Stage prices (3 000 / 6 000 / 12 000 CR) and boosts (1.2x / 1.4x / 1.6x) are configurable in `fh3-mods\NitroSystemII.ini`.
- Push, buildup, tank and lockout; airborne cut; only the player's car.
- Exhaust flames through the game's own backfire effect; Backfire on All Cars.
- NOS gauge on the analog and digital speedometers, hidden on cars without a kit; two HUD themes.
- Camera shake, wider field of view, blue vignette and speed lines (each configurable), boost and stop sounds, controller rumble.
- Refill on race start / car change / fast travel, refund on rewind.
- No nitro in a car that is not the garage car the kit belongs to (mission loaners).
- A car chosen at an event's car select has its nitro from the start (it was taken for a loaner until the game saved it as the driven car; needs platform API 14).
- Nitro works only while you are driving (driving camera), not in the garage, menus or photo mode, and does not depend on the HUD being visible.
- The HUD is ready from the first frame of a fresh start.
- Settings on **Pause > MODS**.
- Update checks through the platform.
