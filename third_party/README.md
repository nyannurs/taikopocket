# third_party

Downloaded material that is **not** part of this repository (git-ignored).
`tools/fetch_third_party.sh` fetches the three folders below (`... font`: only the font).
Anything else here was put there by hand for reference and is not needed to build.

| Folder | What | Used for |
|---|---|---|
| `YataiDON/` | https://github.com/Yonokid/YataiDON at commit `309d7b55` (GPL-3.0) | behavioural reference; `tests/yatai/` links its parser and judge for the golden tests |
| `openfpga-litex/` | https://github.com/agg23/openfpga-litex at `c21570d` (MIT) | reference copy of the upstream that `gateware/` was forked from |
| `fonts/` | Noto Sans JP (OFL 1.1) | default font for the packer's glyph atlas |
