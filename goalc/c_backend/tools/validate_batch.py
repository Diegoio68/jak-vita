#!/usr/bin/env python3
"""goal2c (Jak Vita): valida un lote de ficheros de Jak 1 en C dentro del juego.

Para cada objeto de código de las carpetas pedidas (all_objs.json, versión 3, con fuente .gc)
que no esté ya validado:
  1. Lo transpila (una sola sesión de goalc tras (mi)) a out/goal2c/jak1/batch/.
  2. Exige que el objeto que generó goalc sea IDÉNTICO al que carga el juego
     (out/jak1/obj/<obj>.o) y que el C compile limpio (-Wall -Wextra -Werror) en PC y con el
     gcc de la Vita (ARM).
Con los que pasan, todos a la vez en la librería del modo mixto:
  3. test/goal2c/boot_smoke.sh, test/goal2c/frame_check.sh (huella de cada fotograma idéntica a
     x86) y test/goal2c/collide_check.sh (Jak y cámara idénticos a x86).
  4. Si algo falla, biseca el lote hasta aislar los ficheros culpables (se quedan fuera y se
     informa de ellos).
Los que pasan se copian a out/goal2c/jak1/ y se añaden a test/goal2c/game_only.txt.

Uso:  python3 goalc/c_backend/tools/validate_batch.py engine/gfx engine/draw ...
      --objs a,b,c       objetos concretos en vez de (o además de) carpetas
      --dry-run          solo los pasos 1-2 (no toca la librería ni game_only.txt)
"""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT = ROOT / "out" / "goal2c" / "jak1"
BATCH = OUT / "batch"
GAME_ONLY = ROOT / "test" / "goal2c" / "game_only.txt"
GOALC = ROOT / "build" / "goalc" / "goalc"
RT = ROOT / "goalc" / "c_backend" / "runtime"
ARM_CC = "/usr/local/vitasdk/bin/arm-vita-eabi-gcc"
LIB = OUT / "libgoal2c_jak1.so"
INFO_RE = re.compile(r"^const g2c_file_info \w+ = ", re.MULTILINE)
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def log(msg):
    print(f"[lote] {msg}", flush=True)


def code_objects():
    data = json.loads((ROOT / "goal_src" / "jak1" / "build" / "all_objs.json").read_text())
    objs, seen = [], set()
    for entry in data:
        name, _dgo, version, _dgos, folder = entry[:5]
        if version != 3 or name in seen:
            continue
        src = ROOT / "goal_src" / "jak1" / folder / f"{name}.gc"
        if src.exists():
            seen.add(name)
            objs.append((name, folder, src))
    return objs


def validated():
    """Objetos ya validados: los .c de out/goal2c/jak1 (con casos o en game_only.txt)."""
    done = set()
    for c in OUT.glob("*.c"):
        if c.name != "registry.c" and INFO_RE.search(c.read_text(encoding="utf-8")):
            done.add(c.stem)
    return done


def transpile(objs):
    BATCH.mkdir(parents=True, exist_ok=True)
    forms = ["(mi)"]
    for name, _folder, src in objs:
        rel = src.relative_to(ROOT).as_posix()
        forms.append(f'(asm-file "{rel}" :goal2c "{(BATCH / (name + ".c")).as_posix()}" :no-throw)')
    cmd = [str(GOALC), "--game", "jak1", "-c", "(begin " + " ".join(forms) + ")"]
    logf = BATCH / "goalc.log"
    with open(logf, "w", encoding="utf-8") as f:
        subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=3600)
    return ANSI_RE.sub("", logf.read_text(encoding="utf-8", errors="replace"))


def compile_ok(c_file, cc, extra):
    out = c_file.with_suffix(".chk.o")
    r = subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *extra,
                        "-c", f"-I{RT}", str(c_file), "-o", str(out)],
                       capture_output=True, text=True)
    out.unlink(missing_ok=True)
    return r.returncode == 0, r.stderr[-2000:]


