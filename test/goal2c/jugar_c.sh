#!/usr/bin/env bash
# goal2c (Jak Vita): jugar en PC con todo el código de Jak 1 que ya está en C (modo mixto).
#   test/goal2c/jugar_c.sh           # todo en C (librería out/goal2c/jak1/libgoal2c_jak1.so)
#   test/goal2c/jugar_c.sh --x86     # el mismo juego todo en x86, para comparar
# Modo normal (no determinista): tu configuración y tus partidas, sin -debug (como se validó).
# En WSL usa la GPU por d3d12. El log queda en build/pc_test/jugar_c.log.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="$ROOT/out/goal2c/jak1/libgoal2c_jak1.so"
if [[ "${1:-}" != "--x86" ]]; then
  # reconstruir la librería si algún .c es más nuevo
  if [[ ! -f "$LIB" ]] || [[ -n "$(find "$ROOT/out/goal2c/jak1" -maxdepth 1 -name '*.c' -newer "$LIB" | head -1)" ]]; then
    echo "Construyendo la librería de goal2c..."
    python3 "$ROOT/goalc/c_backend/tools/build_pc_lib.py" --dir "$ROOT/out/goal2c/jak1" --out "$LIB" > /dev/null
  fi
  export GOAL2C_LIB="$LIB"
  echo "Jugando con todo en C ($(ls "$ROOT"/out/goal2c/jak1/*.c | grep -vc registry) ficheros)"
else
  unset GOAL2C_LIB
  echo "Jugando todo en x86"
fi
unset GOAL2C_HUELLA GOAL2C_GRABAR GOAL2C_RECORRIDO GOAL2C_SALTO
if [[ -d /usr/lib/wsl/lib && -z "${GALLIUM_DRIVER:-}" ]]; then
  export GALLIUM_DRIVER=d3d12 MESA_GL_VERSION_OVERRIDE=4.5 MESA_GLSL_VERSION_OVERRIDE=450
fi
mkdir -p "$ROOT/build/pc_test"
"$ROOT/build/game/gk" -v --game jak1 -- -boot -fakeiso > "$ROOT/build/pc_test/jugar_c.log" 2>&1 || true
n=$(grep -c "funciones en C" "$ROOT/build/pc_test/jugar_c.log" || true)
echo "Fin. Objetos con funciones en C instaladas: $n. Log: build/pc_test/jugar_c.log"
grep -E "SIGSEGV|llamado desde" "$ROOT/build/pc_test/jugar_c.log" | head -5 || true
