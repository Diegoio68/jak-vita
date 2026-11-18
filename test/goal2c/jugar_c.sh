#!/usr/bin/env bash
# goal2c (Jak Vita): jugar en PC con todo el código de Jak 1 que ya está en C (modo mixto).
#   test/goal2c/jugar_c.sh           # todo en C, partida GRABADA (reproducible si algo falla)
#   test/goal2c/jugar_c.sh --x86     # lo mismo, todo en x86 (para comparar)
#   test/goal2c/jugar_c.sh --libre   # todo en C, modo normal sin grabar (tiempo real)
#
# Partida grabada: cada sesión va a build/pc_test/sesiones/<fecha>/ con
#   cfg_inicial/  copia de tu configuración y tus partidas al empezar (no se toca)
#   cfg/          la que usa el juego (guarda ahí; tu ~/.config/OpenGOAL no cambia)
#   entrada.txt   el mando de toda la partida (formato de recorrido, con puntos de sincronía)
#   juego.log     el log del juego
# El juego va en modo determinista (reloj virtual de 1/60 s por fotograma, cargas síncronas): si
# el PC no llega a 60 fps va algo más lento, y hay pequeñas pausas en las cargas. Si algo falla:
#   test/goal2c/reproducir.sh build/pc_test/sesiones/<fecha>
# repite la partida sin ventana con la misma entrada y la misma configuración.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="$ROOT/out/goal2c/jak1/libgoal2c_jak1.so"
MODO="${1:-}"
if [[ "$MODO" != "--x86" ]]; then
  if [[ ! -f "$LIB" ]] || [[ -n "$(find "$ROOT/out/goal2c/jak1" -maxdepth 1 -name '*.c' -newer "$LIB" | head -1)" ]]; then
    echo "Construyendo la librería de goal2c..."
    python3 "$ROOT/goalc/c_backend/tools/build_pc_lib.py" --dir "$ROOT/out/goal2c/jak1" --out "$LIB" > /dev/null
  fi
  export GOAL2C_LIB="$LIB"
else
  unset GOAL2C_LIB
fi
unset GOAL2C_HUELLA GOAL2C_GRABAR GOAL2C_RECORRIDO GOAL2C_SALTO GOAL2C_PROGRESO GOAL2C_CELULAS
if [[ -d /usr/lib/wsl/lib && -z "${GALLIUM_DRIVER:-}" ]]; then
  export GALLIUM_DRIVER=d3d12 MESA_GL_VERSION_OVERRIDE=4.5 MESA_GLSL_VERSION_OVERRIDE=450
fi
mkdir -p "$ROOT/build/pc_test"
if [[ "$MODO" == "--libre" ]]; then
  echo "Jugando con todo en C, modo normal (sin grabar)"
  "$ROOT/build/game/gk" -v --game jak1 -- -boot -fakeiso > "$ROOT/build/pc_test/jugar_c.log" 2>&1 || true
  grep -E "SIGSEGV|\[die\]|llamado desde" "$ROOT/build/pc_test/jugar_c.log" | head -5 || true
  exit 0
fi
SES="$ROOT/build/pc_test/sesiones/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$SES/cfg_inicial/OpenGOAL"
if [[ -d "$HOME/.config/OpenGOAL" ]]; then
  cp -a "$HOME/.config/OpenGOAL/." "$SES/cfg_inicial/OpenGOAL/"
fi
cp -a "$SES/cfg_inicial" "$SES/cfg"
echo "${GOAL2C_LIB:+c}${GOAL2C_LIB:-x86}" > "$SES/modo"
echo "Jugando ($( [[ -n "${GOAL2C_LIB:-}" ]] && echo 'todo en C' || echo 'todo en x86')), partida grabada en $SES"
GOAL2C_GRABAR="$SES/entrada.txt" "$ROOT/build/game/gk" -v --game jak1 --config-path "$SES/cfg" \
  -- -boot -fakeiso > "$SES/juego.log" 2>&1 || true
frames=$(grep -E "^[0-9]" "$SES/entrada.txt" 2>/dev/null | awk '{s += $1} END {print s + 0}')
echo "Fin: $((frames / 60)) s de juego grabados."
if grep -qE "SIGSEGV|\[die\]" "$SES/juego.log"; then
  echo "EL JUEGO FALLÓ:"
  grep -E "SIGSEGV|\[die\]|llamado desde" "$SES/juego.log" | head -5
  echo "Para reproducirlo: test/goal2c/reproducir.sh $SES"
fi
