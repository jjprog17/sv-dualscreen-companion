# How it works (technical notes)

*[En español](COMO-FUNCIONA.md)*

Everything comes from `native/sv_module.c`, a native Eden Duo module (ABI in `native/include/`)
that reads the game's memory and the player's romfs and publishes values (`sv.*`). The manifest
(`tools/gen_manifest.py` → `packages/*/dualscreen/manifest.json`) draws them.

## Save data

- Save blocks (SCBlock, XorShift32-encrypted): key table at
  `[main+0x47350D8, 0xD8, 0x0, 0x0, 0x30, 0x08]`.
- Keys used:
  - team `0x3AA1A9AD`;
  - money `0x4F35D0DD`;
  - LP `0xADB4FE17`;
  - MyStatus `0xE3E89BD1` (byte `0x07` is the save's language);
  - bag `0x21C9BD44`;
  - Pokédex `0xF5D7C0E2`;
  - location `0x19FC5B7B`;
  - region `0xF17EB014`;
  - coordinates `0x708D1511`;
  - play time `0xEDAFF794`;
  - Blueberry Points `0x66A33824`.
- Live team: `[main+0x4763C98, 0x08, 0x30+8*slot, 0x30]` → encrypted PK9.
  - Its order is the game's current one; the save block only changes when you save.
  - Bytes 0x148+ (level and stats) of the live object aren't always valid, so they are
    recalculated from experience, IVs, EVs and nature.
- Map state: `[main+0x473ADE0, 0x160, 0xE8, 0x28]` is 0x11 on the field. It also jumps to 4, 5,
  6, 18 and 19, so several reads are used to filter it.

## Game data (romfs)

The game files are read through the Trinity index (`arc/data.trpfd` / `data.trpfs`) and
decompressed with ooz:

- `personal_array` (types, base stats, growth…);
- texts (`monsname`, moves, items, Pokédex…) from `message/dat/<language>/common/`, in the save's
  language;
- BNTX icons for items and Pokémon (BC1–BC7, and ASTC through the host);
- the map.

The map isn't a single image. It is made of tiles in
`appli/tex/ymap_ter_small/ymap_ter_small_CC_RR/`:

- Paldea is 10×10 and each DLC is 4×4;
- CC is the column from west to east and RR is the row from south to north.

The module joins them into a 2048 px mosaic and crops it for the zoom. Scale (from RaidCrawler):

- Paldea: `u = x/5000`, `v = (z+5500)/5000`;
- Kitakami and Blueberry: 2000, with an offset of 2000.

## Interface language

- The setting `lang` (0 auto, 1 English, 2 Spanish) is a persistent Eden Duo flag.
- The manifest writes each widget with words twice, once in Spanish and once in English, and
  shows only the one for the chosen language. The derived values `ui.en` / `ui.es` decide which.
- In *auto*, the module publishes `sv.lang_en` from the save's language byte (7 = Spanish).
- The module also translates its own texts (status, advice, switch suggestions…). It reads the
  setting through the derived value `sv.ui.lang`, because some hosts only answer `@flag:` reads
  from a module's `sample()` with the fallback.

## The opponent

1. **Names on screen** (since 0.6.9):
   - The module sweeps ±192 MB around the live team, looking for species names in UTF-16 (the
     ones in `monsname`, in the save's language).
   - On the field it keeps each (address, name) pair. In battle, only the new ones count.
   - Your team's names are left out (they show which of yours is out), as are
     Koraidon/Miraidon.
   - The name that appears latest wins; on a tie, the one repeated most.
   - The level is the new "Lv. N" that isn't your Pokémon's.
   - When the battle ends, the addresses of the opponent's name are kept, and the next battle
     reads them directly.
2. **HP**: among the PK9s the sweep follows, the one losing HP is in battle.
3. **Field**: a field PK9 whose bytes change when the battle starts. This only counts if its
   experience is above 0.
4. **Learned paths**: a reverse pointer search towards the opponent (experimental).
5. **By hand**: a list of nearby Pokémon, or a search by name with an adjustable level.

What was tried and dropped, and why, is in the changelog (`CHANGELOG.md`).

## Position on the map

The saved position comes from the coordinates block (`PlayerSave_StartPosition`), which the game
rewrites often. For the live position, the module looks for copies of those coordinates in
memory (up to 384):

- Each copy that moves earns points for how many others agree with it.
- The copy with the most points is followed. The module only switches to another copy when that
  one clearly beats it.
- When the block changes and matches the followed copy, that copy is confirmed.
- If the block lands far away (more than 80 units), the distant copies are put in doubt and a new
  search starts, without dropping the current copy.
- A copy that stops being readable is held for 3 seconds.

The facing comes from that same copy's steps (every 4 units). A U-turn is only accepted after
two steps in that direction.

## Tests

`native/test_host.c` simulates the host and the game's memory. It covers:

- the save and the live team;
- battles: new, changed and Terastallized PK9s, trainers, names on screen and reused slots;
- the bag, the Pokédex and the card;
- the map: tiles, zoom and position;
- both languages.

Usage:

```sh
cd native && ./build.sh
clang -O1 -Iinclude -o out/test_host test_host.c -ldl
python3 ../tools/make_test_romfs.py /tmp/romfs <PKHeX>/PKHeX.Core <moves.csv>
SV_ROMFS=/tmp/romfs ./out/test_host out/linux-x86_64/0100A3D008C5C000.so C01884F294C053C4 \
  <team pk9>... --opp <opponent pk9>
```

To test an English game, build the romfs with `--english` and run with `SV_GAMELANG=2`. Some
checks compare Spanish texts, so they are expected to fail in that mode.
