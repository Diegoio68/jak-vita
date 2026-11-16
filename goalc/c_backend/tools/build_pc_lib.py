#!/usr/bin/env python3
"""goal2c (Jak Vita): compila el C generado en la librería del modo mixto de PC.

Busca en --dir los .c generados por goal2c (los que definen `const g2c_file_info X_info`),
escribe un registro (registry.c) con g2c_init y la tabla de ficheros, y lo compila todo con
-DG2C_PC_MIXED en una librería dinámica que el runtime carga con GOAL2C_LIB=<librería>.

Uso:
  python3 goalc/c_backend/tools/build_pc_lib.py --dir out/goal2c/jak1 \
      --out out/goal2c/jak1/libgoal2c_jak1.so
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME_DIR = ROOT / "goalc" / "c_backend" / "runtime"
INFO_RE = re.compile(r"^const g2c_file_info (\w+) = ", re.MULTILINE)

REGISTRY_HEAD = """/* Generado por goalc/c_backend/tools/build_pc_lib.py. NO EDITAR A MANO. */
#include "goal2c_rt.h"

uint8_t* g2c_mem;
static g2c_host g_host;

uint32_t g2c_rt_symbol(const char* name) {
  return g_host.symbol(name);
}
uint32_t g2c_rt_static(const char* obj_name, int seg, int offset) {
  return g_host.static_addr(obj_name, seg, offset);
}
uint64_t g2c_host_call(uint32_t fn, int nargs, const uint64_t* args) {
  return g_host.call(fn, nargs, args);
}

"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True, help="carpeta con los .c generados")
    ap.add_argument("--out", required=True, help="librería de salida (.so)")
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args()

    src_dir = pathlib.Path(args.dir)
    sources, infos = [], []
    for c in sorted(src_dir.glob("*.c")):
        if c.name == "registry.c":
            continue
        m = INFO_RE.search(c.read_text(encoding="utf-8"))
        if m:
            sources.append(c)
            infos.append(m.group(1))
    if not sources:
        print(f"error: no hay C generado por goal2c en {src_dir}", file=sys.stderr)
        return 1

    reg = REGISTRY_HEAD
    for name in infos:
        reg += f"extern const g2c_file_info {name};\n"
    reg += "\nstatic const g2c_file_info* const g_files[] = {\n"
    for name in infos:
        reg += f"  &{name},\n"
    reg += "  0\n};\n\n"
    reg += ("__attribute__((visibility(\"default\")))\n"
            "const g2c_file_info* const* g2c_init(const g2c_host* host) {\n"
            "  g_host = *host;\n  g2c_mem = host->mem;\n  return g_files;\n}\n")
    registry = src_dir / "registry.c"
    registry.write_text(reg, encoding="utf-8")

    cmd = [args.cc, "-std=c11", "-O2", "-fPIC", "-shared", "-fvisibility=hidden",
           "-Wall", "-Wextra", "-Werror", "-DG2C_PC_MIXED", f"-I{RUNTIME_DIR}",
           "-o", args.out, str(registry)] + [str(s) for s in sources]
    print("[goal2c] " + " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode == 0:
        print(f"[goal2c] {len(sources)} ficheros -> {args.out}")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
