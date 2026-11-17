# Jak Vita — port de la trilogía Jak and Daxter a PS Vita

Fork de OpenGOAL (open-goal/jak-project). Meta: Jak 1 nativo en PS Vita a 30 fps,
mismo modelo que TwinsanityEvolved (decomp + runtime nativo + ISO de PS2 del usuario).
Después Jak 2 y Jak 3.

## Idioma (no negociable)
- TODOS los mensajes a Diego en español: los intermedios entre herramientas, los de estado y
  los resúmenes finales. Aunque los logs, el código o las salidas estén en inglés.
  (Se incumplió dos veces el 2026-11-16 en tareas largas.)

## Puntos de control (no negociable, 2026-11-17)
1. Al terminar cada lote o tarea grande: reporte de 5 líneas como máximo (qué se logró, avance %,
   qué sigue, qué se necesita de Diego) y ESPERAR su "sigue" antes de continuar.
2. Mantener PROGRESO.md actualizado en cada punto de control: fases, lotes, %, último logro,
   siguiente paso.
3. Si Diego tiene que probar o jugar algo: decírselo con pasos exactos y NO lanzar juegos (gk)
   mientras prueba (comparten el puerto del listener, la CPU y test/goal2c/recorrido_oficial.txt).

## Reglas legales (no negociables)
- NUNCA commitear datos del juego: ISOs, DGOs, texturas, audio, archivos extraídos.
  Viven en iso_data/, decompiler_out/, out/ y deben estar en .gitignore.
- El usuario aporta su propio ISO de PS2. El .vpk solo lleva código y assets propios.
- No usar dumps de la versión oficial de PS Vita/PS3 como fuente.

## Entorno
- Ubuntu en WSL2 (Windows). Trabajar siempre en ~/proyectos/jak-project, nunca en /mnt/c.
  La carpeta DEBE llamarse jak-project: las herramientas de upstream localizan el proyecto
  buscando ese nombre en su ruta (si no, fallan con "Could not setup project path").
  El repo de GitHub se sigue llamando jak-vita; eso no importa.
- VitaSDK en /usr/local/vitasdk (gcc 15), paquetes con `vdpm install <pkg>`.
  Instalados: vitaGL, vitaShaRK, libmathneon, SceShaccCgExt, taihen, zlib.
- Build de PC (flujo del README): `task gen-cmake-release && task build-release`
  (preset Release-linux-clang, salida en build/). Arrancar el juego: `task boot-game`.

### ISO y extracción (importante)
- El ISO vive en la RAÍZ de iso_data/ (p. ej. iso_data/<juego>.iso), NUNCA dentro de
  iso_data/jak1/. Al extraer un ISO, el extractor hace `remove_all(iso_data/jak1/)` y
  copia ahí los ficheros del disco: un ISO guardado dentro se borraría.
- iso_data/jak1/ contiene el disco ya extraído (DGO/, CGO/, ...). Es lo que usa
  `task extract` (README), que decompila desde esa carpeta.
- Extracción inicial desde el ISO (o tras cambiar de ISO):
  `./build/decompiler/extractor -e -d -c "iso_data/<juego>.iso"`; después
  `git restore iso_data/jak1/.gitignore` (el remove_all borra también ese fichero versionado).
- Versión actual: SCUS-97124 (NTSC-U, black label → `task set-decomp-ntscv1`).

### Capturas de pantalla
- Solo de la ventana de Vita3K o del juego, con vita/tools/screenshot.ps1
  (`-ProcessName Vita3K` o `-TitleLike 'OpenGOAL*'`). NUNCA capturar la pantalla
  completa ni listar ventanas/procesos con título del escritorio de Windows.

### Herramientas
- Vita3K está instalado en Windows: copiar el .vpk a /mnt/c/Users/<usuario>/jakvita/.
- Vita real con HENkaku, VitaShell, vitacompanion (FTP 1337, comandos 1338), ShaRKF00D.