def static_checks(objs):
    """Pasos 1-2 por fichero. Devuelve (aceptados, rechazados {obj: motivo})."""
    ok, bad = [], {}
    for name, folder, src in objs:
        c = BATCH / f"{name}.c"
        if not c.exists():
            bad[name] = "goalc no generó el C"
            continue
        game_obj = ROOT / "out" / "jak1" / "obj" / f"{name}.o"
        if not game_obj.exists() or (BATCH / f"{name}.c.o").read_bytes() != game_obj.read_bytes():
            bad[name] = "el objeto transpilado no es idéntico al del juego"
            continue
        good, err = compile_ok(c, "cc", [])
        if not good:
            bad[name] = "el C no compila en PC:\n" + err
            continue
        good, err = compile_ok(c, ARM_CC, ["-mcpu=cortex-a9", "-mfpu=neon"])
        if not good:
            bad[name] = "el C no compila para ARM:\n" + err
            continue
        ok.append((name, folder, src))
    return ok, bad


def install(objs):
    for name, _f, _s in objs:
        for ext in (".c", ".c.o"):
            shutil.copy(BATCH / f"{name}{ext}", OUT / f"{name}{ext}")


def uninstall(objs):
    for name, _f, _s in objs:
        for ext in (".c", ".c.o"):
            (OUT / f"{name}{ext}").unlink(missing_ok=True)


def game_checks(label):
    """Librería + prueba de humo + huella + colisión/cámara. True si todo pasa."""
    r = subprocess.run([sys.executable, str(ROOT / "goalc/c_backend/tools/build_pc_lib.py"),
                        "--dir", str(OUT), "--out", str(LIB)], capture_output=True, text=True)
    if r.returncode != 0:
        log(f"{label}: la librería no compila\n{r.stdout[-1500:]}{r.stderr[-1500:]}")
        return False
    for script in ("boot_smoke.sh", "frame_check.sh", "collide_check.sh"):
        r = subprocess.run(["bash", str(ROOT / "test/goal2c" / script), str(LIB)],
                           capture_output=True, text=True, cwd=ROOT)
        tail = "\n".join(r.stdout.strip().splitlines()[-4:])
        if r.returncode != 0:
            log(f"{label}: FALLA {script}\n{tail}")
            return False
        log(f"{label}: {script} OK")
    return True


def find_culprits(objs):
    """Con objs instalados y fallando: biseca y devuelve los culpables (los deja sin instalar)."""
    if len(objs) == 1:
        uninstall(objs)
        return objs
    half = len(objs) // 2
    a, b = objs[:half], objs[half:]
    uninstall(b)
    culprits = []
    if not game_checks(f"bisección {len(a)} ({a[0][0]}..)"):
        culprits += find_culprits(a)
    install(b)
    if not game_checks(f"bisección {len(b)} ({b[0][0]}..)"):
        culprits += find_culprits(b)
    return culprits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folders", nargs="*")
    ap.add_argument("--objs", default="")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    wanted_objs = {o for o in args.objs.split(",") if o}
    done = validated()
    objs = [(n, f, s) for n, f, s in code_objects()
            if n not in done and (n in wanted_objs or any(f == d or f.startswith(d + "/")
                                                          for d in args.folders))]
    if not objs:
        log("nada que validar")
        return 0
    log(f"{len(objs)} ficheros: transpilando")
    text = transpile(objs)
    skipped = re.findall(r"\[goal2c\]   sin traducir: (.*)", text)
    ok, bad = static_checks(objs)
    for name, why in bad.items():
        log(f"rechazado {name}: {why}")
    log(f"{len(ok)} pasan objeto idéntico + C limpio (PC y ARM); {len(bad)} rechazados; "
        f"{len(skipped)} funciones sin traducir en el lote")
    if args.dry_run or not ok:
        return 0

    install(ok)
    culprits = []
    if not game_checks(f"lote completo ({len(ok)})"):
        culprits = find_culprits(ok)
        if culprits:
            log("culpables: " + ", ".join(c[0] for c in culprits))
        good = [o for o in ok if o not in culprits]
        if not game_checks(f"lote sin culpables ({len(good)})"):
            log("ERROR: el lote sin culpables sigue fallando; no se valida nada")
            uninstall(ok)
            return 1
        ok = good
    with open(GAME_ONLY, "a", encoding="utf-8") as f:
        for name, folder, src in ok:
            f.write(f"{name} {src.relative_to(ROOT).as_posix()}\n")
    log(f"validados {len(ok)} ficheros (añadidos a {GAME_ONLY.relative_to(ROOT)})")
    for c in culprits:
        log(f"fuera del lote (falla en el juego): {c[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
