#!/usr/bin/env bash
# goal2c (Jak Vita): repite sin ventana una partida grabada con jugar_c.sh (misma entrada, misma
# configuración inicial, misma librería si era en C) y dice si vuelve a fallar.
#   test/goal2c/reproducir.sh build/pc_test/sesiones/<fecha> [segundos]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SES="${1:?uso: reproducir.sh <sesión> [segundos]}"
SECS="${2:-600}"
WORK="$SES/repro_$(date +%H%M%S)"
mkdir -p "$WORK"
cp -a "$SES/cfg_inicial" "$WORK/cfg"
LIB=""
[[ "$(cat "$SES/modo" 2>/dev/null)" == c ]] && LIB="$ROOT/out/goal2c/jak1/libgoal2c_jak1.so"
GOAL2C_HUELLA=1 GOAL2C_LIB="$LIB" GOAL2C_RECORRIDO="$SES/entrada.txt" timeout "$SECS" \
  "$ROOT/build/game/gk" --no-display -v --game jak1 --config-path "$WORK/cfg" -- -boot -fakeiso \
  > "$WORK/juego.log" 2>&1
sed 's/\x1b\[[0-9;]*m//g' "$WORK/juego.log" | grep -E "sincronía:|DESINCRONIZADO" | tail -2
if grep -qE "SIGSEGV|\[die\]" "$WORK/juego.log"; then
  echo "REPRODUCIDO:"; grep -E "SIGSEGV|\[die\]|llamado desde" "$WORK/juego.log" | head -5
else
  echo "No falló ($WORK/juego.log)"
fi
