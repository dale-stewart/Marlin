# Files on the card: `sd/SdBaseFile.cpp` and the SdFat layer

Part of the rescue log — see [README.md](README.md) for the index and `CLAUDE.md` for the rules
that apply to every session.

Opened with a measurement rather than a test, because the measurement changed what the target was.

## The survey ranked this file by a number that means something else (2026-08-17)

[survey-2026-08-14.md](survey-2026-08-14.md) lists `sd/SdBaseFile.cpp` at 46% with 347 uncovered
lines, which made it the largest remaining gap by a wide margin once `pause.cpp`,
`binary_stream.h` and `e_parser.cpp` were closed. Measured again today it is **48%, 654 countable
lines, 340 dark** — so the ranking looked right and the file looked like a week of work.

It is not. **269 of those 340 dark lines are library API that this firmware never calls.**

| | dark lines | functions |
|---|---|---|
| reachable from Marlin | **71** | `readDir` (15), the three `open` overloads (21), `write` (7), `openRoot` (5), and nine smaller |
| never called outside SdFat | **269** | `rename` (37), `lsPrintNext` (37), `timestamp` (39 across two), `rmRfStar` (26), `openNext` (13), `hide` (13), `fgets` (13), `rmdir` (12), `peek` (11), `contiguousRange` (11), `ls` (10), `createContiguous` (10), and nine more |

Excluding what cannot be reached, the file is at **81.6%** of its reachable lines — a long way from
the worst target in the tree, and not worth a week.

### How it was checked, since counting is what went wrong the first time

Grep for each dark function's name as a *method call* anywhere in `Marlin/src` outside SdFat's own
files. Two results needed a second look and both changed the answer:

- **`ls` appears to have callers** — `card.ls()` in `M20.cpp` and `easythreed_ui.cpp` — but that is
  `CardReader::ls`, which walks the directory with its own `printListing`. `SdBaseFile::ls` is
  reached by nothing, and it takes `lsPrintNext` and `openNext` with it: a self-contained cluster
  of 60 dark lines that exists only because the library ships it.
- **`rename` has two callers**, both in `lcd/extui/mks_ui/`, which no configuration here compiles.
  So it is not unused in principle, only unreachable in every build measured — a different
  category, and the one to revisit if an MKS display configuration is ever added.

Counting raw grep hits was useless: `write`, `peek` and `hide` matched hundreds of unrelated
symbols across the firmware. The counts only became meaningful once the pattern required a
*receiver* and the hits were read rather than totalled.

### Why this is worth a log entry rather than just doing the work

It is the same shape as the `heatshrink` finding one file over, and stronger. There, a vendored
library measured in one configuration produced a mutant population 27% of which could not be
killed. Here a vendored library produces a *coverage* figure 79% of whose gap is not work.

A library exports an API for all of its users; a product calls a slice. The uncovered remainder
is not debt, and treating it as debt buys tests that pin an API nobody calls — which is worse than
no tests, because it makes the unused surface harder to delete.

The general form is in the skill's `survivor-taxonomy.md` and `scoring.md`. The instance is here
because the number in the survey is wrong in a way that would have cost days, and the survey now
says so.

## What is actually left

Seventy-one lines, and the largest single piece is `readDir` (15 dark of 133) — the directory walk
behind `M20`, with three call sites in `cardreader.cpp` and the thing every file listing goes
through. The rest is scattered across the `open` overloads, `write`, `openRoot`, and a handful of
position and truncation helpers.

Nothing written yet.
