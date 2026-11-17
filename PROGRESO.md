# Jak Vita: progreso

Actualizado: 2026-11-17 (punto de control: recorrido oficial).

**Avance goal2c: 4611/9648 funciones de goal_src/jak1 en C dentro del juego (47.8 %);
goal2c ya traduce 9637/9648 (99.9 %).**

## Fases

| Fase | Estado |
|---|---|
| 0. Fork en PC, extractor, hola mundo vitaGL en Vita | Terminada |
| 1. goal2c: funciones simples (math) validadas contra goalc | Terminada |
| 2. Todo Jak 1 en C dentro del juego en PC | **En curso (47.8 %)** |
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
| D | target (hecho), common-obs | **common-obs: repetir la bisección** (generic-obs falla la huella) |
| E | levels/village1, village_common | Pendiente |
| F | kernel + 8 funciones en ensamblador | Pendiente |
| G | resto de niveles por DGO | Pendiente |
| — | level-h y res-h (objeto no idéntico) | Al final |

## Último logro

Recorrido oficial grabado por Diego (`test/goal2c/recorrido_oficial.txt`): 8,4 min de Geyser
Rock y Sandover. Se reproduce sin ventana con sincronía exacta (503/503 puntos), y todo lo que ya
está en C (lotes A, B, C, target, suspend nativo) da la misma huella que x86 durante el recorrido
completo (46166 fotogramas). Es la referencia para los lotes D, E y siguientes.

## Siguiente paso

Repetir la bisección de common-obs (lote D) con el recorrido oficial; generic-obs ya fallaba la
huella con el recorrido de prueba.
