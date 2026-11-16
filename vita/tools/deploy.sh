#!/usr/bin/env bash
# Despliegue de Jak Vita a una PS Vita con vitacompanion (FTP :1337, comandos :1338).
#
# Uso:
#   VITA_IP=192.168.1.50 vita/tools/deploy.sh            # sube el .vpk, lo instala y lo lanza
#   VITA_IP=192.168.1.50 vita/tools/deploy.sh --eboot    # solo reemplaza eboot.bin (rápido, ya instalado)
#   VITA_IP=192.168.1.50 vita/tools/deploy.sh --log      # descarga ux0:data/jakvita/log.txt
#   vita/tools/deploy.sh --vita3k                        # copia el .vpk a C:\Users\<usuario>\jakvita\
#
# Variables:
#   VITA_IP        IP de la Vita (obligatoria salvo --vita3k)
#   BUILD_DIR      directorio de build (por defecto build-vita)
#   INSTALL_WAIT   segundos a esperar a que termine la instalación del vpk (por defecto 15)
#   WIN_USER       usuario de Windows para --vita3k (se autodetecta si no se da)
set -euo pipefail

TITLE_ID="JAKV00001"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-vita}"
VPK="$BUILD_DIR/jakvita.vpk"
EBOOT="$BUILD_DIR/eboot.bin"
FTP_PORT=1337
CMD_PORT=1338
INSTALL_WAIT="${INSTALL_WAIT:-15}"
REMOTE_VPK="ux0:data/jakvita/jakvita.vpk"

die() { echo "error: $*" >&2; exit 1; }

need_ip() { [[ -n "${VITA_IP:-}" ]] || die "define VITA_IP (ej. VITA_IP=192.168.1.50 $0)"; }

# Envía un comando a vitacompanion (puerto 1338). Termina en '\n'.
vcmd() {
  echo ">> $*"
  printf '%s\n' "$*" | nc -w 3 "$VITA_IP" "$CMD_PORT" || die "no responde vitacompanion en $VITA_IP:$CMD_PORT"
}

# Sube un fichero por FTP. $2 es la ruta remota estilo 'ux0:app/XXX/eboot.bin'.
ftp_put() {
  local src="$1" dst="$2"
  echo ">> FTP $src -> $dst"
  # vitacompanion acepta rutas 'ux0:/...'; curl necesita la ruta tras el host.
  curl --fail --silent --show-error --ftp-create-dirs -T "$src" \
    "ftp://$VITA_IP:$FTP_PORT/${dst/:/:\/}"
}

ftp_get() {
  local src="$1" dst="$2"
  curl --fail --silent --show-error -o "$dst" "ftp://$VITA_IP:$FTP_PORT/${src/:/:\/}"
}

mode="${1:-vpk}"
case "$mode" in
  vpk|--vpk)
    need_ip
    [[ -f "$VPK" ]] || die "no existe $VPK (compila antes: cmake --build build-vita)"
    vcmd destroy
    sleep 1
    ftp_put "$VPK" "$REMOTE_VPK"
    vcmd vpk "$REMOTE_VPK"
    echo ">> esperando ${INSTALL_WAIT}s a que se instale..."
    sleep "$INSTALL_WAIT"
    vcmd launch "$TITLE_ID"
    ;;
  --eboot)
    need_ip
    [[ -f "$EBOOT" ]] || die "no existe $EBOOT"
    vcmd destroy
    sleep 1
    ftp_put "$EBOOT" "ux0:app/$TITLE_ID/eboot.bin"
    vcmd launch "$TITLE_ID"
    ;;
  --log)
    need_ip
    out="$ROOT/build-vita/log.txt"
    ftp_get "ux0:data/jakvita/log.txt" "$out"
    echo ">> guardado en $out"
    cat "$out"
    ;;
  --vita3k)
    [[ -f "$VPK" ]] || die "no existe $VPK"
    if [[ -z "${WIN_USER:-}" ]]; then
      WIN_USER="$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')" || true
    fi
    [[ -n "${WIN_USER:-}" ]] || die "no pude detectar el usuario de Windows; define WIN_USER"
    dst="/mnt/c/Users/$WIN_USER/jakvita"
    mkdir -p "$dst"
    cp "$VPK" "$dst/"
    echo ">> copiado a $dst/jakvita.vpk"
    ;;
  -h|--help)
    sed -n '2,15p' "$0"
    ;;
  *)
    die "modo desconocido: $mode (usa --help)"
    ;;
esac
