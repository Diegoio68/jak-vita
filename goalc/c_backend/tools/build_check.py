#!/usr/bin/env python3
"""goal2c (Jak Vita): prepara y ejecuta la app de comprobación (goalc/c_backend/check).

1. Escribe out/goal2c/jak1/check/registry_check.c con la tabla de todos los ficheros C
   validados (los .c de out/goal2c/jak1/), la misma que usa la app de la Vita.
2. Comprime las imágenes grabadas (<lote>.img -> <lote>.img.z, zlib) que deja el test
   Goal2CTests.Arm* en out/goal2c/jak1/check/.
3. Con --run: compila g2c_check para el PC (x86-64, mismo C y mismas opciones de
   comprobación que en ARM) y ejecuta los lotes pedidos. Sirve para comprobar la grabación y
   el arnés antes de llevarlos a Vita3K.

Uso: python3 goalc/c_backend/tools/build_check.py [--run lote_a ...]
"""
import argparse
import pathlib
import re
import subprocess
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT = ROOT / "out" / "goal2c" / "jak1"
CHECK = OUT / "check"
SRC = ROOT / "goalc" / "c_backend" / "check"
RT = ROOT / "goalc" / "c_backend" / "runtime"
INFO_RE = re.compile(r"^const g2c_file_info (\w+) = ", re.MULTILINE)


def c_sources():
    srcs, infos = [], []
    for c in sorted(OUT.glob("*.c")):
        if c.name == "registry.c":
            continue
        m = INFO_RE.search(c.read_text(encoding="utf-8"))
        if m:
            srcs.append(c)
            infos.append(m.group(1))
    return srcs, infos


def write_registry(infos):
    CHECK.mkdir(parents=True, exist_ok=True)
    reg = "/* Generado por goalc/c_backend/tools/build_check.py. NO EDITAR A MANO. */\n"
    reg += '#include "goal2c_rt.h"\n\n'
    for name in infos:
        reg += f"extern const g2c_file_info {name};\n"
    reg += "\nconst g2c_file_info* const g2c_check_files[] = {\n"
    for name in infos:
        reg += f"  &{name},\n"
    reg += "  0\n};\n"
    path = CHECK / "registry_check.c"
    if not path.exists() or path.read_text() != reg:
        path.write_text(reg)
    return path


def compress_images():
    for img in sorted(CHECK.glob("*.img")):
        z = img.with_suffix(".img.z")
        if not z.exists() or z.stat().st_mtime < img.stat().st_mtime:
            z.write_bytes(zlib.compress(img.read_bytes(), 6))
            print(f"[check] {z.name}: {img.stat().st_size >> 20} MB -> {z.stat().st_size >> 20} MB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", nargs="*", default=None, help="lotes a ejecutar en PC")
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args()
    srcs, infos = c_sources()
    reg = write_registry(infos)
    compress_images()
    print(f"[check] {len(srcs)} ficheros C en el registro")
    if args.run is None:
        return 0
    exe = CHECK / "g2c_check_pc"
    cmd = [args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{RT}", f"-I{SRC}",
           "-include", str(SRC / "g2c_check.h"), "-o", str(exe), str(SRC / "g2c_check.c"),
           str(reg)] + [str(s) for s in srcs] + ["-lz", "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-4000:])
        return 1
    r = subprocess.run([str(exe), str(CHECK)] + args.run)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
