# Credits

**NitroSystem II** is made by **Levilente**, and stands on the work of several people. Thank you.

| Who | What | Where it is used | Licence |
|---|---|---|---|
| **[qiangqiang101](https://github.com/qiangqiang101)** | **[NitroSystem](https://github.com/qiangqiang101/NitroSystem)** (internally called *FnFNitro*), the Forza Horizon 2 nitro mod on the FH2 Mod Platform that NitroSystem II is built on: the boost physics, tank, flames, HUD gauge, sounds, settings page and most of `src/nitro_mod.cpp` | the code base this repository started from, and the name | MIT, see [LICENSE](LICENSE) (the upstream copyright notice is kept) |
| **Bartholomew Ho** | Named as the copyright holder in the upstream NitroSystem `LICENSE` | the copyright line in [LICENSE](LICENSE) is kept as in upstream | MIT |
| **[Trinculo54](https://github.com/Trinculo54)** | **[FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform)**, the loader and API everything runs on | `include/fh2mod.h` is the platform's public header, as published by the platform | MIT, [LICENSE-fh2mod-MIT.txt](LICENSE-fh2mod-MIT.txt) |
| **[RokkuDayo](https://github.com/RokkuDayo)** | **[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions)**, which made modding the game's media possible | background / compatibility (on platform versions before 2.7.3 it provides the `mediaoverride` loading this mod needs) | see its repository |
| **Levilente** | The Nitrous Kit garage upgrade (database rows, per-car view, installed marker, install handling, texts, per-car stages, configurable prices and boosts), hiding the NOS gauge without a kit, the wider field of view, blue vignette and speed lines, custom icons, the PNG to `.swatchbin` converter and the documentation | `src/nu_sql.h`, `src/nu_upgrade.h`, `icons/`, `scripts/icontools/`, `docs/` and the changes in `src/nitro_mod.cpp` | MIT |

The upgrade-menu technique was developed first in the sibling mod **Universal Wheel Spacers** (same author), and is written up in [docs/RUNTIME_DB_GUIDE.md](docs/RUNTIME_DB_GUIDE.md).

## Assets carried over from the upstream project

These files come from the upstream NitroSystem project and are included only so the DLL can be rebuilt from this repository:

- `hud/` (`HUDSpeedometerControl.xaml` and the HUD textures): modified versions of Forza Horizon 2's own speedometer interface file and recolored HUD textures, with the NOS gauge added;
- `particles/Backfire.xml`: a modified version of the game's own backfire particle effect;
- `sounds/*.wav`: the nitro sounds. Their origin is documented in the upstream project, not here.

The original game assets remain the property of Microsoft / Playground Games and their licensors; their modified forms are included for interoperability with the game only and are **not** covered by this repository's MIT licence. If you are a rights holder and want any of them removed, please open an issue.

## Trademarks

Forza Horizon 2 is a trademark of Microsoft Corporation / Playground Games. This project is an unofficial fan modification, not affiliated with or endorsed by them. It does not modify the game's files on disk.
