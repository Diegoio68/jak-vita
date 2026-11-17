#!/usr/bin/env bash
# goal2c (Jak Vita): prueba de humo del juego real en modo mixto.
#
# Arranca el juego (sin ventana) con la librería de goal2c durante SMOKE_SECS segundos y exige:
#   - que no se cuelgue ni falle (el proceso sigue vivo al acabar el tiempo),
#   - que no haya SIGSEGV,
#   - que se hayan instalado funciones en C ("[goal2c] <obj>: N funciones en C", N > 0),
#   - que cargue el nivel y siga avanzando después (escenas, audio...).
# Cubre lo que los tests de goalc-test no ven: el código del juego que corre en la pila x86
# nativa, niveles cargados, procesos, etc.
#
# Uso: test/goal2c/boot_smoke.sh [librería.so]   (por defecto out/goal2c/jak1/libgoal2c_jak1.so)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/out/goal2c/jak1/libgoal2c_jak1.so}"
SECS="${SMOKE_SECS:-45}"
LOG="$ROOT/build/pc_test/boot_smoke.log"
mkdir -p "$(dirname "$LOG")"
[[ -f "$LIB" ]] || { echo "error: no existe $LIB"; exit 1; }

GOAL2C_LIB="$LIB" timeout "$SECS" "$ROOT/build/game/gk" --no-display -v --game jak1 -- \
  -boot -fakeiso -debug > "$LOG" 2>&1
code=$?
clean="$(sed 's/\x1b\[[0-9;]*m//g' "$LOG")"

status=0
if [[ $code -ne 124 ]]; then
  echo "FALLO: el juego terminó antes de tiempo (código $code)"
  status=1
fi
if grep -q "SIGSEGV" <<<"$clean"; then
  echo "FALLO: SIGSEGV"
  grep -E "SIGSEGV|llamado desde" <<<"$clean" | head -10
  status=1
fi
# suspend copia la pila del proceso a un área pequeña: si un marco de C quedara en medio, el
# kernel lo detecta (break / mensaje con KERNEL_DEBUG)
if grep -q "suspend called without enough stack" <<<"$clean"; then
  echo "FALLO: suspend sin pila suficiente (¿código C en la pila de un proceso que suspende?)"
  grep -A2 "suspend called without enough stack" <<<"$clean" | head -6
  status=1
fi
installed="$(grep -E "\[goal2c\] [^ ]+: [0-9]+ funciones en C" <<<"$clean")"
if ! grep -qE ": [1-9][0-9]* funciones en C" <<<"$installed"; then
  echo "FALLO: no se instaló ninguna función en C"
  status=1
fi
# señal de vida (sin ventana el kernel de jak1 no imprime "Kernel dispatch time"): el nivel se
# carga y después el juego sigue avanzando (escenas, audio...), lo que deja líneas en el log.
if ! grep -q "link finish: village1-vis" <<<"$clean"; then
  echo "FALLO: no llegó a cargar el nivel (village1-vis)"
  status=1
  progress=0
else
  progress="$(sed -n '/link finish: village1-vis/,$p' <<<"$clean" | wc -l)"
  if [[ "$progress" -lt 10 ]]; then
    echo "FALLO: el juego no avanzó tras cargar el nivel ($progress líneas)"
    status=1
  fi
fi
echo "$installed" | sed 's/^.*\[goal2c\]/[goal2c]/'
echo "líneas de log tras cargar el nivel: $progress, log: $LOG"
[[ $status -eq 0 ]] && echo "OK"
exit $status
