#!/usr/bin/env bash
# goal2c (Jak Vita): graba tu mando jugando, para usarlo como recorrido de la huella.
#   task boot-game GRABAR=test/goal2c/recorrido_oficial.txt
#   task boot-game GRABAR=test/goal2c/recorridos/snow-flut.txt NIVEL=snow-flut-flut
#   (o directamente: test/goal2c/grabar.sh <fichero> [<punto de continuación>])
# Con NIVEL, el juego arranca, carga village1 con el mando bloqueado (unos 10 s en el título) y
# salta a ese punto de continuación; desde ahí se graba. El salto queda anotado en la grabación
# ("# salto ...") y la reproducción lo repite.
#
# El juego corre todo en x86, sin -debug, en modo determinista (reloj virtual de 1/60 s por
# fotograma y cargas del IOP síncronas) y con una configuración limpia (sin partidas guardadas ni
# ajustes tuyos), exactamente como lo reproduce test/goal2c/frame_check.sh. Si el PC no llega a
# 60 fps el juego va a cámara lenta (cada fotograma es 1/60 s de juego): es normal.
# El fichero se escribe mientras juegas (cada segundo de juego se añade un punto de sincronía con
# la posición de Jak). Para terminar, cierra la ventana.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:?uso: grabar.sh <fichero de salida> [<punto de continuación>]}"
NIVEL="${2:-}"
CFG="$ROOT/build/pc_test/cfg_grabacion"
rm -rf "$CFG"
mkdir -p "$(dirname "$OUT")"
# WSL: sin esto Mesa dibuja por software (llvmpipe, ~10 fps); con d3d12 usa la GPU de Windows
if [[ -d /usr/lib/wsl/lib && -z "${GALLIUM_DRIVER:-}" ]]; then
  export GALLIUM_DRIVER=d3d12 MESA_GL_VERSION_OVERRIDE=4.5 MESA_GLSL_VERSION_OVERRIDE=450
fi
unset GOAL2C_LIB GOAL2C_HUELLA GOAL2C_RECORRIDO GOAL2C_SALTO GOAL2C_PROGRESO GOAL2C_CELULAS
# Progreso que necesita cada mecánica, aplicado justo tras el salto (tareas con todas sus etapas
# cerradas, ver game-task-h.gc; y células). 17 beach-flutflut, 103 village2-levitator,
# 105 village3-button. El flut flut exige 17 + 103 (o 105); algunos zoomer, 103 o 72 células.
case "$NIVEL" in
  snow-flut-flut) export GOAL2C_PROGRESO=17,103,105 ;;
  misty-bike|firecanyon-start|ogre-start|ogre-race|lavatube-start)
    export GOAL2C_PROGRESO=17,103,105 GOAL2C_CELULAS=72 ;;
  finalboss-start|finalboss-fight|citadel-*) export GOAL2C_PROGRESO=17,103,105 GOAL2C_CELULAS=100 ;;
esac
if [[ -n "$NIVEL" ]]; then
  export GOAL2C_SALTO="$NIVEL"
  echo "Salto a $NIVEL: espera unos segundos en el título (el mando no responde hasta el salto)"
fi
echo "Grabando en $OUT (cierra la ventana del juego para terminar)"
GOAL2C_GRABAR="$OUT" "$ROOT/build/game/gk" -v --game jak1 --config-path "$CFG" -- -boot -fakeiso \
  > "$ROOT/build/pc_test/grabacion.log" 2>&1 || true
frames=$(grep -E "^[0-9]" "$OUT" | awk '{s += $1} END {print s + 0}')
syncs=$(grep -c "^# sync" "$OUT" || true)
echo "Grabados $frames fotogramas ($((frames / 60)) s de juego, $syncs puntos de sincronía) en $OUT"
echo "Comprobación (reproduce sin ventana y compara la posición de Jak): test/goal2c/frame_check.sh"
