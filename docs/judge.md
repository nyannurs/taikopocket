# Judging, scoring, gauge and branches

`src/core/judge.c` is a port of YataiDON's `JudgeCore` and `GaugeCore`
(`src/libs/judge.cpp` at `309d7b55`). `make golden` replays simulated plays through
both: for every course of every chart, six simulated players (perfect, good, average,
sloppy, random mashing, idle) play it, the inputs are recorded, and YataiDON's own
code replays them. Every judgment, branch decision and final result must match.
Current result: **6,432 plays, 3,863,809 judgments, 1,608 branch decisions
identical**, 0 different.

## Timing windows (ms, from `judge.h`)

| Course | GOOD | OK | BAD |
|---|---|---|---|
| Hard, Oni, Edit | 25.0250015 | 75.0750046 | 108.4416656 |
| Easy, Normal | 41.7083359 | 108.4416656 | 125.125 |

A course with `LEVEL:0` (or none) is judged and gauged as Oni level 10, as in YataiDON.

## Notes

- Don and ka notes wait in separate queues. A hit is checked against the front note
  of its own queue only.
- A note is missed (BAD, combo reset) once the BAD window has passed.
- **Giving up a note:** if the front note is already past its OK window, the next
  note of the same colour is within its OK window, and no other note lies between
  the two, the hit counts for the next note and the front one becomes BAD.
- **Big notes** score like normal notes. YataiDON has no bonus for hitting both sides.
- **Drumroll:** every hit (either drum) scores 100. **Balloon:** only don hits count,
  100 each; it bursts at its `BALLOON` count. **Kusudama:** as a balloon.
  A balloon or kusudama without a count needs 1 hit.
- Rolls and balloons do not add combo. A burst balloon does not either.
- **The end of the chart** (`chart_end_ms`, after which the game waits 2 seconds and
  goes to the result screen) is the time of its last note. YataiDON counts bar lines
  too, which is the one place this judge differs: a chart that ends in empty measures
  at a tempo near zero (four bars at 0.47 BPM after the last note, a trick that holds
  the last picture of the lane; such charts exist) would otherwise go on for an hour
  after the music. Bar lines take no part in any judgment, so the replays still match.

## Score (shinuchi only)

One fixed value per GOOD: `ceil((1,000,000 - 100 * balloon hits - 1692.0 * drumroll
seconds) / notes / 10) * 10`, counted over the chart with the master route of every
branch. OK is half of that, rounded down to a multiple of 10. BAD is 0. There is no
combo bonus and no Go-Go multiplier. `SCOREINIT`, `SCOREDIFF` and `SCOREMODE` are
ignored (YataiDON's "gen3" mode is not ported).

## Soul gauge

10,000 points. GOOD adds `1,000,000 / (notes * soul%)`, OK adds that times the OK
factor, BAD the BAD factor (negative). The table per course and level is in
`judge.c`. Clear line: Easy 60%, Normal and Hard 70%, Oni and Edit 80%. A full gauge
is the "rainbow" state. The note count uses the master route of every branch.

YataiDON's table has no row for Easy above level 5, Normal above 7 and Hard above 8;
there its gauge does not move at all. The game gives such a course the highest row of
its table, so that it can be cleared. This is done where the game loads a song, not in
the judge, which stays identical to YataiDON's.

## Branches

- `#BRANCHSTART` is placed on the judging timeline two measures before its position
  (or before the bar that followed `#SECTION`). `#SECTION` resets the counters.
- Counters: `p` adds 1 per GOOD and 0.5 per OK, every judged note (misses too) adds 1
  to the note count; `r` counts drumroll hits.
- The decision is made when the **first note of the next section would come on
  screen** (its load time, computed with YataiDON's 720p lane constants and the
  user's scroll speed, as in YataiDON).
- `p` becomes a whole percentage 0-100. Count >= master threshold: Master. Otherwise
  count >= expert threshold (and the expert threshold is not negative): Expert.
  Otherwise Normal.
- The chosen path's notes are merged into the queues; the other two are dropped.
- Conditions other than `p` and `r` are not evaluated by YataiDON: the previous
  thresholds are reused with the drumroll count. Matched.

### Extensions (not in YataiDON, off in the parity tests)

- `#LEVELHOLD`: from that point on every decision keeps the current path.
- A section belongs to its own `#BRANCHSTART`. Charts exist that define one route per
  branch point (only `#M` at the first, only `#N` at the next, no `#BRANCHEND`) with
  conditions that force exactly that route. YataiDON takes the first section left of
  every route at each decision, so the first decision throws away the `#N` section of
  the second branch point and the rest of the chart has no notes. Here a decision
  uses and discards only the sections of its own branch point, and waits for the
  first note of those.
- Condition `s`: the score gained since the last `#SECTION` is compared with the
  thresholds.

Both are enabled in the game through `tp_judge_load(..., extensions = true)`.

## Auto-play

Port of `Player::autoplay_manager`: notes are hit at their own time (always GOOD),
rolls and balloons at 24th-note speed of the current BPM.

## Input timing in the game

Hits carry the hardware timestamp of the button edge. Each frame the judge is first
advanced to every hit's own time (`begin_frame(t_hit)`, `hit(t_hit)`) and only then
to the present, so judgments do not depend on the frame rate.
