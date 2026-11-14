#!/usr/bin/env python3
"""Golden test: our TJA parser against YataiDON's, chart by chart.

    python3 tests/golden.py [FOLDER...]

Every .tja under the folders (default: the test charts, YataiDON's bundled songs
and, when present, taikojiro_reference/) is copied to build/golden/ as UTF-8 and
parsed by both build/tjadump (ours, strict mode) and build/yatai/yatai_dump
(YataiDON's own code). The dumps list every note, bar line, timeline event and
tempo point with full precision and must be identical.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "packer"))
from taikopack import tja_meta  # noqa: E402

OUT = os.path.join(ROOT, "build", "golden")
OURS = os.path.join(ROOT, "build", "tjadump")
REF = os.path.join(ROOT, "build", "yatai", "yatai_dump")
DEFAULT = ["tests/data/songs", "third_party/YataiDON/Songs", "taikojiro_reference"]


def run(tool, files):
    """{file name: dump text}; a file the tool dies on maps to None."""
    proc = subprocess.run([tool, *files], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if proc.returncode != 0 and len(files) > 1:
        out = {}
        for f in files:
            out.update(run(tool, [f]))
        return out
    if proc.returncode != 0:
        return {os.path.basename(files[0]): None}
    out = {}
    for block in proc.stdout.decode("utf-8", errors="replace").split("#file ")[1:]:
        name, _, body = block.partition("\n")
        out[name] = body
    return out


def main():
    folders = sys.argv[1:] or [os.path.join(ROOT, d) for d in DEFAULT]
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        os.remove(os.path.join(OUT, old))
    sources = {}
    for folder in folders:
        for dirpath, dirnames, names in os.walk(folder):
            dirnames.sort()
            for name in sorted(names):
                if name.lower().endswith(".tja"):
                    with open(os.path.join(dirpath, name), "rb") as f:
                        text, _ = tja_meta.decode_tja(f.read())
                    copy = f"{len(sources):04d}.tja"
                    with open(os.path.join(OUT, copy), "w", encoding="utf-8", newline="") as f:
                        f.write(text)
                    sources[copy] = os.path.relpath(os.path.join(dirpath, name), ROOT)
    files = [os.path.join(OUT, n) for n in sorted(sources)]
    ours, ref = {}, {}
    for i in range(0, len(files), 50):
        ours.update(run(OURS, files[i:i + 50]))
        ref.update(run(REF, files[i:i + 50]))

    same = different = ref_failed = courses = notes = 0
    for name in sorted(sources):
        if ref.get(name) is None:
            ref_failed += 1
            print(f"reference failed: {sources[name]}")
        elif ours.get(name) == ref[name]:
            same += 1
            courses += ref[name].count("\n== course")
            notes += ref[name].count("\nN ")
        else:
            different += 1
            print(f"DIFFERENT: {sources[name]}")
            a, b = (ref[name] or "").split("\n"), (ours.get(name) or "<crashed>").split("\n")
            shown = 0
            for la, lb in zip(a, b):
                if la != lb and shown < 3:
                    print(f"   ref : {la[:230]}\n   ours: {lb[:230]}")
                    shown += 1
            if len(a) != len(b):
                print(f"   {len(a)} reference lines, {len(b)} of ours")
    print(f"{same} charts identical ({courses} courses, {notes} notes and bar lines), "
          f"{different} different, {ref_failed} the reference could not parse")
    if same == 0:
        print("nothing was compared: no charts found, or the reference tool is missing")
    return 1 if different or same == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
