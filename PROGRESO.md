# Jak Vita: progreso

Actualizado: 2026-11-17 (PAUSA a petición de Diego, a mitad de la bisección de common-obs).

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
| D | target (hecho), common-obs | **common-obs: bisección a medias** (ver "Para retomar") |
| E | levels/village1, village_common | Pendiente |
| F | kernel + 8 funciones en ensamblador | Pendiente |
| G | resto de niveles por DGO | Pendiente |
| — | level-h y res-h (objeto no idéntico) | Al final |

## Último logro

Recorrido oficial grabado por Diego (`test/goal2c/recorrido_oficial.txt`, 8,4 min de Geyser Rock
y Sandover): sincronía exacta sin ventana (503/503) y huella x86 = C con todo lo ya en C (46166
fotogramas). Arreglo de `level.gc` aprobado.

## Para retomar (estado exacto de la pausa)

Bisección de common-obs (27 ficheros) con el recorrido oficial, interrumpida a mitad. Log en
`docs/notas/biseccion_common_obs_parcial.log`. Ningún fichero de common-obs está instalado:
`out/goal2c/jak1/` tiene los 271 validados de siempre y `game_only.txt` no cambió.

Orden del lote: generic-obs-h water-h process-drawable generic-obs voicebox water
collectables-part collectables process-taskable crates rigid-body-h water-anim dark-eco-pool |
rigid-body nav-enemy-h nav-enemy baseplat basebutton tippy babak sharkey orb-cache plat
plat-button plat-eco ropebridge ticky.

- **Pasan provisionalmente (12):** generic-obs-h, water-h, process-drawable, voicebox,
  collectables-part, process-taskable, rigid-body-h, water-anim, dark-eco-pool, rigid-body,
  nav-enemy-h, nav-enemy.
- **Culpables (4):**
  - generic-obs, water y collectables fallan la huella (frame_check);
  - crates hace caer el juego (boot_smoke).
- **Sin terminar:**
  - baseplat, basebutton, tippy y babak: el grupo falla la huella, falta bisecarlo;
  - sharkey, orb-cache, plat, plat-button, plat-eco, ropebridge y ticky: sin probar.

Siguiente sesión:
1. `python3 goalc/c_backend/tools/validate_batch.py --objs <los 12 que pasan + los 11 pendientes>`.
   No usar la carpeta entera: así se salta lo ya sabido. Si sale mal, biseca solo.
2. Depurar los culpables, uno a uno:
   - **crates:** SIGSEGV en boot_smoke. Ver el log `build/pc_test/boot_smoke.log` y la librería
     con símbolos: `build_pc_lib.py --symbols`, `addr2line`.
   - **generic-obs, water, collectables:** primer fotograma distinto con frame_check.
     `GOAL2C_HUELLA_VOLCADO=<n>` vuelca el fotograma y `GOAL2C_FN_X86=nombre,...` biseca por
     función dentro del fichero. Después `GOAL2C_WATCH` sobre la dirección que cambie.
3. Casos ARM del lote D: target tiene pocas funciones de cálculo, por ejemplo
   `warp-vector-into-surface!` y `death-movie-remap`.
4. Commit del lote D y punto de control.

Cada paso de la bisección tarda unos 4-5 min con el recorrido oficial. Lanzar siempre los tests
con `timeout` y esperar en primer plano. No lanzar juegos si Diego está probando.
