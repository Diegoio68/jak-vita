# Jak Vita: progreso

Actualizado: 2026-11-18 (fin de la noche: todo Jak 1 traducible en C).

**Avance goal2c: 9637/9648 funciones de goal_src/jak1 en C dentro del juego (99.9 %);
goal2c ya traduce 9637/9648 (99.9 %).**

## Fases

| Fase | Estado |
|---|---|
| 0. Fork en PC, extractor, hola mundo vitaGL en Vita | Terminada |
| 1. goal2c: funciones simples (math) validadas contra goalc | Terminada |
| 2. Todo Jak 1 en C dentro del juego en PC | **Casi terminada (99.9 %)**: quedan las 11 funciones en ensamblador del kernel |
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
| F | kernel | Hecho (8 ficheros; las 8 asm-func + 3 .push/.add siguen en x86) |
| G | resto de niveles | Hecho (20 carpetas, con salto a cada nivel) |
| — | level-h y res-h | Hecho (traducidos durante (mi)) |

## Último logro (noche del 17 al 18)

- **Lote D:** target y common-obs enteros. Los 10 culpables eran un solo fallo del runtime: los
  objetos de entrada a C estaban mal alineados como basics y los actores no nacían.
- **Lote E:** Geyser Rock, Sandover, título, intro y common (19 ficheros) pasan el recorrido
  oficial a la primera.
- **Lote G:** el resto de niveles (20 carpetas). Para validarlos, salto de nivel determinista
  (`GOAL2C_SALTO`): Jak aparece en el punto de continuación de cada nivel y se compara la huella
  x86 = C durante 60 s. Sin culpables.
- **Lote F, el kernel:** `gkernel` caía en los `throw` de inicialización. El catch-frame guarda la
  vuelta en 32 bits, y desde C esa vuelta es una dirección del host. Lo arregla una rampa de
  llamada C -> GOAL en la memoria GOAL.
- **res-h y level-h:** se traducen durante la propia compilación del juego. Así su objeto sale
  idéntico.

## Decisiones pendientes para Diego

1. **Las 11 funciones no traducibles del kernel:** `return-from-thread(-dead)`, `reset-and-call`,
   el `new` de catch-frame, `throw-dispatch`, `set-to-run-bootstrap`, `thread-suspend/resume` y 3
   con `.push/.add`. En PC siguen en x86, con el suspend nativo en su lugar. En la Vita necesitan
   una implementación nativa. Es el primer trabajo de la Fase 3, y también un diseño (pilas y
   saltos no locales en ARM) que conviene decidir contigo.
2. **Profundidad de la validación del lote G:** con Jak quieto en el punto de continuación solo
   se ejecuta lo que hay cerca. Grabar recorridos cortos de algunos niveles (por ejemplo, uno por
   zona) reforzaría la validación de las mecánicas propias: flut, zoomer, jefes...

## Siguiente paso (con tu visto bueno)

Cerrar la Fase 2: una partida tuya en PC con `GOAL2C_LIB` (todo en C) para confirmarlo a mano.
Después, la Fase 3: el kernel nativo en ARM y el runtime en VitaSDK hasta la carga de nivel.
