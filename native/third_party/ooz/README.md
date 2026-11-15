# ooz (vendored)

Open-source Kraken / Mermaid / Selkie / Leviathan / LZNA / BitKnit decompressor by Powzix.

- Upstream: https://github.com/powzix/ooz, commit `05038060aa68f9187ae9923b2388ca8db40e58d1`
  (2019-02-13).
- License: GNU General Public License version 3 or (at your option) any later version, as stated
  in the upstream `kraken.cpp` header (kept unchanged at the top of `kraken.cpp`). The full license
  text is in [COPYING](COPYING). The companions repository itself is GPLv3.
- Used by: the Scarlet/Violet module to decompress Oodle-compressed files of the player's own
  romfs (`arc/data.trpfs`). Nothing decoded is shipped.
- Upstream warns that ooz is **not fuzz safe**; the module only feeds it entries located through
  the game's own `data.trpfd` index, with bounded, exact destination sizes.
