#!/usr/bin/env python3
"""Assembles the openFPGA folder layout for the SD card and zips it.

    make_release.py [--rbf FILE.rbf] [--firmware build/pocket/taikopocket.bin] [-o build/release]

Result (copy onto the card's root):
    Cores/nyannurs.TaikoPocket/   core definition, bitstream (name from core.json)
    Platforms/                    taikopocket.json and its image
    Assets/taikopocket/common/    taikopocket.bin (the game); put library.tpl here
"""

import argparse
import json
import os
import shutil
import sys
import zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
CORE = "nyannurs.TaikoPocket"


def reverse_bits(data: bytes) -> bytes:
    """The Pocket wants every byte of the Quartus .rbf bit-reversed (.rbf_r)."""
    table = bytes(int(f"{i:08b}"[::-1], 2) for i in range(256))
    return data.translate(table)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rbf", help="bitstream from Quartus (gateware/projects/output_files/*.rbf)")
    ap.add_argument("--firmware", default=os.path.join(ROOT, "build", "pocket", "taikopocket.bin"))
    ap.add_argument("-o", "--output", default=os.path.join(ROOT, "build", "release"))
    args = ap.parse_args()

    out = args.output
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "gateware", "pkg", "pocket"), out,
                    ignore=shutil.ignore_patterns(".gitkeep", ".DS_Store"))
    assets = os.path.join(out, "Assets", "taikopocket", "common")
    os.makedirs(assets, exist_ok=True)
    if not os.path.exists(args.firmware):
        print(f"firmware not found: {args.firmware} (run `make pocket`)")
        return 1
    shutil.copy(args.firmware, os.path.join(assets, "taikopocket.bin"))
    with open(os.path.join(out, "Cores", CORE, "core.json")) as f:
        bitstream = json.load(f)["core"]["cores"][0]["filename"]
    if len(bitstream) > 15:  # APF limit; a longer name gives "can't find bitstream"
        print(f"core.json: bitstream filename '{bitstream}' is longer than 15 characters")
        return 1
    if args.rbf is not None:
        if not os.path.isfile(args.rbf) or os.path.getsize(args.rbf) == 0:
            print(f"bitstream not found or empty: '{args.rbf}'")
            return 1
        with open(args.rbf, "rb") as f:
            data = reverse_bits(f.read())
        with open(os.path.join(out, "Cores", CORE, bitstream), "wb") as f:
            f.write(data)
    else:
        print("no --rbf given: the release has no bitstream and will not start on a Pocket")

    zip_path = out.rstrip("/") + ".zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for dirpath, dirnames, names in os.walk(out):
            dirnames.sort()
            for name in sorted(names):
                full = os.path.join(dirpath, name)
                z.write(full, os.path.relpath(full, out))
    print(f"wrote {out}/ and {zip_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
