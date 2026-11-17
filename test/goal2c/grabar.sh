#!/usr/bin/env bash
# goal2c (Jak Vita): graba tu mando jugando, para usarlo como recorrido de la huella.
#   task boot-game GRABAR=test/goal2c/recorrido_oficial.txt
#   (o directamente: test/goal2c/grabar.sh <fichero>)
#
# El juego corre todo en x86, sin -debug, en modo determinista (reloj virtual de 1/60 s por
# fotograma y cargas del IOP síncronas) y con una configuración limpia (sin partidas guardadas ni
# ajustes tuyos), exactamente como lo reproduce test/goal2c/frame_check.sh. Si el PC no llega a
# 60 fps el juego va a cámara lenta (cada fotograma es 1/60 s de juego): es normal.
# El fichero se escribe mientras juegas (cada segundo de juego se añade un punto de sincronía con
# la posición de Jak). Para terminar, cierra la ventana.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:?uso: grabar.sh <fichero de salida>}"
CFG="$ROOT/build/pc_test/cfg_grabacion"
rm -rf "$CFG"
mkdir -p "$(dirname "$OUT")"
# WSL: sin esto Mesa dibuja por software (llvmpipe, ~10 fps); con d3d12 usa la GPU de Windows
if [[ -d /usr/lib/wsl/lib && -z "${GALLIUM_DRIVER:-}" ]]; then
  export GALLIUM_DRIVER=d3d12 MESA_GL_VERSION_OVERRIDE=4.5 MESA_GLSL_VERSION_OVERRIDE=450
fi
unset GOAL2C_LIB GOAL2C_HUELLA GOAL2C_RECORRIDO
echo "Grabando en $OUT (cierra la ventana del juego para terminar)"
GOAL2C_GRABAR="$OUT" "$ROOT/build/game/gk" -v --game jak1 --config-path "$CFG" -- -boot -fakeiso \
  > "$ROOT/build/pc_test/grabacion.log" 2>&1 || true
frames=$(grep -E "^[0-9]" "$OUT" | awk '{s += $1} END {print s + 0}')
syncs=$(grep -c "^# sync" "$OUT" || true)
echo "Grabados $frames fotogramas ($((frames / 60)) s de juego, $syncs puntos de sincronía) en $OUT"
echo "Comprobación (reproduce sin ventana y compara la posición de Jak): test/goal2c/frame_check.sh"
