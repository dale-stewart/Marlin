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

## First slice: the names a computer wrote

`readDir` was the largest reachable piece, and all fifteen of its dark lines turned out to be one
thing — **the long-filename decoder**. Every media test in this tree names its files in 8.3
(`round.gco`, `present.gco`), and an 8.3 name has no VFAT entries at all, so the sequence
handling, the checksum and the orphan detection had never run.

The reason nothing had reached it is worth stating, because it is not laziness. **This firmware
cannot write a long filename.** `LONG_FILENAME_WRITE_SUPPORT` is off by default, so
`openFileWrite()` lays down an 8.3 entry and stops. Every long name a printer ever sees was
written by something else — the user's computer, copying slicer output onto the card — and no
test could produce one by asking the firmware to.

So `SimulatedMedia::add_pc_written_file()` is the stand-in for that computer: VFAT chunks in
reverse order ahead of the 8.3 entry, each carrying a checksum of the short name. The checksum is
computed in the fixture from the FAT specification rather than by calling the firmware's
`lfn_checksum()`, for the same reason Fletcher-16 is written by hand in the transfer tests — a
fixture that asks the code under test to prepare its own input agrees with it however wrong it is.

Four tests: the name read back whole across two chunks, the short name still being what the
machine opens, an orphaned name discarded rather than shown against the wrong file, and the
limit below.

### Register #66: names past the limit are cut silently, and files collide

`VFAT_ENTRIES_LIMIT` is 2 here, so `longFilename` is twenty-seven bytes and the reader ignores
chunks past the second. A forty-two character name comes back as its first twenty-six, with
nothing to say anything was dropped.

That matters because of the shape slicer output has — a long common prefix with the distinguishing
part at the end. `benchy_0.2mm_PLA_20min_quality_draft.gcode` and `..._final.gcode` are **listed
identically**, so a user picking from the menu cannot tell which will print. Long filenames exist
to prevent exactly that, and here they reintroduce it.

**The test asserts the truncated string rather than the full one deliberately**, and the injection
is why. Removing the range check returns the whole name — by writing chunk three at offset 26 and
running off the end of a twenty-seven byte buffer, without crashing. The truncation is the visible
face of a bounds guard, so a test demanding the nicer answer would license a memory fault. A fix
marks the truncation; it does not remove the limit.

### The limit is not the same number in every configuration

The first version of that test hard-coded twenty-six characters, passed under `004-sd_powerloss`,
and **failed under `010-dwin`** — which enables `HAS_DWIN_E3V2`, and with it
`VFAT_ENTRIES_LIMIT` 5, sixty-five characters, comfortably longer than the name being sent. The
name came back whole and the assertion had nothing to say except that it expected a shorter one.

Fixed by deriving the expectation — `VFAT_ENTRIES_LIMIT * FILENAME_LENGTH` — and by choosing
names long enough to exceed the limit in *every* configuration here rather than only in the one
being run. It now pins the rule instead of the number, and passes under both limits.

This is the whole-suite version of a caution already in the log: a figure measured in one
configuration is a figure about that configuration. Here it was not a figure but an *assertion*,
which is worse, because it looked like a fact about the firmware.

### Three counting mistakes, all caught by the assertions

Worth recording because it is the argument for asserting exact strings rather than lengths or
prefixes. In writing four small tests I got the file size wrong by one byte, called a
twenty-six character name twenty-seven, and claimed two names shared twenty-eight characters when
they diverged at twenty-four. Every one of them came back as a failure naming both values, and the
last was the one that mattered — the collision test was passing for the wrong reason until the
names actually shared a prefix longer than the limit.

## What is left

Fifty-six lines, scattered across the `open` overloads, `write`, `openRoot`, and a handful of
position and truncation helpers. No single cluster like this one.
