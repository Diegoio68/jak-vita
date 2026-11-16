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
- Tests: Goal2CTests.{Math,Vector} (referencia goalc) y Goal2CMixedTests.{MathInGame,
  VectorInGame} (lo mismo en el juego con las funciones en C):
  `build/goalc-test --gtest_filter='Goal2C*'`. Casos: test/goal2c/math_cases.txt y
  test/goal2c/vector_cases.gc (salida en bits, se compara texto exacto).
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

### Vectores y enteros de 128 bits (VECTOR_FLOAT / INT_128 -> g2c_q)
- Semántica = la de goalc x86, que sigue la MIPS de la PS2 con src1 = rs, src2 = rt
  (ojo con los *_swapped de IGenX86). vmaxps/vminps: con NaN o iguales gana src2.
- FLOAT -> vector es movss: solo el elemento 0, el resto se CONSERVA. GPR -> vector es movq:
  parte alta a CERO. vector -> GPR: 64 bits bajos.
- Funciones con parámetros o retorno de 128 bits todavía no se traducen (ABI xmm).
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
  87 ns). Próximo benchmark: compilar con -falign-functions=64 para reducirlo.

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
