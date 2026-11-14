#!/usr/bin/env python3
"""Replay test: our judge against YataiDON's JudgeCore.

    python3 tests/replay.py [--limit N]

Uses the UTF-8 chart copies in build/golden/ (run `make golden` first). For
every course and six simulated players (perfect, good, average, sloppy, random
mashing, idle), build/judgesim plays the chart and records the inputs;
build/yatai/yatai_judge replays the same inputs through YataiDON's code. Every
judgment, every branch decision and the final result must be identical.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHARTS = os.path.join(ROOT, "build", "golden")
LOGS = os.path.join(ROOT, "build", "replay", "logs")


def runs(tool, files):
    proc = subprocess.run([tool, LOGS, *files], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if proc.returncode != 0 and len(files) > 1:
        out = {}
        for f in files:
            out.update(runs(tool, [f]))
        return out
    if proc.returncode != 0:
        return {os.path.basename(files[0]): None}
    out = {}
    for block in proc.stdout.decode().split("#run ")[1:]:
        head, _, body = block.partition("\n")
        out[head] = body
    return out


def main():
    limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else 0
    files = sorted(os.path.join(CHARTS, n) for n in os.listdir(CHARTS) if n.endswith(".tja"))
    if limit:
        files = files[:limit]
    os.makedirs(LOGS, exist_ok=True)
    same = different = judgments = branches = 0
    for i in range(0, len(files), 40):
        batch = files[i:i + 40]
        ours = runs(os.path.join(ROOT, "build", "judgesim"), batch)
        ref = runs(os.path.join(ROOT, "build", "yatai", "yatai_judge"), batch)
        for key in sorted(ref):
            if ref[key] is not None and ours.get(key) == ref[key]:
                same += 1
                judgments += ref[key].count("\nJ ") + ref[key].startswith("J ")
                for line in ref[key].split("\n"):
                    if line.startswith("H"):
                        branches += len(line.split()) - 1
            else:
                different += 1
                if different <= 10:
                    print(f"DIFFERENT: {key}")
                    a, b = (ref[key] or "<crashed>").split("\n"), (ours.get(key) or "<missing>").split("\n")
                    for la, lb in zip(a, b):
                        if la != lb:
                            print(f"   ref : {la}\n   ours: {lb}")
                            break
        for name in os.listdir(LOGS):
            os.remove(os.path.join(LOGS, name))
    print(f"{same} runs identical ({judgments} judgments, {branches} branch decisions), {different} different")
    if same == 0:
        print("nothing was compared: run tests/golden.py first")
    return 1 if different or same == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
