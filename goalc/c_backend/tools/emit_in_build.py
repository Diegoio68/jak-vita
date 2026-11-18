#!/usr/bin/env python3
"""goal2c (Jak Vita): traduce a C, DURANTE la compilación del juego, los ficheros de
test/goal2c/emit_in_build.txt (res-h, level-h).

Su objeto depende del estado de tipos en el punto de la compilación en que el juego los compila
(el inspect autogenerado mira si ciertos tipos existen ya). Transpilados después de (mi), como
hace validate_batch.py, el objeto no sale idéntico. Aquí se fuerza su recompilación dentro de un
(mi) con GOAL2C_EMIT_DIR/GOAL2C_EMIT_ONLY (ver Compiler::asm_file), y se exige que el objeto de
goalc (<obj>.c.o) sea idéntico al del juego.

Uso: python3 goalc/c_backend/tools/emit_in_build.py
"""
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT = ROOT / "out" / "goal2c" / "jak1"
LIST = ROOT / "test" / "goal2c" / "emit_in_build.txt"


def main():
    entries = [l.split() for l in LIST.read_text().splitlines() if l.strip() and not l.startswith("#")]
    for _name, src in entries:
        (ROOT / src).touch()  # que (mi) los recompile
    env = dict(os.environ, GOAL2C_EMIT_DIR=str(OUT), GOAL2C_EMIT_ONLY=",".join(n for n, _ in entries))
    r = subprocess.run([str(ROOT / "build" / "goalc" / "goalc"), "--game", "jak1", "-c", "(mi)"],
                       cwd=ROOT, env=env, capture_output=True, text=True, timeout=1800)
    ok = True
    for name, _src in entries:
        mine, game = OUT / f"{name}.c.o", ROOT / "out" / "jak1" / "obj" / f"{name}.o"
        same = mine.exists() and game.exists() and mine.read_bytes() == game.read_bytes()
        print(f"[emit] {name}: {'objeto idéntico' if same else 'ERROR: objeto distinto'}")
        ok &= same
    if r.returncode != 0:
        print(r.stdout[-2000:])
    return 0 if ok and r.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
