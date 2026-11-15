# Third-party code and assets

| What | Where | Licence | Source |
|---|---|---|---|
| ooz (Oodle Kraken decompressor) | `native/third_party/ooz/` | GPL-3.0-or-later ([COPYING](native/third_party/ooz/COPYING)) | https://github.com/powzix/ooz, commit `05038060` |
| bc_decoder (BC1–BC7 textures) | `native/third_party/bc_decoder/` | MPL-2.0 | Skyline Team (https://github.com/skyline-emu/) |
| Eden Duo module ABI headers | `native/include/` | GPL-3.0-or-later | Eden Duo (`src/core/mods/dsmod_module_abi.h`, `dsmod_module_extensions.h`) |
| `.dsmod.zip` packager | `tools/third_party/build_dualscreen_package.py` | GPL-3.0-or-later | eden-duo-companions (`tools/build_dualscreen_package.py`) |
| Barlow Condensed font (generated atlas) | `art/barlow.mfnt`, `art/barlow.png` | SIL OFL 1.1 ([licence](art/LICENSE-Barlow-OFL.txt)) | https://github.com/jpt/barlow |

The interface art (`art/`, `art_v/` and their light theme in `l/`) is drawn by code in
`tools/make_art.py`.

**Nothing from the game is included.** Names, texts, species and move data, item and Pokémon
icons and the map are read at run time from the player's own romfs.
