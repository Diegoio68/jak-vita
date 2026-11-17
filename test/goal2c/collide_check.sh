#!/usr/bin/env bash
# goal2c (Jak Vita): comprobación de colisiones con el juego real.
#
# Arranca el juego sin ventana dos veces (todo en x86, y en modo mixto con la librería de
# goal2c), espera COLLIDE_SECS segundos y pide por el listener la posición de Jak
# ((-> *target* control trans), en bits). Jak en reposo sobre el suelo debe quedar en el MISMO
# sitio con x86 y con C: si las colisiones fallaran en C, caería o se movería.
#
# Uso: test/goal2c/collide_check.sh [librería.so]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/out/goal2c/jak1/libgoal2c_jak1.so}"
SECS="${COLLIDE_SECS:-40}"
OUT="$ROOT/build/pc_test"
mkdir -p "$OUT"

# Se habla con el juego por el REPL de goalc alimentado por una tubería, línea a línea:
# (mi) carga los tipos del proyecto (ya compilado: segundos), (lt) conecta con el juego y la
# consulta se compila y se envía ya conectada. (Con goalc -c no sirve: compila todo de una vez,
# antes de conectar, y sale sin esperar la respuesta.)
QUERY='(format #t "JAK ~X ~X ~X ~X~%" (the-as int (-> *target* control trans x)) (the-as int (-> *target* control trans y)) (the-as int (-> *target* control trans z)) (the-as int (-> *target* control trans w)))'

measure() {  # $1 = etiqueta, $2 = librería ("" = todo x86)
  local tag="$1" lib="$2"
  GOAL2C_LIB="$lib" timeout $((SECS + 40)) "$ROOT/build/game/gk" --no-display -v --game jak1 -- \
    -boot -fakeiso -debug > "$OUT/collide_$tag.log" 2>&1 &
  local pid=$!
  sleep "$SECS"
  { echo '(mi)'; sleep 20; echo '(lt)'; sleep 3; echo "$QUERY"; sleep 5; echo '(:exit)'; } |
    timeout 120 "$ROOT/build/goalc/goalc" --game jak1 > "$OUT/collide_${tag}_query.log" 2>&1
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  if grep -q "SIGSEGV" "$OUT/collide_$tag.log"; then
    echo "SIGSEGV"
    return
  fi
  # solo vale una respuesta completa: JAK y cuatro valores hexadecimales
  sed 's/\x1b\[[0-9;]*m//g' "$OUT/collide_${tag}_query.log" |
    grep -oE "JAK [0-9a-fA-F]+ [0-9a-fA-F]+ [0-9a-fA-F]+ [0-9a-fA-F]+" | tail -1
}

x86="$(measure x86 "")"
c="$(measure c "$LIB")"
echo "posición de Jak, todo x86:      ${x86:-(sin respuesta)}"
echo "posición de Jak, con goal2c:    ${c:-(sin respuesta)}"
if [[ "$x86" =~ ^JAK\ [0-9a-fA-F]+\ [0-9a-fA-F]+\ [0-9a-fA-F]+\ [0-9a-fA-F]+$ && "$x86" == "$c" ]]; then
  echo "OK"
  exit 0
fi
echo "FALLO: la posición difiere o no hubo respuesta (logs en $OUT/collide_*.log)"
exit 1
