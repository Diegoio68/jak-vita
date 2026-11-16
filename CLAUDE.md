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
- Claude Code escribe y compila. Diego prueba en Vita3K y en hardware real y reporta.
- Antes de pedir prueba: compila sin errores. Indicar qué probar y qué se espera ver.
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
