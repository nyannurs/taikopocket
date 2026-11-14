#!/bin/sh
# Fetches the default font, and the reference code the tests compare with, into
# third_party/ (git-ignored). `fetch_third_party.sh font`: only the font, which is all
# the packer needs.
set -e
cd "$(dirname "$0")/../third_party"

mkdir -p fonts
get() { # file url: a download that breaks off leaves nothing behind
    if [ ! -f "$1" ]; then
        curl -fsSL -o "$1.part" "$2"
        mv "$1.part" "$1"
        echo "$1"
    fi
}
get fonts/NotoSansJP-Regular.otf https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf
get fonts/OFL.txt https://github.com/notofonts/noto-cjk/raw/main/Sans/LICENSE
[ "$1" = font ] && exit 0

fetch() { # dir url commit
    if [ ! -d "$1/.git" ]; then
        git init -q "$1"
        git -C "$1" remote add origin "$2"
    fi
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" checkout -q FETCH_HEAD
    echo "$1 @ $3"
}

fetch YataiDON https://github.com/Yonokid/YataiDON 309d7b55cc30c18ae2b0f70ba5a161b4effda419
fetch openfpga-litex https://github.com/agg23/openfpga-litex c21570d85260856c10f7b11c9e4d233c0fe65748
