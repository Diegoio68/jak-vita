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
#   FRAME_RECORRIDO: guion de entrada de mando (por defecto test/goal2c/recorrido.txt: Jak anda,
#     salta, pega... por Sandover; vacío = Jak en reposo). Ver scripted_pad en goal2c_jak1.cpp.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/out/goal2c/jak1/libgoal2c_jak1.so}"
SECS_DEFAULT=45
MIN="${FRAME_MIN:-600}"
# recorrido oficial (grabado por Diego con task boot-game GRABAR=...) si existe
if [[ -f "$ROOT/test/goal2c/recorrido_oficial.txt" ]]; then
  RECORRIDO="${FRAME_RECORRIDO-$ROOT/test/goal2c/recorrido_oficial.txt}"
else
  RECORRIDO="${FRAME_RECORRIDO-$ROOT/test/goal2c/recorrido.txt}"
fi
OUT="$ROOT/build/pc_test"
# con una grabación (sin-repetir): tiempo para reproducirla entera (sin ventana van ~300 fps)
if [[ -n "$RECORRIDO" ]] && grep -q "^# sin-repetir" "$RECORRIDO"; then
  rec_frames=$(grep -E "^[0-9]" "$RECORRIDO" | awk '{s += $1} END {print s + 0}')
  SECS_DEFAULT=$(( rec_frames / 250 + 30 > 45 ? rec_frames / 250 + 30 : 45 ))
fi
SECS="${FRAME_SECS:-$SECS_DEFAULT}"
mkdir -p "$OUT"

run() {  # $1 = etiqueta, $2 = librería ("" = todo x86)
  # configuración limpia (sin partidas ni ajustes del usuario), como en la grabación
  rm -rf "$OUT/cfg_$1"
  GOAL2C_HUELLA=1 GOAL2C_LIB="$2" GOAL2C_RECORRIDO="$RECORRIDO" timeout "$SECS" "$ROOT/build/game/gk" --no-display -v \
    --game jak1 --config-path "$OUT/cfg_$1" -- -boot -fakeiso > "$OUT/frame_$1.log" 2>&1
  sed 's/\x1b\[[0-9;]*m//g' "$OUT/frame_$1.log" |
    grep -oE "\[huella\] [0-9]+ [0-9a-f]+ estricto [0-9a-f]+" |
    awk '{print $2, $3}' > "$OUT/frame_$1.txt"
  sed 's/\x1b\[[0-9;]*m//g' "$OUT/frame_$1.log" |
    grep -oE "\[huella\] [0-9]+ [0-9a-f]+ estricto [0-9a-f]+" |
    awk '{print $2, $5}' > "$OUT/frame_$1_estricto.txt"
}

# La referencia x86 es determinista: se reutiliza mientras gk y los objetos del juego no
# cambien (huella_ref_*.txt, con la marca de tiempo de gk y de out/jak1/obj en el nombre).
stamp="$(stat -c %Y "$ROOT/build/game/gk")_$(ls -t "$ROOT/out/jak1/obj" | head -1 |
  xargs -I{} stat -c %Y "$ROOT/out/jak1/obj/{}")_$SECS"
if [[ -n "$RECORRIDO" ]]; then
  stamp="${stamp}_r$(stat -c %Y "$RECORRIDO")"
fi
REF="$OUT/huella_ref_$stamp"
if [[ "${CHECK_X86_TWICE:-0}" != 1 && -s "$REF.txt" && -s "$REF.estricto.txt" ]]; then
  cp "$REF.txt" "$OUT/frame_a.txt"
  cp "$REF.estricto.txt" "$OUT/frame_a_estricto.txt"
  echo "(referencia x86 reutilizada: $REF.txt)"
else
  run a ""
  rm -f "$OUT"/huella_ref_*
  cp "$OUT/frame_a.txt" "$REF.txt"
  cp "$OUT/frame_a_estricto.txt" "$REF.estricto.txt"
fi
if [[ "${CHECK_X86_TWICE:-0}" == 1 ]]; then
  run b ""
  label="todo x86 (2.ª vez)"
else
  run b "$LIB"
  label="con goal2c"
fi
# control de sincronía del recorrido grabado: la posición de Jak debe ser la de la grabación
for f in a b; do
  if [[ -f "$OUT/frame_$f.log" ]] && grep -q "DESINCRONIZADO" "$OUT/frame_$f.log"; then
    echo "AVISO ($f): $(sed 's/\x1b\[[0-9;]*m//g' "$OUT/frame_$f.log" | grep -o "DESINCRONIZADO.*" | head -1)"
  fi
done
sync_b=$(sed 's/\x1b\[[0-9;]*m//g' "$OUT/frame_b.log" | grep -o "sincronía: .*" | tail -1)
[[ -n "$sync_b" ]] && echo "recorrido ($label): $sync_b"
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
