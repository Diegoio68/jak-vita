# Jak Vita: progreso

Actualizado: 2026-11-17 (punto de control: segunda grabación).

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

- Grabación de Diego (7,3 min) completa y sin cierres: el arreglo de `level.gc` funciona.
- Pero sin ventana se desincroniza a los 86 s (primera célula). La causa probable: con ventana,
  el juego ve el tamaño de la pantalla y los mandos reales (proporción, cámara, menús); sin
  ventana, no. Arreglado: en modo determinista el juego ve siempre "sin pantalla y sin mandos".
  Hay que grabar otra vez para comprobarlo (la grabación anterior se hizo sin este arreglo).

## Siguiente paso

1. Nueva grabación (o antes una prueba mía de 2 min con ventana, si Diego la permite).
2. Verificar la sincronía sin ventana; si cuadra, es el recorrido oficial.
3. Repetir la bisección de common-obs con ese recorrido.
