#!/usr/bin/env python3
"""goal2c (Jak Vita): contador de avance de la traducción de goal_src/jak1 a C.

Cifras (todas en número de funciones; no cuenta el código top-level de cada fichero):
  - total:       funciones de los objetos de código de Jak 1 (goal_src/jak1/build/all_objs.json,
                 versión 3, con fuente .gc).
  - traducibles: las que goal2c ya traduce sin error (aunque no estén validadas en el juego).
  - en C:        las de los ficheros VALIDADOS en el juego (los .c de out/goal2c/jak1/, que son
                 los que van en la librería del modo mixto y en test/goalc/test_goal2c.cpp),
                 incluidas las G2C_FN_WRITES_PP (suspend/with-proc), que desde el suspend nativo
                 (2026-11-17) también se instalan; se informa aparte de cuántas son.

Para "total" y "traducibles" ejecuta goal2c sobre todos los ficheros en una sola sesión de goalc
(tras (mi)), escribiendo en out/goal2c/jak1/all/ (fuera de la librería del juego).

Uso:  python3 goalc/c_backend/tools/progress.py [--reuse]
      --reuse: no vuelve a ejecutar goalc, usa el último out/goal2c/jak1/all/goalc.log
Escribe la tabla en out/goal2c/jak1/progress.txt e imprime la línea de resumen.
"""
import argparse
import collections
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT = ROOT / "out" / "goal2c" / "jak1"
ALL = OUT / "all"
GOALC = ROOT / "build" / "goalc" / "goalc"
LINE_RE = re.compile(r"\[goal2c\] (\S+): (\d+) funciones traducidas, (\d+) sin traducir")
INFO_RE = re.compile(r"^const g2c_file_info \w+ = ", re.MULTILINE)
ENTRY_RE = re.compile(r'^  \{"[^"]*", \(void\*\).*$', re.MULTILINE)
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def code_objects():
    """[(obj, carpeta, fuente)] de los objetos de código de Jak 1, en orden de carga."""
    data = json.loads((ROOT / "goal_src" / "jak1" / "build" / "all_objs.json").read_text())
    objs, seen = [], set()
    for entry in data:
        name, _dgo_name, version, _dgos, folder = entry[:5]
        if version != 3 or name in seen:
            continue
        src = ROOT / "goal_src" / "jak1" / folder / f"{name}.gc"
        if src.exists():
            seen.add(name)
            objs.append((name, folder, src))
    return objs


def run_goalc(objs):
    ALL.mkdir(parents=True, exist_ok=True)
    forms = ["(mi)"]
    for name, _folder, src in objs:
        rel = src.relative_to(ROOT).as_posix()
        forms.append(f'(asm-file "{rel}" :goal2c "{(ALL / (name + ".c")).as_posix()}" :no-throw)')
    cmd = [str(GOALC), "--game", "jak1", "-c", "(begin " + " ".join(forms) + ")"]
    log = ALL / "goalc.log"
    with open(log, "w", encoding="utf-8") as f:
        subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=3600)
    return log


def validated_counts():
    """obj -> (funciones en C, funciones que escriben pp) de cada .c validado."""
    counts = {}
    for c in OUT.glob("*.c"):
        if c.name == "registry.c":
            continue
        text = c.read_text(encoding="utf-8")
        if INFO_RE.search(text):
            entries = ENTRY_RE.findall(text)
            pp = sum(1 for e in entries if "G2C_FN_WRITES_PP" in e)
            counts[c.stem] = (len(entries), pp)
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reuse", action="store_true")
    args = ap.parse_args()

    objs = code_objects()
    log = ALL / "goalc.log"
    if not args.reuse or not log.exists():
        log = run_goalc(objs)
    results = {}
    for line in ANSI_RE.sub("", log.read_text(encoding="utf-8", errors="replace")).splitlines():
        m = LINE_RE.search(line)
        if m:
            results[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    in_c = validated_counts()

    per_folder = collections.OrderedDict()
    total = translatable = done = kept_pp = 0
    failed = []
    for name, folder, _src in objs:
        if name not in results:
            failed.append(name)
            continue
        ok, bad = results[name]
        n = ok + bad
        c, pp = in_c.get(name, (0, 0))
        kept_pp += pp
        row = per_folder.setdefault(folder, [0, 0, 0, 0])  # ficheros, total, traducibles, en C
        row[0] += 1
        row[1] += n
        row[2] += ok
        row[3] += c
        total += n
        translatable += ok
        done += c

    pct = lambda a, b: 100.0 * a / b if b else 0.0
    lines = [f"{'carpeta':<34} {'ficheros':>8} {'funciones':>9} {'traducibles':>11} {'en C':>6} {'% en C':>7}"]
    for folder, (files, n, ok, c) in sorted(per_folder.items(), key=lambda kv: (-kv[1][3], -kv[1][1])):
        lines.append(f"{folder or '(raíz)':<34} {files:>8} {n:>9} {ok:>11} {c:>6} {pct(c, n):>6.1f}%")
    lines.append(f"{'TOTAL':<34} {sum(r[0] for r in per_folder.values()):>8} {total:>9} "
                 f"{translatable:>11} {done:>6} {pct(done, total):>6.1f}%")
    if failed:
        lines.append(f"sin datos (goalc no los compiló aparte): {len(failed)}: {', '.join(failed[:20])}"
                     + (" ..." if len(failed) > 20 else ""))
    if kept_pp:
        lines.append(f"de ellas escriben pp (suspend/with-proc, con el suspend nativo): {kept_pp}")
    summary = (f"Avance goal2c: {done}/{total} funciones de goal_src/jak1 en C dentro del juego "
               f"({pct(done, total):.1f} %); goal2c ya traduce {translatable}/{total} "
               f"({pct(translatable, total):.1f} %).")
    lines.append(summary)
    (OUT / "progress.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
