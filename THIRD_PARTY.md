# Código y recursos de terceros

| Qué | Dónde | Licencia | Origen |
|---|---|---|---|
| ooz (descompresor Oodle Kraken) | `native/third_party/ooz/` | GPL-3.0-or-later ([COPYING](native/third_party/ooz/COPYING)) | https://github.com/powzix/ooz, commit `05038060` |
| bc_decoder (texturas BC1–BC7) | `native/third_party/bc_decoder/` | MPL-2.0 | Skyline Team (https://github.com/skyline-emu/) |
| Cabeceras ABI de módulos de Eden Duo | `native/include/` | GPL-3.0-or-later | Eden Duo (`src/core/mods/dsmod_module_abi.h`, `dsmod_module_extensions.h`) |
| Empaquetador `.dsmod.zip` | `tools/third_party/build_dualscreen_package.py` | GPL-3.0-or-later | eden-duo-companions (`tools/build_dualscreen_package.py`) |
| Fuente Barlow Condensed (atlas generado) | `art/barlow.mfnt`, `art/barlow.png` | SIL OFL 1.1 ([licencia](art/LICENSE-Barlow-OFL.txt)) | https://github.com/jpt/barlow |

El arte de la interfaz (`art/`, `art_v/` y su tema claro en `l/`) está dibujado por código en `tools/make_art.py`.

**No se incluye nada del juego.** Nombres, textos, datos de especies y movimientos, iconos de
objetos y Pokémon y el mapa se leen en tiempo de ejecución del romfs del propio jugador.
