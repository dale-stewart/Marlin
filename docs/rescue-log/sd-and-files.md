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

## Running the workflow cold on `cardreader.cpp` (2026-08-17)

The skill had gained about ten rules in one session and had not been run end to end since. This
was that check, on a target chosen by its own step 0 rather than by what was convenient.

Step 0 put `cardreader.cpp` at 381 lines, 82% covered, **66 dark and all of them inside named
methods** — no unreachable-API problem here, because unlike `SdBaseFile` this file is not
vendored. The largest single piece was `write_command()` at **15 dark of 15**: the entire ASCII
upload path, the one the binary protocol exists to replace.

It was dark for a reason worth noticing. An `M28`/`M29` test already existed and passed — it
opens the file and closes it **without sending anything between**, so the feature looked tested
while the function that does the work had never run. A round trip with nothing in the middle is
a shape to watch for.

Three characterization tests, and the middle one is the point:

- a line sent while saving is stored verbatim with a CRLF the printer adds and no host sent;
- `N5 G1 X10*42` is stored as `G1 X10` — the line number and checksum protect the *link*, not
  the file;
- a line without a checksum is **refused** while saving.

### Register #67: the arithmetic is safe because of another file

`write_command()` finds its payload with `strchr(npos, ' ') + 1` and `strchr(npos, '*') - 1` and
uses both without checking either. A line carrying an `N` and no `*` makes the second
`nullptr - 1`, and the three writes after it land near address zero.

**Demonstrated: SIGSEGV**, by injecting `M117 NOTE` while a file was open.

It cannot happen through the path a host uses, because `get_serial_commands()` refuses any line
without a checksum while saving — including an *unnumbered* one, which is the part that is easy
to miss. So the safety of this arithmetic is a property of `queue.cpp`, and nothing in
`cardreader.cpp` says so. It is reachable through the injection path, which validates nothing:
193 call sites inject commands, and `EVENT_GCODE_*` lets a user configure arbitrary strings.

**The defect cannot be pinned by a test** — the reproduction crashes the process, so a committed
test would take the suite with it. What is pinned instead is the guarantee that protects it. That
is the taxonomy's third category applied to a *dependency* rather than to the code itself.

### Where the workflow document fell short

Two things, both now visible only because the run was done cold:

- **Steps 1-2 and 3-5 do not run once per file, they interleave per region.** The document reads
  as phases: seed tests, measure, mutate, kill. On a partially covered file the largest dark
  region has no mutants at all, because mutation is restricted to covered lines — so it needs
  step 1 again before step 3 can see it. Nothing says that, and following the order literally
  would have measured the file and missed its biggest gap entirely.
- **Nothing warns that the route into the code is part of the test design.** Two of these three
  tests failed first time because the helper that every other test in the file uses — `send()` —
  dispatches straight to the parser, and the behaviour under test only happens inside
  `queue.advance()`. The file's own fixture was the wrong tool and looked like the right one.

### Step 3 on `cardreader.cpp`, and a cluster that is a missing configuration

With `write_command` covered, the mutation run could finally see it — which is the interleaving
rule the cold run had just produced, working:

    testable 1002 · killed 572 by assertion · timed out 33 · survived 397
    raw 605/1002 = 60.4%      by assertion 57.1%
    1908 mutants on 329 covered lines

86% covered, 60.4% killable. That is the fourth data point for the rule measured this morning —
pure code scores near its coverage, stateful code observed indirectly does not — and
`cardreader.cpp` is squarely the second kind.

The largest cluster was **`manage_media()` at 66 of 397**: the state machine that notices a card
arriving or leaving. Every test in this tree starts with the card in the slot and leaves it
there, so it had never been given anything to notice.

**It is not a missing test.** Whether a card is present is `READ(SD_DETECT_PIN) == SD_DETECT_STATE`
when `HAS_SD_DETECT` is set, and in this tree it never is: the only board that defines
`SD_DETECT_PIN` does so inside `#if ANY(TFT_COLOR_UI, TFT_CLASSIC_UI, TFT_LVGL_UI)`, and **none of
the fourteen configurations enables any of them**. Without a detect line `isSDCardInserted()` is a
constant, the removal branch cannot be taken, and the cluster is unreachable by construction.

So it is the `006-eeprom` situation again — a behaviour that needs a *configuration* rather than a
test, and the same lever applies. What it would take: a card-detect pin defined for the native
board outside the TFT block, either by a new configuration or by moving the definition (which
would turn detection on for all fourteen and is therefore not a change to make quietly).

**Resolved the same day by adding the configuration.** `015-sd_detect` defines `SD_DETECT_PIN`
directly from the config file rather than by enabling a display — on this board the pin otherwise
exists only inside `#if ANY(TFT_COLOR_UI, TFT_CLASSIC_UI, TFT_LVGL_UI)` and the LCD blocks, so
reaching it through a UI option would drag in thousands of lines of vendored display code to test
four branches in the media layer. Marlin's `config.ini` mechanism appends unknown keys as
`#define`s, which makes the whole configuration four settings.

### Register #68: the removal branch cannot be reached at all

The first run of that configuration failed all three tests, and the reason was not the fixture.

`manage_media()` computes `did_insert = TERN(HAS_MULTI_VOLUME, vadd, stat) != INSERT_NONE`, so
without multi-volume support the `else` is entered **only** when `stat == INSERT_NONE` — and the
condition guarding that `else` *is* `stat`. It is therefore always false. `release()` is never
called, `flag.mounted` stays true after the card is physically gone, and because `release()` is
what calls `abortFilePrintSoon()`, **a print whose card is pulled keeps running**.

The probe that settled it: with the card removed, `isSDCardInserted()` correctly returned 0 and
`isMounted()` was still 1 after `manage_media()`. The pin worked; the branch did not.

The tests were then rewritten to characterize what the firmware does rather than what it should,
per the rescue's rule that a discovered defect is pinned and reported rather than quietly fixed —
`a_removed_card_is_not_released` and `a_card_pulled_during_a_print_does_not_stop_it`, both marked
LEGACY-BEHAVIOR.

**`inserting_a_card_mounts_it` is the control, and it is the part that makes the other two mean
anything.** It passes, so `manage_media()` demonstrably runs and demonstrably acts. Without it,
"removal does nothing" is equally well explained by the function never being reached, and the
tests would be pinning the fixture.

This is the argument for the whole `006-eeprom` pattern in one example. The cluster was
unreachable, the configuration cost four lines, and what it exposed on its first run was a
firmware defect that had been invisible in every build this project measures — not a gap in the
tests.

The remaining clusters, for whoever picks this up: `openAndPrintFile` (40, of which 33 are on one
line computing the size of a command buffer that is never tight — likely equivalent),
`printListing` (37, needs nested directories in a listing), `diveToFile` (31, needs deep paths).

## What is left

Fifty-six lines, scattered across the `open` overloads, `write`, `openRoot`, and a handful of
position and truncation helpers. No single cluster like this one.