## Cambios de Jak Vita en goal_src (upstream)
Marcados en el código entre `;; CAMBIO JAK VITA (inicio)` y `;; CAMBIO JAK VITA (fin)`
(`grep -rn "CAMBIO JAK VITA" goal_src`). Revisarlos en cada merge de upstream.
- engine/gfx/foreground/bones.gc (draw-bones): pesos de blerc a cero (aprobado 2026-11-17).
- engine/level/level.gc (unload!): termina el enlace en curso del nivel antes de limpiarlo
  (cierre del juego tras saltar la intro y coger la primera célula; 2026-11-17, pendiente de
  aprobar).

## Arquitectura objetivo
goal_src/ (GOAL decompilado, ya existe)
  ├─ goalc -> x86-64          (upstream, PC; NO romper)
  └─ goal2c (NUEVO) -> C      -> arm-vita-eabi-gcc -> ARMv7 (Vita)
runtime C++ (game/kernel, overlord, sound) -> portado a VitaSDK
renderer -> backend nuevo sobre vitaGL (nivel GLES2, sin compute shaders)
Código nuevo: vita/ y goalc/c_backend/. Cambios a upstream mínimos, detrás de
#ifdef __vita__ o flags de CMake, para poder hacer merge de upstream.

## Restricciones de la Vita
- ARM Cortex-A9 x4, 333 MHz (444 con overclock), NEON, 32 bits.
- RAM limitada (512 MB totales, menos para la app), 128 MB VRAM.
- OpenGOAL asume punteros de 64 bits: revisar todo cast puntero<->entero.
- vitaGL necesita libshacccg.suprx en la consola (ShaRKF00D).

## Flujo de trabajo
- Claude Code escribe y compila. Diego prueba en hardware real y reporta.
- Antes de pedir prueba: compila sin errores. Indicar qué probar y qué se espera ver.
- Prueba en Vita3K obligatoria antes de pedir prueba en la Vita física:
  `cmake --build build-vita && vita/tools/emu_test.sh` (desde WSL). Instala el .vpk,
  lanza JAKV00001, captura la ventana y copia logs a build-vita/emu/
  (screenshot.png, log.txt de la app, vita3k.log, vita3k_errors.txt) y cierra el emulador.
  Revisar la captura y los logs; exit 0 = la app seguía viva y su log no está vacío ni
  tiene "ERROR" (el script no mira la imagen: revisarla siempre).
  Solo pedir a Diego que pruebe en la Vita lo que ya pasó en Vita3K (o explicar por qué
  algo no se puede comprobar en el emulador).
- Ruta de Vita3K.exe: variable VITA3K_EXE o fichero vita/tools/.vita3k_path (no versionado).
- Despliegue a la Vita física: POR USB con VitaShell (vitacompanion no le funciona a Diego).
  `APP=<app> vita/tools/deploy.sh --usb` copia el .vpk a C:\Users\<usuario>\jakvita\ e
  imprime los pasos (USB de VitaShell, instalar el .vpk, lanzar, copiar el log de vuelta a
  build-vita/). Al pedir una prueba, dar esos pasos. Los modos FTP de deploy.sh quedan
  como alternativa.
- Logs en la Vita: ux0:data/jakvita/log.txt además de stdout.
- Probar el backend C primero en PC (Linux) antes de llevarlo a la Vita.
- Varias apps de Vita, elegidas con APP= en emu_test.sh y deploy.sh:
  hello (JAKV00001, log.txt) y bench (JAKV00002, benchmark goal2c, bench.txt).
  Resultados de Vita3K en build-vita/emu/<APP>/.

## goal2c (Fase 1+)
- Código: goalc/c_backend/ (CBackend.*, runtime/goal2c_rt.h). Enganche en upstream:
  IR::do_codegen_c (IR.h) y la opción `:goal2c "salida.c"` de asm-file.
  Ejemplo en el REPL/goalc -c: `(asm-file "goal_src/jak1/engine/math/math.gc" :goal2c "out/goal2c/jak1/math.c")`.
- Semántica: replicar EXACTAMENTE la de do_codegen_x86 de cada IR (la referencia es goalc
  x86). Ojo: float->GPR extiende el signo; IMUL_32/DIV_32 extienden el signo; desplazamientos
  enmascarados a 6 bits; comparaciones float con semántica de comiss.
