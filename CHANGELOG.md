# Changelog

## 0.10.0
- **English and Spanish.** The interface follows the game's language: Spanish games get Spanish,
  every other language gets English. The new **Language / Idioma** setting in Eden Duo can force
  English or Spanish. Game words (Pokémon, moves, items, places) still come from the game's own
  files, in its language.
- Numbers follow the language too (`0.5` / `0,5`), and the date reads "Wednesday, 12 November" in
  English.
- The settings are labelled in both languages ("Theme / Tema").
- `tools/make_test_romfs.py --english` builds an English test romfs; `native/test_host` checks
  both languages.

## 0.9.0
- Light theme as well as the dark one: **CHANGE THEME** button on the Card page (or Eden Duo's
  settings). Eden Duo remembers the choice between sessions. Same design with a cream background,
  white panels and soft type boxes.

## 0.8.2
- Battle: guided search for the game's move-menu cursor (**CURSOR** button). Tap the number of the
  move the cursor is on, move it and repeat; once one position is left, the selected move is
  highlighted on the bottom screen. A first step towards hiding the move list on the top screen.

## 0.8.1
- Scarlet gets its own theme: wine-red background, golden Paldea tiles on the strips, the
  Academy's gold details and Tera crystals in battle.

## 0.8.0
- New look: dark background, slanted panels and tabs, Barlow Condensed (italic headings on recent
  Eden Duo builds) and each game's colour: red and orange in Scarlet, purple and lilac in Violet.
- Top bar on Team, Pokédex and Bag: place, money, battery and time.
- Team cards with a gem in the colour of the Pokémon's type, and its types in view.
- Battle page redone: the opponent in large print, a "Tip" strip (who moves first and the best
  switch), its base stats against yours and three boxes with colours that mean something: red for
  what hurts it, green for what it resists and blue for what has no effect.

## 0.7.5
- Map: no longer flips between "north up" and "south up", and the position no longer jumps. The
  game rewrites its saved position often, and each change used to drop the live copy and search
  again. Now it is kept when it agrees, searches only add copies, the choice changes slowly, a
  copy that fails for a moment is held, and the heading only comes from that copy's own steps (a
  U-turn needs two steps).
- Map diagnostics reordered: the live state first.

## 0.7.4
- Clean AddressSanitizer/UBSan run. The name table no longer empties in the middle of a battle;
  a learned level only counts if it was written in this battle; gentler sweep outside battles.

## 0.7.3
- Name detector rewritten: it watches what is written at each memory location. It follows a
  Pokémon forced in (Whirlwind) and a trainer's switches.
- The name on screen wins: a wild Pokémon nearby no longer makes the screen flicker during a
  trainer battle.
- Opponent's level: the "Lv." written closest to its name, without the levels in your team menu.

## 0.7.2
- "Heading up" map: round and turned with your direction; turning compass.
- The game's place names on the map.

## 0.7.1
- Opponent by name: a reused memory slot with another name counts as new (the second battle
  missed the opponent); Koraidon/Miraidon are never the opponent; the companion remembers where
  the game writes the name and reads it at once in later battles.
- Your Pokémon in battle is recognised when you switch too ("Go, …!").

## 0.7.0
- The team follows the game's real order even if you haven't saved.
- Opponent's level from the battle's "Lv. XX" texts.

## 0.6.9
- **Opponent detection by the name the game writes on screen** (wild, Terastallized and
  trainers' Pokémon).

## 0.6.8
- Map with its tiles the right way round; more reliable live position (384 copies, consensus).
- Time, date, battery and FPS from Eden Duo itself.
- Opponent chosen by species: adjustable level, never shown as "Fainted".

## 0.6.7
- New trainer card (clock, battery, play time, Pokédex, Blueberry Points).
- Detector fixes (stale reads, retries, a trainer's switches).

## 0.6.6
- Map built from its real tiles (10×10 in Paldea, 4×4 in the DLC).
- Steady battle page (no flicker between the opponent and the list).

## 0.6.5
- Map with zoom, facing arrow and compass.

## 0.6.4
- Opponent's card: coloured base stats, comparison with your Pokémon, frames by multiplier, header
  in its type's colour, Terastallized? button.

## 0.6.2 – 0.6.3
- Platinum-style switch suggestions; larger types.

## 0.5.x
- Bag with the game's icons, Pokédex, map, opponent picker and search by name.

## 0.3 – 0.4
- Team and summary with the game's own data; first battle page.
