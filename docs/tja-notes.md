# TJA support

The parser (`src/core/tja.c`) is a port of YataiDON's `TJAParser` at commit
`309d7b55`. `make golden` parses every available chart with both and requires
identical output: every note, bar line, timeline event and tempo point, compared
with full `double` precision. Current result: **848 charts, 1072 courses, 757,715
notes and bar lines identical**, 0 different (the test charts, YataiDON's bundled
songs, and the 838 charts in `taikojiro_reference/`).

## What is supported

- **Headers** `TITLE` (and `TITLEJA` etc.), `SUBTITLE` (`--`, `++`, `/ ` prefixes),
  `BPM`, `WAVE`, `OFFSET`, `DEMOSTART`, `GENRE`, `SONGVOL`, `SEVOL`,
  `COURSE` (names or 0-6, `Ura`), `LEVEL`, `BALLOON`, `BALLOONNOR/EXP/MAS`, `STYLE`.
  `SCOREINIT`, `SCOREDIFF`, `SCOREMODE` are read and unused (scoring is shinuchi).
- **Notes** `0`-`9`, `A`, `B` (big don / big ka). Repeated `9`s are one kusudama.
- **Commands** `#START` (also `P1`/`P2`), `#END`, `#MEASURE`, `#BPMCHANGE`, `#DELAY`,
  `#SCROLL` (negative, complex `a+bi`, polar `speed, lower, upper`), `#GOGOSTART`,
  `#GOGOEND`, `#BARLINEOFF`, `#BARLINEON`, `#BRANCHSTART`, `#N`, `#E`, `#M`,
  `#BRANCHEND`, `#SECTION`, `#LEVELHOLD`, `#LYRIC`, `#NMSCROLL`, `#BMSCROLL`,
  `#HBSCROLL`, `#SUDDEN`, `#JPOSSCROLL`, `#ENABLEDORON`, `#DISABLEDORON`.
- Parsed and ignored: camera commands (`#CAM...`), `#BORDERCOLOR`, anything unknown.
- **Double play**: single player only. A course with only `#START P1` / `#START P2`
  charts plays P1.
- Tower and Dan courses are not offered.

## Where YataiDON differs from the TJA specification

The [taiko-web TJA page](https://web.archive.org/web/20230103093826/https://github.com/bui/taiko-web/wiki/TJA-format)
is the reference. We follow YataiDON in each case.

| Topic | Specification | YataiDON (and this project) |
|---|---|---|
| Other note letters (`F` ad-lib, `C`, `D`, `G`, ...) | `F` is a hidden note | blank, with a warning |
| Branch condition | any type except `r` means accuracy | only exactly `p` and `r` are evaluated; other types reuse the previous thresholds |
| Moment of the branch decision | one measure before `#BRANCHSTART` | when the first note of the next section comes on screen |
| Go-Go Time | score x1.2 | no score effect (shinuchi) |
| Scoring | `SCOREMODE` 0/1/2 with `SCOREINIT`/`SCOREDIFF` | shinuchi: one fixed score per note |
| Lines with a comma in the middle (`1010,1010`) | not defined | the whole line is one part; the inner comma is a blank note |
| Header names | exact | prefix match (`BPMX:` is `BPM:`) |
| `HEADSCROLL`, `#DIRECTION`, `#SENOTECHANGE`, `LIFE`, `TOTAL`, `SIDE` | defined | ignored |
| `#LEVELHOLD` | locks the branch | **not implemented in YataiDON**; implemented here as an extension |

## Deliberate differences from YataiDON (compatibility mode)

YataiDON drops or mangles charts written the way Taikojiro accepts them, and 11 of
the 838 charts in the test library are affected. The game therefore parses with
`compat = true`, which adds four rules. With `compat = false` the parser is exactly
YataiDON (that is what `make golden` checks).

1. **No `COURSE:` before `#START`** means Oni (the specification's default).
   YataiDON finds no course at all.
2. **Headers before the first `COURSE:`** (`LEVEL`, `BALLOON`, ...) belong to that
   course. YataiDON resets them when it sees `COURSE:`, losing the level and the
   balloon counts.
3. **`COURSE` must be the whole header name.** In YataiDON `COURSEDESIGNER4:` counts
   as an unknown `COURSE:` line and discards the course's remaining headers.
4. **A missing `#END`** ends the chart at the next `COURSE:` line or the end of the
   file. YataiDON finds no chart.

With these rules the test library yields 1228 playable courses instead of 1072.

## Where Taikojiro differs

Most charts in use were made for Taikojiro 1, which is closed. What is known about it
comes from IepIweidieng's TJA reference
(<https://github.com/IepIweidieng/TJAPlayer3/blob/gh-pages/tja.md>, behaviour set
`jiro1`). Not imitated here, as YataiDON does not:

- **Time in whole milliseconds.** Taikojiro drops the fraction of a millisecond of the
  time between two `#BPMCHANGE`s (and the end of a measure that has one). Notes after
  n tempo changes are up to about n ms earlier there than here. One tempo change: under
  1 ms. A chart with hundreds of them could differ audibly; none has been reported.
- **`#BMSCROLL` in the chart** behaves like `#HBSCROLL` in Taikojiro 1: `#SCROLL` still
  counts. Here, as in YataiDON, `#SCROLL` is ignored under `#BMSCROLL`. Charts that
  use both are rare: of 684 charts looked at, four had `#BMSCROLL` and none of those
  used `#SCROLL`.
- Notes past the drum, and a positive `#DELAY`, change how notes scroll in the beat
  modes there (constant speed after the hit; the stop moved to the next tempo change).
  Only the appearance of missed notes and of stops differs.

## Limits

Fixed storage (see `src/game/song.c`): 16,384 notes and bar lines outside branches,
32,768 inside, 4,096 timeline events, 2,048 branch sections, 2,048 tempo points,
16,384 lines. The largest chart in the test library needs 2,276 / 3,588 / 495 /
372 / 195 / 1,879. A chart over a limit is refused with a message.

Numbers are parsed like `std::stof` (longest valid prefix, rounded to `float`).
Hexadecimal floats are not accepted.