- Llamadas entre funciones: el objeto función de GOAL guarda en su zona de código el
  puntero a la función C nativa; el C llama con `G2C_NATIVE_FN(dir)` y la aridad del
  llamante (seguro con AAPCS/SysV, que limpian la pila en el llamante). Los datos estáticos
  (strings...) siguen en el .go; el C obtiene su dirección al enlazar con
  g2c_rt_static(fichero, índice en FileEnv::statics()).
- Validación en PC (obligatoria antes de llevar nada a la Vita):
  `ninja -C build goalc-test && build/goalc-test --gtest_filter='Goal2CTests.*'`
  Transpila math.gc, compila el C con cc + test/goal2c/harness.c y compara bit a bit con
  goalc ejecutando test/goal2c/math_cases.txt en el runtime real. Genera
  out/goal2c/jak1/{math.c,math_expected.h}, que usa el benchmark de la Vita.
- No romper upstream: tras tocar goalc o el runtime, pasar `build/goalc-test` completo.

### Modo mixto en el runtime de PC (x86 de goalc + C de goal2c en el juego real)
- Generar C de un fichero: `(asm-file "<gc>" :goal2c "out/goal2c/jak1/<obj>.c")`. Además del
  .c escribe <obj>.c.o, el objeto de goalc, que debe ser IDÉNTICO al out/jak1/obj/<obj>.o
  que carga el juego (de él salen las posiciones de los estáticos y la huella de segmentos).
- Librería: `python3 goalc/c_backend/tools/build_pc_lib.py --dir out/goal2c/jak1
  --out out/goal2c/jak1/libgoal2c_jak1.so` (todo el C de la carpeta, con -DG2C_PC_MIXED).
- Jugar con ella: `GOAL2C_LIB=$PWD/out/goal2c/jak1/libgoal2c_jak1.so task boot-game`
  (o gk directamente). El log dice "[goal2c] <obj>: N funciones en C". Si el objeto cargado
  no coincide con el transpilado, ese fichero se queda en x86 (aviso en el log).
- Runtime: game/goal2c/goal2c_jak1.cpp, enganchado en klink.cpp DESPUÉS del top-level.
  x86->C: objeto función que guarda r13 (pp) y salta a _arg_call_systemv. C->x86:
  _call_goal8_asm_systemv. Solo Linux x86-64.
- Tests: Goal2CTests.<Fichero> (transpila + referencia goalc) y
  Goal2CMixedTests.<Fichero>InGame (lo mismo en el juego con las funciones en C):
  `build/goalc-test --gtest_filter='Goal2C*'`. Casos: test/goal2c/math_cases.txt y
  test/goal2c/<obj>_cases.gc, a los que el test antepone test/goal2c/cases_prelude.gc
  (macros de impresión en bits, pc-rand determinista). Se compara el texto exacto.
- Añadir un fichero validado: tabla kGameFiles en test/goalc/test_goal2c.cpp + su
  <obj>_cases.gc + dos TEST_F de una línea. No imprimir direcciones de objetos (inspect/print
  de objetos del heap) y no llamar a funciones que el juego no usa: algunas son trampas a
  propósito (matrix-axis-sin-cos-vu! hace (/ 0 0) -> SIGFPE en x86). El test instala un
  manejador de SIGFPE que dice el offset EE; con las líneas "[link and exec] ... : 0x..." del
  log se encuentra el objeto.
