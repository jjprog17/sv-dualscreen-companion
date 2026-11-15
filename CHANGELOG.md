# Historial

## 0.9.0
- Tema claro además del oscuro: en Ficha, botón **CAMBIAR TEMA** (o en los ajustes de Eden Duo).
  Eden Duo recuerda el tema elegido entre sesiones. Mismo diseño con fondo crema, paneles blancos
  y casillas de tipos en tonos suaves.

## 0.8.2
- Combate: búsqueda guiada del cursor de ataques del juego (botón CURSOR). Tocas el número del
  ataque donde está el cursor, lo mueves y repites; cuando queda una posición, el ataque marcado
  se resalta abajo. Primer paso para poder esconder la lista de ataques de la pantalla de arriba.

## 0.8.1
- Escarlata con su propio tema («D Escarlata»): fondo vino, azulejos dorados de Paldea en las
  franjas, detalles dorados de la Academia y cristales Tera en el combate.

## 0.8.0
- Aspecto nuevo («D»): fondo oscuro, paneles y pestañas en diagonal, letra Barlow Condensed
  (con cursiva en los títulos si tu Eden Duo es reciente) y el color de cada juego: rojo y
  naranja en Escarlata, morado y lila en Púrpura.
- Barra superior en Equipo, Pokédex y Mochila: lugar, dinero, batería y hora.
- Tarjetas del equipo con una gema del color de su tipo y sus tipos a la vista.
- Combate rehecho: el rival en grande, una franja de «Consejo» (quién es más rápido y el mejor
  cambio), sus estadísticas base frente a las tuyas y tres bloques con colores que significan
  algo: rojo lo que le hace daño, verde lo que aguanta y azul lo que no le afecta.

## 0.7.5
- Mapa: deja de alternar entre «norte arriba» y «sur arriba» y la posición ya no salta. La
  posición guardada del juego cambia a menudo; antes cada cambio tiraba la copia en vivo y
  volvía a buscar. Ahora se conserva si coincide, las búsquedas solo añaden copias, la elección
  cambia despacio, una copia que falla un momento se retiene y el rumbo solo sale de los pasos de
  la misma copia (media vuelta: hacen falta dos pasos).
- Diagnóstico del mapa reordenado: primero el estado en vivo (EN VIVO / RETENIDO / sin mover).

## 0.7.4
- Revisión con AddressSanitizer/UBSan sin errores. Tabla de nombres que no se vacía en mitad de
  un combate; el nivel aprendido solo cuenta si se ha escrito en este combate; barrido más suave
  fuera de combate.

## 0.7.3
- Detector por nombre reescrito: vigila qué se escribe en cada sitio de memoria. Sigue al
  Pokémon que entra a la fuerza (Remolino) y a los cambios del entrenador.
- El nombre en pantalla manda: un salvaje cercano ya no hace parpadear la pantalla en un
  combate contra un entrenador.
- Nivel del rival: el «Nv.» escrito más cerca de su nombre, sin los niveles del menú de tu equipo.

## 0.7.2
- Mapa «Hacia donde vas»: redondo y girado según tu dirección; brújula que gira.
- Nombres de lugares del juego sobre el mapa.

## 0.7.1
- Rival por su nombre: un hueco de memoria reutilizado con otro nombre cuenta como nuevo (el
  segundo combate no detectaba al rival); Koraidon/Miraidon nunca son el rival; el companion
  recuerda dónde escribe el juego el nombre y lo lee al instante en los combates siguientes.
- Tu Pokémon en combate se reconoce también al cambiarlo («¡Adelante, …!»).

## 0.7.0
- El equipo sigue el orden real del juego aunque no hayas guardado.
- Nivel del rival a partir de los textos «Nv. XX» del combate.

## 0.6.9
- **Detección del rival por el nombre que el juego escribe en pantalla** (salvajes,
  teracristalizados y entrenadores).

## 0.6.8
- Mapa con las piezas bien orientadas; posición en vivo más fiable (384 copias, consenso).
- Hora, fecha, batería y FPS desde el propio Eden Duo.
- Rival elegido por especie: nivel ajustable, nunca «Debilitado».

## 0.6.7
- Ficha de entrenador nueva (reloj, batería, tiempo de juego, Pokédex, Puntos Arándano).
- Correcciones del detector (lecturas viejas, reintentos, cambios de Pokémon del entrenador).

## 0.6.6
- Mapa montado con sus piezas reales (10×10 en Paldea, 4×4 en las DLC).
- Pantalla de combate estable (sin parpadeos entre rival y lista).

## 0.6.5
- Mapa con zoom, flecha de orientación y brújula.

## 0.6.4
- Ficha del rival: estadísticas base con colores, comparación con tu Pokémon, marcos por
  multiplicador, cabecera del color de su tipo, botón ¿Teracristal?.

## 0.6.2 – 0.6.3
- Sugerencias de cambio al estilo de Platino; tipos más grandes.

## 0.5.x
- Mochila con iconos del juego, Pokédex, mapa, selector de rival y búsqueda por nombre.

## 0.3 – 0.4
- Equipo y ficha con datos del propio juego; primera pantalla de combate.
