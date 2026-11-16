# Jak Vita — port de la trilogía Jak and Daxter a PS Vita

Fork de OpenGOAL (open-goal/jak-project). Meta: Jak 1 nativo en PS Vita a 30 fps,
mismo modelo que TwinsanityEvolved (decomp + runtime nativo + ISO de PS2 del usuario).
Después Jak 2 y Jak 3.

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
- Despliegue a la Vita física: `VITA_IP=<ip> vita/tools/deploy.sh` (ver --help).
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
- Validación en PC (obligatoria antes de llevar nada a la Vita):
  `ninja -C build goalc-test && build/goalc-test --gtest_filter='Goal2CTests.*'`
  Transpila math.gc, compila el C con cc + test/goal2c/harness.c y compara bit a bit con
  goalc ejecutando test/goal2c/math_cases.txt en el runtime real. Genera
  out/goal2c/jak1/{math.c,math_expected.h}, que usa el benchmark de la Vita.
- No romper upstream: tras tocar goalc, pasar `build/goalc-test` completo.

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
