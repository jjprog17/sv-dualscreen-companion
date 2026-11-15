# Cómo funciona (notas técnicas)

Todo sale de `native/sv_module.c`, un módulo nativo de Eden Duo (ABI en `native/include/`) que
lee la memoria del juego y el romfs del jugador y publica valores (`sv.*`) que el manifiesto
(`tools/gen_manifest.py` → `packages/*/dualscreen/manifest.json`) dibuja.

## Partida

- Bloques del guardado (SCBlock, cifrado XorShift32): tabla de claves en
  `[main+0x47350D8, 0xD8, 0x0, 0x0, 0x30, 0x08]`.
- Claves usadas: equipo `0x3AA1A9AD`, dinero `0x4F35D0DD`, PL `0xADB4FE17`, MyStatus `0xE3E89BD1`,
  mochila `0x21C9BD44`, Pokédex `0xF5D7C0E2`, ubicación `0x19FC5B7B`, región `0xF17EB014`,
  coordenadas `0x708D1511`, tiempo de juego `0xEDAFF794`, Puntos Arándano `0x66A33824`.
- Equipo en directo: `[main+0x4763C98, 0x08, 0x30+8*slot, 0x30]` → PK9 cifrado. Su orden es el
  del juego en ese momento (el bloque del guardado solo cambia al guardar). Los bytes 0x148+
  (nivel y estadísticas) del objeto en vivo no siempre son válidos: se recalculan con la
  experiencia, los IV, los EV y la naturaleza.
- Estado del mapa: `[main+0x473ADE0, 0x160, 0xE8, 0x28]` vale 0x11 en el campo (salta también
  a 4, 5, 6, 18 y 19; se filtra con varias lecturas).

## Datos del juego (romfs)

Se leen con el índice Trinity (`arc/data.trpfd` / `data.trpfs`) y se descomprimen con ooz:
`personal_array` (tipos, estadísticas base, crecimiento…), textos (`monsname`, movimientos,
objetos, Pokédex…), iconos BNTX de objetos y Pokémon (BC1–BC7 y ASTC con el host) y el mapa.
El mapa no es una imagen: son piezas `appli/tex/ymap_ter_small/ymap_ter_small_CC_RR/` (Paldea
10×10, las DLC 4×4; CC = columna de oeste a este, RR = fila de sur a norte). El módulo las une
en un mosaico de 2048 px y lo recorta para el zoom. Escala: Paldea `u = x/5000`,
`v = (z+5500)/5000`; Kitakami y Arándano con 2000 y desfase 2000 (RaidCrawler).

## El rival

1. **Nombres en pantalla** (desde 0.6.9). Se barre ±192 MB alrededor del equipo en vivo buscando
   nombres de especie en UTF-16 (los de `monsname`, en el idioma de la partida). En el campo se
   guarda cada (dirección, nombre); en combate cuentan los que son nuevos. Se quitan los del
   equipo (sirven para saber cuál de los tuyos está fuera) y Koraidon/Miraidon. Gana el nombre
   que aparece más tarde y, si empatan, el más repetido. El nivel es el «Nv. N» nuevo que no es
   el de tu Pokémon. Al acabar el combate se guardan las direcciones del nombre del rival: en el
   siguiente se leen directamente.
2. **PS**: entre los PK9 que el barrido sigue, el que pierde vida está en combate.
3. **Campo**: un PK9 del campo cuyos bytes cambian al empezar el combate (solo si tiene
   experiencia > 0).
4. **Caminos aprendidos**: búsqueda inversa de punteros hacia el rival (experimental).
5. **A mano**: lista de Pokémon cercanos o búsqueda por nombre con nivel ajustable.

Lo que se descartó, y por qué, está en el historial (`CHANGELOG.md`).

## Posición en el mapa

La posición guardada sale del bloque de coordenadas (`PlayerSave_StartPosition`, que el juego
reescribe a menudo). Para la posición en vivo se buscan copias de esas coordenadas en memoria
(hasta 384). Cada copia que se mueve gana puntos según cuántas otras coinciden con ella; se sigue
la de más puntos y solo se cambia de copia si otra la supera con claridad. Cuando el bloque
cambia y coincide con la copia seguida, esta se confirma; si queda lejos (más de 80 unidades),
las copias lejanas quedan en entredicho y se busca de nuevo sin soltar la actual. Una copia que
deja de leerse se retiene 3 segundos. La orientación sale de los pasos de esa misma copia (cada
4 unidades); una media vuelta solo se acepta tras dos pasos en esa dirección.

## Pruebas

`native/test_host.c` simula el host y la memoria del juego: partida, equipo en vivo, combates
(PK9 nuevos, cambiados, teracristalizados, entrenadores, nombres en pantalla, huecos reutilizados),
mochila, Pokédex, mapa (piezas, zoom, posición) y ficha. Uso:

```sh
cd native && ./build.sh
clang -O1 -Iinclude -o out/test_host test_host.c -ldl
python3 ../tools/make_test_romfs.py /tmp/romfs <PKHeX>/PKHeX.Core <moves.csv>
SV_ROMFS=/tmp/romfs ./out/test_host out/linux-x86_64/0100A3D008C5C000.so C01884F294C053C4 \
  <pk9 del equipo>... --opp <pk9 del rival>
```
