# Jak Vita: progreso

Actualizado: 2026-11-18 (noche: lote D terminado; sigo con el E).

**Avance goal2c: 5343/9648 funciones de goal_src/jak1 en C dentro del juego (55.4 %);
goal2c ya traduce 9637/9648 (99.9 %).**

## Fases

| Fase | Estado |
|---|---|
| 0. Fork en PC, extractor, hola mundo vitaGL en Vita | Terminada |
| 1. goal2c: funciones simples (math) validadas contra goalc | Terminada |
| 2. Todo Jak 1 en C dentro del juego en PC | **En curso (55.4 %)** |
| 3. Runtime en VitaSDK hasta la carga de nivel | Pendiente |
| 4. Renderer vitaGL mínimo, Geyser Rock jugable | Pendiente |
| 5. Audio, entrada, guardado | Pendiente |
| 6. Optimización: 30 fps | Pendiente |

## Fase 2: lotes

| Lote | Contenido | Estado |
|---|---|---|
| Base | engine/math, geometry, collide, camera, res | Hecho |
| A | gfx/*, draw, dma, ps2, engine, data, physics | Hecho (119 ficheros; ARM 106 OK) |
| B | anim, level, load, util, sound, entity, nav | Hecho (38; level-h y res-h al final) |
| C | game, game/task, ui, ui/progress, debug | Hecho (43; ARM 58 OK) |
| Suspend nativo | suspend/resume con copia de pila fuera del heap | Hecho (cam-states, target) |
| D | target, common-obs | Hecho (10 + 27 ficheros; ARM 7 OK) |
| E | training, village1, village_common, title, intro, common | Hecho (19 ficheros, recorrido oficial) |
| F | kernel + 8 funciones en ensamblador | Pendiente |
| G | resto de niveles por DGO | Pendiente |
| — | level-h y res-h (objeto no idéntico) | Al final |

## Último logro

Lote D terminado: engine/target y engine/common-obs entero en C. Ambos pasan el recorrido oficial
(huella x86 = C, sincronía 503/503, colisión y cámara). Los 10 culpables de common-obs eran un
solo fallo del runtime: los objetos de entrada a C estaban mal alineados como basics, así que
`valid?` los rechazaba y los actores con `init-from-entity!` en C no llegaban a nacer. ARM: 205
casos OK en PC y Vita3K.

## Siguiente paso

Lote E: levels/village1, village_common y training (Geyser Rock), que recorre la grabación de
Diego. Después, el resto de niveles por DGO (lote G), con el recorrido oficial donde aplique.
