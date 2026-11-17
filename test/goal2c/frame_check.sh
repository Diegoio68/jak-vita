#!/usr/bin/env bash
# goal2c (Jak Vita): huella de cada fotograma, todo x86 frente a modo mixto.
#
# Arranca el juego sin ventana dos veces durante FRAME_SECS segundos con GOAL2C_HUELLA=1: el
# runtime escribe en el log "[huella] <n> <hash>" con un hash de la cadena DMA que el juego
# envía al renderer en cada fotograma (geometría, matrices de cámara, colores, animación... de
# todo lo que se dibuja). Compara los fotogramas que tengan las dos ejecuciones: deben ser
# idénticos. Requisito: los objetos de entrada a C viven fuera de los heaps de GOAL, así que el
# heap (y las direcciones que hay en la cadena DMA) es el mismo con y sin goal2c.
# Determinismo (con GOAL2C_HUELLA): reloj virtual de 1/60 s por fotograma, cargas del IOP que
# terminan en el fotograma en que se consultan, y sin -debug (el texto de depuración cambia).
# El hash enmascara campos que el hardware ignora y que el código original deja sin inicializar
# (4.ª palabra de STQ); el hash estricto se da como información.
#
# Uso: test/goal2c/frame_check.sh [librería.so]
#   CHECK_X86_TWICE=1: compara dos ejecuciones todo x86 (comprueba que la huella es determinista)
#   FRAME_MIN: fotogramas comunes mínimos para dar el resultado por bueno (por defecto 600)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/out/goal2c/jak1/libgoal2c_jak1.so}"
SECS="${FRAME_SECS:-45}"
MIN="${FRAME_MIN:-600}"
OUT="$ROOT/build/pc_test"
mkdir -p "$OUT"

run() {  # $1 = etiqueta, $2 = librería ("" = todo x86)
  GOAL2C_HUELLA=1 GOAL2C_LIB="$2" timeout "$SECS" "$ROOT/build/game/gk" --no-display -v \
    --game jak1 -- -boot -fakeiso > "$OUT/frame_$1.log" 2>&1
  sed 's/\x1b\[[0-9;]*m//g' "$OUT/frame_$1.log" |
    grep -oE "\[huella\] [0-9]+ [0-9a-f]+ estricto [0-9a-f]+" |
    awk '{print $2, $3}' > "$OUT/frame_$1.txt"
  sed 's/\x1b\[[0-9;]*m//g' "$OUT/frame_$1.log" |
    grep -oE "\[huella\] [0-9]+ [0-9a-f]+ estricto [0-9a-f]+" |
    awk '{print $2, $5}' > "$OUT/frame_$1_estricto.txt"
}

run a ""
if [[ "${CHECK_X86_TWICE:-0}" == 1 ]]; then
  run b ""
  label="todo x86 (2.ª vez)"
else
  run b "$LIB"
  label="con goal2c"
fi
if grep -q "SIGSEGV" "$OUT/frame_b.log"; then
  echo "FALLO: SIGSEGV ($label)"
  grep -E "SIGSEGV|llamado desde" "$OUT/frame_b.log" | head -5
  exit 1
fi

na=$(wc -l < "$OUT/frame_a.txt")
nb=$(wc -l < "$OUT/frame_b.txt")
n=$(( na < nb ? na : nb ))
first_diff=$(paste -d' ' <(head -n "$n" "$OUT/frame_a.txt") <(head -n "$n" "$OUT/frame_b.txt") |
  awk '$1 != $3 || $2 != $4 {print $1; exit}')
strict_diff=$(paste -d' ' <(head -n "$n" "$OUT/frame_a_estricto.txt") \
  <(head -n "$n" "$OUT/frame_b_estricto.txt") | awk '$2 != $4 {c++} END {print c+0}')
echo "fotogramas: todo x86 $na, $label $nb, comparados $n"
echo "(informativo) fotogramas distintos sin enmascarar campos indefinidos: $strict_diff"
if [[ -n "$first_diff" ]]; then
  echo "FALLO: el fotograma $first_diff difiere"
  echo "  x86:   $(sed -n "$((first_diff + 1))p" "$OUT/frame_a.txt")"
  echo "  otro:  $(sed -n "$((first_diff + 1))p" "$OUT/frame_b.txt")"
  echo "  logs: $OUT/frame_a.log $OUT/frame_b.log"
  exit 1
fi
if [[ "$n" -lt "$MIN" ]]; then
  echo "FALLO: solo $n fotogramas comunes (mínimo $MIN)"
  exit 1
fi
echo "OK"