- Variables de pila de GOAL (new 'stack...): IR_GetStackAddr -> array local alineado a 16
  por función (g2c_stk); dirección GOAL = dirección real - g2c_mem. IMPRESCINDIBLE el
  `__asm__ volatile("" : : "r"(g2c_stk) : "memory")` tras declararlo y que G2C_ADDR calcule
  la dirección como entero: si no, gcc -O2 no le reserva sitio en el marco (solo ve su
  dirección como entero) y las llamadas lo machacan (fallo real visto en euler.gc).
- pp (r13: self en behaviors, with-pp) -> G2C_PP, variable global del runtime (en modo mixto
  apunta al g_pp del runtime). self NO es argumento de la firma C. rsp/r14/r15 son de solo
  lectura (G2C_SP_REG = marco de C actual, tabla de símbolos = #f, G2C_BASE_REG = g2c_mem).
  OJO: un rlet que reutiliza un registro ya restringido (pp dentro de un behavior, el 2.º
  with-pp de una función) crea una RegVal SIN rlet_constraint; la restricción solo está en
  FunctionEnv::constraints() (contrain_everywhere). CFunctionWriter las mira por ireg. Antes
  de arreglarlo (2026-11-16) pp salía como una local a 0 en collide-shape, path-h, etc.
- Validados en el juego (2026-11-16): engine/math entero (con casos también vector-h y
  transformq), engine/geometry, engine/collide, engine/camera entero (solo en el juego) y
  engine/entity/res (con casos: prueba la ABI de 128 bits). res-h no: su inspect autogenerado
  depende de si entity-links está ya definido, y el objeto no sale idéntico.
- Suspend nativo (2026-11-17, modo mixto de PC): thread-suspend/thread-resume de cpu-thread
  (métodos 10 y 11) se sustituyen por g2c_native_suspend/resume (asm en goal2c_jak1.cpp). Mismo
  modelo que Jak (todos los procesos en la misma pila; al suspender se copia [sp, stack-top) y
  al reanudar se devuelve a la MISMA dirección: los punteros a la pila de los marcos de C siguen
  valiendo), pero la copia va a un búfer del host por suspensión (sin el límite de 256 bytes del
  área de respaldo; pilas de hasta ~2.8 KB con marcos de C). El área de respaldo del hilo guarda
  {marca, id, cookie} (viaja con el proceso si se reubica). La dirección de retorno se queda en
  la pila guardada (desde C es del host, no cabe en pc) y el resume vuelve con ret; pc solo vale
  para el arranque (set-to-run). deactivate de process pasa por un gancho que libera el búfer
  de un proceso que muere suspendido. host_call deja en g_pp el r13 FINAL (tras un resume, el
  proceso que fija el kernel). Se omite el aviso KERNEL_DEBUG "suspend called without enough
  stack" del macro suspend (ya no aplica). Solo con la librería cargada; GOAL2C_SUSPEND_X86=1
  vuelve al de x86 (y entonces las G2C_FN_WRITES_PP se quedan en x86).
  Validado con cam-states (17 funciones que suspenden) + todo lo ya en C: boot_smoke,
  frame_check, collide_check; mutación (pp viejo tras host_call) -> collide_check falla.
  Vita (Fase 3): mismo diseño en ARM (r4-r11, sp, lr, d8-d15) junto con el kernel en C.
  OJO reubicación: si un C suspende y su proceso se mueve de memoria, al volver a su llamador x86
  r13 debe ser el pp NUEVO. El C conserva r13 (callee-saved en SysV), así que los trampolines
  x86 -> C propios (g2c_arg_call, g2c_stack_call, g2c_q_call_systemv) recargan r13 de g_pp al
  volver. Sin eso: pp viejo -> lista de conexiones corrupta -> bucle infinito (visto en
  ja-play-spooled-anim con el recorrido). En la Vita: tras cada llamada, pp sale de G2C_PP.
- Recorrido guionizado: test/goal2c/recorrido.txt (GOAL2C_RECORRIDO, solo con GOAL2C_HUELLA):
  CPadGetData sustituye el mando 0 por el guion; los fotogramas cuentan desde que existe
  *target*; se repite. frame_check.sh lo usa por defecto (FRAME_RECORRIDO= lo desactiva). Pasa
  por título, nueva partida y cinemática: cubre mucho más que Jak en reposo.
- Grabación del mando (recorrido oficial de Diego): `task boot-game GRABAR=<fichero>` ->
  test/goal2c/grabar.sh: todo x86, sin -debug, modo determinista (GOAL2C_GRABAR activa el reloj
  virtual y RpcBusy síncrono, como la huella), configuración limpia (--config-path
  build/pc_test/cfg_grabacion: sin partidas ni ajustes) y en WSL GPU por d3d12
  (GALLIUM_DRIVER=d3d12 MESA_GL_VERSION_OVERRIDE=4.5; sin eso llvmpipe a ~10 fps). Graba
  botones, sticks y presiones por fotograma desde que existe *target*, y cada segundo de juego
  "# sync t x y z" (posición de Jak). Al reproducir (frame_check usa
  test/goal2c/recorrido_oficial.txt si existe, también con config limpia) se compara la posición:
  "DESINCRONIZADO" = la grabación no se reproduce igual. Probado: grabar con ventana y GPU y
  reproducir sin ventana -> sincronía exacta y huella x86 = C.
  Sonido determinista (game/goal2c/det_sound_jak1.cpp): el vblank del IOP (que escribe
  *sound-iop-info*: strpos, str-id) va con el vsync del renderer y strpos con lo que suena de
  verdad -> las cinemáticas (ja-play-spooled-anim) se desincronizaban. En modo determinista el
  IOP no lo copia; el EE lo calcula cada fotograma desde las órdenes del RPC de streams (canal 5)
  con reloj virtual y la duración de la cabecera VAG. GOAL2C_SONIDO_REAL=1 = comportamiento
  anterior (diagnóstico). Probado: 406 puntos de sincronía (6,8 min, 8 streams) sin ventana.
  Pantalla y mandos: en modo determinista las consultas de GOAL a la pantalla (tamaño de
  ventana, resoluciones...) y a los mandos (número, presión, vibración) responden como sin
  ventana (game_display() en kmachine.cpp): con ventana daban otros valores y la grabación se
  desincronizaba a los 86 s (2.ª grabación de Diego, 2026-11-17).
- PROGRESO.md: estado del proyecto para Diego (actualizarlo en cada punto de control).
- Diagnóstico: GOAL2C_FN_X86=nombre,... deja esas funciones en x86; GOAL2C_PP_X86 /
  GOAL2C_PP_SOLO=obj,... para bisecar las que escriben pp. Proceso colgado: enviar SIGSEGV al
  hilo EE (tgkill) y el manejador lista la pila (direcciones de gk, librerías y GOAL).
  build_pc_lib.py --symbols = -O2 -g. NO usar --debug (-O0) para depurar en el juego: los marcos
  de -O0 ocupan KB y desbordan la pila de los procesos (24 KB), da fallos falsos.
- Lambdas (mov-fa) y funciones sin símbolo ni método (estados, behaviors anónimos): el C
  obtiene la dirección del objeto función en el objeto cargado (g2c_fn[], como los
  estáticos). En el modo mixto el runtime PARCHEA in situ su código x86 con
  `movabs rax, entrada; jmp rax` (12 bytes; si la función es más corta se queda en x86).
  En la Vita el cargador deberá escribir el puntero C nativo en el objeto función.
- Variables con la dirección tomada ((& var)): local de C cuya dirección escapa con
  `__asm__ volatile("" : : "r"(&var) : "memory")`; el puntero GOAL es &var - g2c_mem.
- Los .gc decompilados leen a veces registros nunca escritos (valores indefinidos del MIPS
  original): en x86 es basura, en C 0. No es un fallo de goal2c.
- Funciones TRAMPA de OpenGOAL (no las usa el juego; abortan a propósito con (crash!) o
  (/ 0 0)): matrix-axis-sin-cos-vu!, circle-circle-xz-intersect (y vector-circle-tangent-new,
  que la llama), raw-ray-sphere-intersect, dma-sync-crash. No llamarlas en los casos.
- Ficheros "solo en el juego" (tabla kGameOnlyFiles): objeto idéntico, C que compila limpio,
  funciones instaladas, prueba de humo y, para colisión y cámara,
  `test/goal2c/collide_check.sh`: posición de Jak en reposo y cámara (posición, fov y matriz
  del combinador), todo x86 frente a modo mixto, deben ser idénticas bit a bit (comprobado con
  mutación: collide-cache roto -> Jak a otra altura; slave-set-rotation! roto -> matriz a 0).
  `CHECK_X86_TWICE=1` compara dos ejecuciones x86 (la medida es determinista).
- res-lump en los casos: `new res-lump` deja `tag` (un puntero en OpenGOAL) a 0; hay que darle
  tabla de tags y zona de datos a mano (ver res_cases.gc).
- En los casos, un trsqv (basic) se pasa como transformq con `(the-as transformq (-> x trans))`:
  `(the-as transformq x)` queda desalineado y el movaps de x86 da SIGSEGV.
- Hablar con el juego desde un script: REPL de goalc por tubería, línea a línea
  ((mi), (lt), consulta, (:exit) con esperas). `goalc -c` NO sirve: compila antes de conectar.
- Cuidado con lecturas de 16 bytes de estructuras de 12 (vector3s): arrastran 4 bytes del
  heap, distintos en cada runtime; en los casos, guardarlas dentro de un vector.
- collide-planes aparece en all_objs.json pero no tiene fuente .gc en OpenGOAL.

### Contador de avance (reportarlo en UNA línea al final de cada entrega)
- `python3 goalc/c_backend/tools/progress.py` (unos 20 s; `--reuse` no recompila). Ejecuta
  goal2c sobre todos los objetos de código de Jak 1 (all_objs.json, versión 3) escribiendo en
  out/goal2c/jak1/all/ (FUERA de la librería del juego) y cuenta:
  total de funciones, traducibles (goal2c las traduce) y en C (validadas en el juego = los .c
  de out/goal2c/jak1/). Tabla por carpeta en out/goal2c/jak1/progress.txt.
- Línea a copiar al final de la entrega: la última que imprime ("Avance goal2c: ...").
- Lo que queda sin traducir (2026-11-16, 11 funciones): asm-func (8, kernel) y .push/.add
  de registros (3, gkernel/gstate). Las G2C_FN_WRITES_PP cuentan como traducibles pero no
  "en C" (se listan aparte).
- OBLIGATORIO además: `test/goal2c/boot_smoke.sh` (juego real sin ventana 45 s con la
  librería; exige que no falle, que instale funciones y que avance tras cargar el nivel).
  Los tests de goalc-test NO ejecutan código en la pila x86 nativa; el juego sí.
- Para comprobar que un test sirve: romper a propósito el C generado (o goal2c_rt.h) y ver
  que falla.
- Depurar un fallo en modo mixto: librería con símbolos
  `build_pc_lib.py --debug --out .../libgoal2c_jak1_dbg.so`; el runtime imprime
  "[goal2c] SIGSEGV en <lib> + 0xOFF" y la cadena de llamadas; `addr2line -f -e <lib> 0xOFF`.
- Direcciones: G2C_ADDR suma la dirección GOAL como uintptr_t. En el PC el código GOAL que
  corre en la pila x86 nativa pasa direcciones "rsp - base" de 64 bits (vectores en pila);
  truncarlas a 32 bits rompía el juego (y ningún test de goalc-test lo veía).

### Fase 2: validación por lotes (plan aprobado 2026-11-16)
- Lotes con `python3 goalc/c_backend/tools/validate_batch.py <carpetas>` (`--dry-run` solo
  transpila y comprueba). Por fichero: objeto idéntico al del juego y C limpio en PC y ARM. Por
  lote: boot_smoke.sh + frame_check.sh + collide_check.sh; si falla, biseca y deja fuera los
  culpables. Los validados se añaden a test/goal2c/game_only.txt (lo lee goalc-test).
  Un commit por lote.
- Objetos de entrada a C: en la zona 0x4e00000-0x5000000 (hueco libre entre el heap global y el
  de depuración), NO en el heap. El C tampoco crea símbolos (los del segmento de depuración no
  cargado apuntan a un símbolo ficticio). Así el heap es idéntico al de una ejecución x86.
- Huella por fotograma (`test/goal2c/frame_check.sh`, GOAL2C_HUELLA=1): hash de la cadena DMA en
  send_gfx_dma_chain, x86 frente a C, fotograma a fotograma. Determinismo: reloj virtual
  (__read-ee-timer avanza 1/60 s por fotograma), RpcBusy espera al IOP, y SIN -debug (el texto
  de depuración cambia cada fotograma). Se enmascara la 4.ª palabra de STQ (GIF PACKED y
  UNPACK V4-32 con Q=1.0): GOAL la deja indefinida (movss sobre un registro: en x86 queda lo
  anterior, en C 0). `CHECK_X86_TWICE=1` = x86 contra x86 (0 diferencias en 26000 fotogramas).
  `GOAL2C_HUELLA_VOLCADO=<n>` vuelca el fotograma n para compararlo con cmp;
  `GOAL2C_HUELLA_QUE=<addr>` dice de dónde sale un byte del volcado. También se enmascaran las
  pilas muertas (`*dram-stack*` y `*fake-scratchpad-stack*`), que el DMA puede arrastrar.
- `GOAL2C_WATCH=<addr>[,len]`: watchpoint casero (mprotect + paso a paso) que registra quién
  escribe en esa dirección GOAL (rip, argumentos y direcciones GOAL de la pila).
- Lote A (2026-11-17): los dos "culpables" (connect, drawable) eran basura de pila: draw-bones
  solo rellena los pesos de blerc del modelo y pc-merc-draw-request copia los 40 a la cadena
  DMA. Corregido EN goal_src (bones.gc los pone a cero; cambio de Jak Vita aprobado).
- goalc-test SIEMPRE con `timeout`: si el runtime cae, el test siguiente se queda esperando
  para siempre (se perdió una noche así).
- Comprobación en ARM por lote: casos en test/goal2c/arm/<lote>_arm.gc (preludio
  arm_prelude.gc), grabados en x86 por Goal2CTests.Arm<Lote> con GOAL2C_REC: imagen de la
  memoria GOAL + arena antes/después + retorno en out/goal2c/jak1/check/. Reproducción:
  goalc/c_backend/check/g2c_check.c parchea en la imagen cada objeto función con su puntero C
  (como hará el cargador de la Vita) y compara; NaN sin signo. En PC:
  `python3 goalc/c_backend/tools/build_check.py --run <lote>`; en la Vita: app JAKV00003,
  `cmake -S vita -B build-vita && cmake --build build-vita && APP=check vita/tools/emu_test.sh`.
  Caso INVALIDO = llama a algo sin C o lee fuera de la imagen: cambiar el caso, no es fallo
  (el mensaje dice el símbolo llamado; p. ej. `new 'stack` llama al new del kernel).
  El parche son 4 bytes (índice en una tabla que debe apuntar a esa dirección), como el puntero
  de la Vita: hay funciones de solo 6 bytes de código (sphere-cull). La zona baja protegida de
  la memoria EE (EE_MAIN_MEM_LOW_PROTECT) se graba a cero en la imagen.
  Casos: no llamar a funciones que hagan format, lean punteros por fotograma (*ocean-work*) o
  no terminen con esos datos (time-to-ground con gravedad negativa = bucle infinito).
  Lote A: 106 OK, 0 difieren, 3 inválidos, igual en PC y en Vita3K. Lote B: 34 OK.
  Funciones declaradas none en all-types (unpack-comp-rle): g2c-arm-none, el retorno de x86
  es basura. defun-debug (glst-*): segmento de depuración, fuera de la imagen.
- Lote C (2026-11-17): 43 ficheros validados (game, game/task, ui, ui/progress, debug); ARM 58 OK.
  En los casos, game-task 1 (complete) no es válida (su camino de error llama a format).
- Lote B (2026-11-17): 38 ficheros validados. level-h rechazado como res-h (objeto con 13 bytes
  distintos: el inspect autogenerado depende del orden de compilación).

### Vectores y enteros de 128 bits (VECTOR_FLOAT / INT_128 -> g2c_q)
- Semántica = la de goalc x86, que sigue la MIPS de la PS2 con src1 = rs, src2 = rt
  (ojo con los *_swapped de IGenX86). vmaxps/vminps: con NaN o iguales gana src2.
- FLOAT -> vector es movss: solo el elemento 0, el resto se CONSERVA. GPR -> vector es movq:
  parte alta a CERO. vector -> GPR: 64 bits bajos.
- ABI de 128 bits (goalc x86: GPR en rdi..r11 por orden entre los GPR, 128 bits en
  xmm1..xmm8 por orden entre los de 128, retorno en rax o xmm0). En C: parámetros/retorno
  g2c_q por valor con la firma exacta; la llamada nativa (Vita) castea a esa firma. Modo mixto:
  x86 -> C por g2c_q_call_systemv + envoltorio `<f>__q(g8, q8, qret)` (campo fn_q de la
  tabla); C -> x86 por g2c_host_call_q / g2c_call_goal_q (asm en goal2c_jak1.cpp). GOAL
  conserva rbx, rbp, r10, r11, r12 y xmm8..15 (SysV no conserva r10, r11 ni xmm).
- Para la Vita: la NaN por defecto de ARM es 0x7fc00000 y la de x86 0xffc00000. Los casos
  con NaN de vector_cases.gc darán otro signo en ARM; la PS2 no tiene NaN, así que el juego
  no debería depender de ello. Comparar en la Vita ignorando el signo de las NaN.

### Benchmarks
- Siempre varias ejecuciones (vita/bench: 7, alternando variantes) y reportar la MEDIANA
  (con rango mín.-máx.), nunca el mejor tiempo ni una ejecución suelta.
- Decisión riesgo 3 (2026-11-16, medido en Vita real a 333 MHz con vita/bench):
  los GPR del C generado son u64 (semántica exacta de GOAL). u64 cuesta +8 % total frente
  a u32 en math.gc (rango 0.97x-1.21x por función), por debajo del umbral del 15-20 %.
  No se hace narrowing a 32 bits en Fase 2; queda como optimización de Fase 6, solo en
  funciones calientes que señale el perfilado y solo si el tipo GOAL lo garantiza.
  Actualización (con llamadas, Vita real): total +21 %; las llamadas cuestan +23-44 % con
  u64 (en ARMv7 cada arg u64 ocupa 2 registros y a partir del 3.º van a la pila). Ojo: hay
  ruido de ±35 % por función entre builds con el mismo código (seekl u32: 71.8 -> 45.0 ns).
  Decisión: se mantiene u64 como base; el riesgo 3 sigue ABIERTO. Línea a estudiar tras la
  Fase 2 con perfil del juego real en la Vita: ABI tipada que pase en 32 bits los argumentos
  y retornos cuyo tipo GOAL lo garantice (float, punteros, basic) y reconstruya el valor
  exacto en el llamado, validada contra la versión u64.
  Logs: build-vita/bench_fase1_sin_llamadas.txt y build-vita/bench.txt (no versionados).
  Medianas (7 ejecuciones, Vita real): total +18 %; rango dentro de un build < 2 %, pero
  entre builds el mismo C varía hasta ±40 % por colocación del código (lerp-clamp u32 60 ->
  87 ns). Desde 2026-11-16 vita/bench compila el C generado con -falign-functions=64
  (comprobado: todas las funciones empiezan en múltiplos de 64) para reducirlo.

## Fases
0. Fork compila en PC; extractor corre con el ISO de Jak 1. Hello world vitaGL en Vita.
1. goal2c: transpilar funciones simples (math) y validar contra goalc en PC.
2. Todo goal_src/jak1 transpila y el juego corre en PC usando el código C.
3. Runtime en VitaSDK, arranca y llega a la carga de nivel (sin render).
4. Renderer vitaGL mínimo (tfrag, merc, sprites) -> Geyser Rock jugable.
5. Audio, input, guardado, ruta ux0:data/jak1/game.iso.
6. Optimización en hardware: perfilado, NEON, menos draw calls -> 30 fps.

## Estilo
- C++17 como upstream, clang-format del repo. Comentarios y commits en español.
