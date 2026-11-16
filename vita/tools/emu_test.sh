#!/usr/bin/env bash
# Prueba automática del .vpk en Vita3K (Windows) desde WSL.
#
#   1. Borra la instalación previa de JAKV00001 y el log de la app.
#   2. Instala el .vpk y lanza la app (frontend SDL: la ventana se cierra con la app).
#   3. Espera EMU_WAIT segundos y captura la ventana con PowerShell.
#   4. Copia log.txt de la app y vita3k.log del emulador.
#   5. Cierra el emulador.
#
# Resultados en build-vita/emu/: screenshot.png, log.txt, vita3k.log, vita3k_errors.txt
# Código de salida: 0 = la app seguía viva en la captura, su log no está vacío y no
# contiene "ERROR". Revisar igualmente screenshot.png: el script no mira la imagen.
#
# Variables:
#   VITA3K_EXE   ruta Windows a Vita3K.exe (si no, se lee de vita/tools/.vita3k_path, no versionado)
#   EMU_WAIT     segundos antes de la captura (por defecto 8)
#   BUILD_DIR    directorio de build (por defecto build-vita)
set -uo pipefail

TITLE_ID="JAKV00001"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLS="$ROOT/vita/tools"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-vita}"
VPK="$BUILD_DIR/jakvita.vpk"
OUT="$BUILD_DIR/emu"
EMU_WAIT="${EMU_WAIT:-8}"

die() { echo "error: $*" >&2; exit 1; }
win2wsl() { wslpath -u "$1"; }

# --- localizar Vita3K ---
if [[ -z "${VITA3K_EXE:-}" && -f "$TOOLS/.vita3k_path" ]]; then
  VITA3K_EXE="$(tr -d '\r\n' < "$TOOLS/.vita3k_path")"
fi
[[ -n "${VITA3K_EXE:-}" ]] || die "define VITA3K_EXE o escribe la ruta en vita/tools/.vita3k_path"
EXE_WSL="$(win2wsl "$VITA3K_EXE")"
[[ -f "$EXE_WSL" ]] || die "no existe $VITA3K_EXE"
EMU_DIR="$(dirname "$EXE_WSL")"

# pref-path del config.yml (donde viven ux0/ur0 emulados)
PREF_WIN="$(tr -d '\r' < "$EMU_DIR/config.yml" | sed -n 's/^pref-path: *//p' | head -1)"
if [[ -n "$PREF_WIN" ]]; then PREF="$(win2wsl "$PREF_WIN")"; else PREF="$EMU_DIR"; fi
PREF="${PREF%/}"

[[ -f "$VPK" ]] || die "no existe $VPK (compila: cmake --build build-vita)"
mkdir -p "$OUT"
rm -f "$OUT"/screenshot.png "$OUT"/log.txt "$OUT"/vita3k.log "$OUT"/vita3k_errors.txt

WIN_USER="$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')"
STAGE="/mnt/c/Users/$WIN_USER/jakvita"
mkdir -p "$STAGE"

emu_alive() { tasklist.exe /FI "IMAGENAME eq Vita3K.exe" 2>/dev/null | grep -qi vita3k.exe; }
# Cierre ordenado (WM_CLOSE) y, si no responde en 5 s, forzado. Con /F directo Vita3K
# pierde las escrituras de la app que aún tiene en búfer (log.txt queda vacío).
kill_emu() {
  emu_alive || return 0
  taskkill.exe /IM Vita3K.exe >/dev/null 2>&1 || true
  for _ in 1 2 3 4 5; do
    sleep 1
    emu_alive || return 0
  done
  echo ">> Vita3K no se cerró solo, forzando"
  taskkill.exe /IM Vita3K.exe /F >/dev/null 2>&1 || true
}

# --- 1. limpiar ---
kill_emu
sleep 1
rm -rf "$PREF/ux0/app/$TITLE_ID" "$PREF/ux0/data/jakvita/log.txt"
cp "$VPK" "$STAGE/jakvita.vpk"
VPK_WIN="$(wslpath -w "$STAGE/jakvita.vpk")"

# --- 2. instalar y lanzar ---
echo ">> Vita3K: instalando y lanzando $TITLE_ID"
( cd "$EMU_DIR" && "$EXE_WSL" --frontend sdl "$VPK_WIN" >"$OUT/vita3k_stdout.txt" 2>&1 ) &

# --- 3. esperar y capturar ---
sleep "$EMU_WAIT"
alive=0
if emu_alive; then
  alive=1
  SHOT_WIN="$(wslpath -w "$STAGE")\\screenshot.png"
  powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File "$(wslpath -w "$TOOLS/screenshot.ps1")" -ProcessName Vita3K -Out "$SHOT_WIN" | tr -d '\r'
  [[ -f "$STAGE/screenshot.png" ]] && mv "$STAGE/screenshot.png" "$OUT/screenshot.png"
else
  echo ">> la app/emulador ya había terminado antes de la captura"
fi

# --- 5. cerrar (antes de copiar logs para que vita3k.log esté completo) ---
kill_emu
wait 2>/dev/null
sleep 1

# --- 4. copiar logs ---
[[ -f "$PREF/ux0/data/jakvita/log.txt" ]] && cp "$PREF/ux0/data/jakvita/log.txt" "$OUT/log.txt"
[[ -f "$EMU_DIR/vita3k.log" ]] && tr -d '\r' < "$EMU_DIR/vita3k.log" > "$OUT/vita3k.log"
grep -E '\|(E|C)\|' "$OUT/vita3k.log" 2>/dev/null > "$OUT/vita3k_errors.txt"

# --- resumen ---
echo
echo "=== log de la app ==="
if [[ -f "$OUT/log.txt" ]]; then cat "$OUT/log.txt"; else echo "(no se generó log.txt)"; fi
echo
echo "=== errores de Vita3K ($(wc -l < "$OUT/vita3k_errors.txt") líneas, ver $OUT/vita3k_errors.txt) ==="
head -15 "$OUT/vita3k_errors.txt"
echo
echo "resultados en $OUT"

status=0
[[ $alive -eq 1 ]] || { echo "FALLO: la app no seguía viva a los ${EMU_WAIT}s"; status=1; }
[[ -s "$OUT/log.txt" ]] || { echo "FALLO: no hay log de la app (o está vacío)"; status=1; }
grep -q "ERROR" "$OUT/log.txt" 2>/dev/null && { echo "FALLO: el log de la app contiene ERROR"; status=1; }
[[ -f "$OUT/screenshot.png" ]] || [[ $alive -eq 0 ]] || { echo "FALLO: no se pudo capturar la ventana"; status=1; }
[[ $status -eq 0 ]] && echo "OK"
exit $status
