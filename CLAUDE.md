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
- Ubuntu en WSL2 (Windows). Trabajar siempre en ~/proyectos/jak-vita, nunca en /mnt/c.
- VitaSDK en /usr/local/vitasdk (gcc 15), paquetes con `vdpm install <pkg>`.
  Instalados: vitaGL, vitaShaRK, libmathneon, SceShaccCgExt, taihen, zlib.
- Build de PC: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
  -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld && cmake --build build -j 12`.
- Las herramientas de upstream buscan una carpeta llamada "jak-project"; la nuestra se
  llama jak-vita, así que extractor/goalc/gk necesitan `--proj-path ~/proyectos/jak-vita`
  (los `task ...` de upstream no lo pasan y fallan con "Could not setup project path").
- El ISO va en iso_data/ (raíz), NO en iso_data/jak1/: el extractor hace remove_all de
  iso_data/jak1/ y lo borraría. Extraer:
  `./build/decompiler/extractor --proj-path "$PWD" -e -d -c "iso_data/<iso>"`, luego
  `git restore iso_data/jak1/.gitignore`. Arrancar:
  `./build/game/gk --proj-path "$PWD" -v --game jak1 -- -boot -fakeiso -debug`.
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
