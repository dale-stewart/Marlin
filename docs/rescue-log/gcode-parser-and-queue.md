# `gcode.cpp`, `queue.cpp`, and sizing the `GCodeParser` migration

The dispatcher and the command queue, and the survey of what reads the parser's global
state — the consumers the blocked `GCodeParser` correction has to wait for.

Part of the rescue log — see [README.md](README.md) for the index and
`CLAUDE.md` for the rules that apply to every session.

**First target rescued end to end with the new structure: `gcode.cpp` (2026-08-11).**
Surveyor picked it over better-covered candidates because it is the heaviest direct reader of
the parser globals among compiled files — 11 raw hits, the consumer the blocked `GCodeParser`
correction needs first. Line coverage was the least useful number available: 63% suggested
thin neglect, while the validated mutation baseline was **72.1% raw / 74.5% killable**, with
29% of the killable survivors inside one function.

Now **93.2% raw / 97.4% killable** (261/268), from 17 tests. `host_keepalive()` was the
cluster and it needed *an input*, not assertions — simulated time, plus busy and paused as
independent guard terms. What is left is 12 equivalents and 7 genuinely blocked, and the
blocked ones are worth knowing: `report_heading`'s `if (fstr)` false branch needs a null
`FSTR_P` that would segfault natively, and G2/G3 arc dispatch fills the planner buffer and
hangs. **`gcode.cpp` is now 93.8% raw / 97.8% killable over 119 covered lines** — not comparable
to the earlier 93.2%, which was measured over 110.

**Correction: `dwell()` was reported blocked here and is not.** The claim was that its
busy-wait never returns because `millis()` does not advance inside the loop — stated as
"verified by reading the loop, not attempted", and relayed by me into two commit messages
before anyone tried it. Under the test HAL `marlin.idle()` costs simulated time, which is the
whole reason that HAL exists, and `G4_P_dwells_for_milliseconds` in
`test_blocking_commands.cpp` has been passing all along. A blocked classification asserted
from reading is worth exactly as much as an equivalence asserted from reading, which is to say
it needs the same probe.

**Two verdicts fell to one probe.** `KEEPALIVE_STATE(IN_HANDLER)` at `:330` had also been
called observable only mid-call. A long `G4` is a handler holding the machine busy while
`idle()` advances the clock, so `a_long_dwell_reports_busy_to_the_host` drives the real
dispatch path and dies when the busy mark is deleted. One wrong sentence about `millis()` had
been hiding a reachable region, and it hid it in two places.

**The `GCodeParser` correction now has a net, and the net was tested (2026-08-11).**
Steps 6-7 for `gcode.cpp` were not bookkeeping: **239 references across 22 test files name the
`parser.` surface**, twice the `planner.settings` figure, so the unit tests would be edited by
the very refactor they are meant to protect. The acceptance step file names it **zero** times.

Proven rather than argued, by the same drill used on the print job timer: `codenum` →
`command_number`, `string_arg` → `command_text`, `codebits` → `seen_bits` across **20
production files**, feature files and steps untouched. The acceptance suite built and passed
37/37; the unit-test build failed to compile. That is the migration's safety net demonstrated
against the migration itself.

Acceptance-only coverage of `gcode.cpp` is 47% (was 43%), `parser.cpp` 79%, `queue.cpp` 58%.

Two spans are **unreachable from outside** and are not coverage gaps: `host_keepalive()`'s
reporting branches and `process_subcommands_now()`. Both need a handler that loops on
`marlin.idle()` past the keepalive interval while simulated time advances — the same state
`dwell()` needs and cannot get, because nothing drives the virtual clock while a handler spins
on `idle()`. That is a production seam, and it is the one thing standing between the acceptance
suite and the dispatcher's reporting paths.

**`queue.cpp` rescued (2026-08-11): 60.6% -> 77.2% raw, ~87% killable, 68% -> 76% line.**
The M112 cluster — 28 mutants at `queue.cpp:541` — is the taxonomy's third category, code with
no observable outcome: `Marlin::minkill()` ends in `for (;;) hal.watchdog_refresh()` and never
returns, so no assertion can run after it. Verified by reading `MarlinCore.cpp`, not assumed.
A real printer does stop dead on M112, so the substitute halting is accurate rather than
defective, and the cluster is a blocked seam rather than a gap.

**The over-long-line branch was defect #25 and no longer hangs (2026-08-12).** Nine shapes,
both HALs, all complete — see the register for what was tried and what remains unestablished
about why. It is now asserted from the narrowing side only, and the reason is the useful part:
the obvious companion assertion — a parameter one character *past* the cut, expected to be
lost — passes with the guard deleted, because without it the reader writes past the end of the
buffer rather than reading more of the line. Widening mutants there are undefined behaviour,
not different behaviour. Also worth knowing before writing a test like it: a test cannot
transmit more than the port's 128-byte receive buffer in one go, and the newline goes over the
side with everything else, so the symptom is no command at all — which looks exactly like a
line the firmware refused.

Two counting traps caught here, both worth remembering:

- **The agent reported 131 survivors killed; the true figure is 79.** It had subtracted one full
  run's kill count from another's (433 - 302) — but the new tests raised covered lines from 120
  to 135, so the second run had a larger mutant population, including new mutants that were born
  dead. Re-running the *previous run's survivor list* is the only way to count survivors killed,
  because that population is fixed. Its headline 77.2% was correct; the work-done figure was
  inflated by 65%.
- **A measurement taken straight after `make unit-test-all-local` is unrestricted and wrong** —
  see the coverage-build gotcha in `CLAUDE.md`. This bit me in this very session, one hour after using the same fault
  deliberately to test `harness-validator`.

### Sizing the `GCodeParser` migration (2026-08-11)

18 files read `parser.codenum` / `codebits` / `string_arg`. Measured rather than assumed, and
the shape changed twice in the measuring.

**Eight were compiled by an existing configuration and had simply never been measured.** Under
`004`: `M23` 100%, `M30` 100%, `M32` 100%, `M928` 100%, `M28_M29` 71%. Under `003`: `T.cpp` 75%.
Under `008`: `M117` 80%. `M118` turned out to be 92% in the default build and had never been
listed as rescued at all. That is nine of eighteen effectively closed by pointing a coverage run
at builds that already existed — no tests written.

**`M0_M1.cpp` is rescued (2026-08-12): 0% -> 100% line, 100% mutation (21/21) under `003`.**
It was the biggest remaining consumer — 11 parser reads — and had sat at 0% under `003`, `008`
*and* `010` (`HAS_DWIN_E3V2` is in the `HAS_RESUME_CONTINUE` list too, which the earlier survey
missed). Not neglect: it is a command whose entire job is to not return, so nothing in the
suite could reach it until the test HAL made `idle()` cost simulated time.

Three things it taught, none of them about M0:

- **The untimed branch needs a second thread.** With no `P` and no `S` there is no deadline,
  so the only thing that ends the loop is a person. The answer has to arrive from another
  thread — the `SerialCapture` arrangement — and its delay must be *wall-clock*, because
  simulated time only moves when the machine moves it.
- **`> before` is not enough, and the mutation run is what said so.** `ms = 0` mutated to
  `ms = 1` gives a one-millisecond deadline, which satisfies "time passed" perfectly. The
  untimed case has to be shown to outlast any trivial deadline. Two survivors, one bound.
- **A bare `planner.synchronize()` has no return value and no message.** Queueing a move that
  takes ten times the wait is what makes it observable: with the synchronize the command
  cannot return until the move is done, without it the wait is over first. This is the one
  behaviour that actually matters — `M0` is where a person reaches into the machine.

The `command_number()` read is asserted through the host prompt (`//action:prompt_begin M0 Stop`
against `M1 Stop`), which is the only place in the file where the two commands differ at all.

**Register #41 is fixed, and the fix has no test — deliberately.** `MString::ltrim()` called
`strcpy` on overlapping ranges; it is now `memmove`. The trim produces the right answer either
way, so no assertion in this suite can distinguish fixed from broken, and writing one would be
theatre. **The sanitizer is the pinning**: before, `make unit-test-asan` aborted under `009` after
331 tests; after, both `001` and `009` run to completion with only register #27's float-rounding
assertion failing. When the only instrument that can see a defect is the sanitizer, the sanitizer
run *is* the regression test, and it has to be run deliberately for that to mean anything.

**The five `009` consumers are closed, and the figures in this file were stale.** They read
0-7% when the configuration was new; `test_parser_consumers.cpp` was written since, and they are
**98% line** with mutation measured under `009`:

| file | raw | note |
|---|---|---|
| `sd/M33.cpp` | 100% | |
| `config/M550.cpp` | 92.3% | 1 survivor: `ui.reset_status(false)` deleted — no display in this build |
| `feature/macro/M810-M819.cpp` | 87.5% | 5 survivors, all equivalent |
| `geometry/G53-G59.cpp` | 83.8% | 11 survivors, all equivalent |
| `host/M16.cpp` | 75% | 1 survivor: the mismatched-name branch calls `kill()`, which never returns |

**Every one of those 16 "equivalent" survivors is the same shape, and it is worth naming: a
mutant whose only effect is undefined behaviour.** Widening a bounds check lets an out-of-range
index through, and the array read or write that follows is out of bounds — so the mutant does not
compute a *different* answer, it computes an *undefined* one, which on this platform happens to
read as zero. No assertion separates those, and reaching for one means asserting on memory layout.

**But a sanitizer does separate them, and that was checked rather than assumed.** With the guard
in `M810_819()` deleted, `make unit-test-asan` reports `global-buffer-overflow ...
M810-M819.cpp:52` from inside `a_macro_number_past_the_configured_slots_does_nothing`. So the
honest classification is *equivalent under the ordinary suite, detectable under the sanitizer
build* — not *unkillable*.

Two of the sixteen are a different and prettier equivalence: `command_number() - 54` mutated to
`% 54`, and `- 810` to `% 810`. Over the reachable domains (54..59 and 810..819) subtraction and
modulo agree exactly, so those are equivalent by reachable range rather than by undefined
behaviour.

**Getting to that answer took two false starts, both instructive.** The first sanitizer run
reported nothing, and it would have been easy to write down "the sanitizer does not see it" — but
it had aborted six test files earlier on an unrelated fault and never reached the macro tests. A
control that does not run is not a control. The fault it aborted on is register #41, a genuine
`strcpy`-on-overlapping-ranges in `MString::ltrim()`, and #42 is why it was invisible: PlatformIO
prints `331 test cases: 1 failed, 329 succeeded` beside `[ERRORED]` and none of the sanitizer's
output. Run `.pio/build/testhal_native_asan/program` directly to see it.

**`009-parser_consumers.ini` makes five more visible** — `M550`, `G53-G59`, `M16`, `M810-M819`,
`M33`, which no configuration compiled. They now read 0-7%, which is the point: 0% and measurable
is a rescue, invisible is not. Three traps on the way, two of which `004` had already hit and
documented — `CONFIGURABLE_MACHINE_NAME` needs `GCODE_QUOTED_STRINGS`; media defines
`EVENT_GCODE_SD_ABORT` and `SanityCheck` asserts on it with a **non-constexpr `strstr`**, which
can never compile; and `REINIT_NOISY_LCD` is on by default but unwanted on `BOARD_SIMULATED`,
which `-Werror` turns fatal. The third is new: **these `.ini` files take no inline `#` comments.**
The configuration script copies the rest of the line into the generated `#define`, and an em-dash
in a comment surfaced as "extended character is not valid in an identifier" in
`Configuration_adv.h` — a file I had not edited.

**Two remain genuinely out of reach, and only one is a wall.** `M485.cpp` (8 reads) pulls a
third-party RS485 library that wants `arduino/HardwareSerial.h`: not host-buildable, same class
as `sovol_rts` and `mks_ui`, a porting job. `mmu3.cpp` (1 read) is buildable in principle but
chains `MMU_MODEL` → exactly 5 extruders → `FILAMENT_RUNOUT_SENSOR`, which would turn `009` into
a different machine for the cheapest consumer in the set. It wants its own configuration, and it
is worth one read.

**The migration is now blocked on `M485` alone.** `M0_M1` is rescued (100% line, 100% mutation),
the five `009` files are closed, and **`M28_M29` is 100% line / 98.0% raw, 100% killable** under
`004` — its one survivor is `p[1] > '0'` mutated to `!= '0'`, which the `NUMERIC(p[1])` guard on
the line above makes equivalent by constraining the operand to `'0'..'9'`. Worth keeping as a
pattern: **a guard on the preceding line is part of the domain of the line after it**, and the
mutation runner does not know that.

The one test that was needed there is also worth keeping. `while (*p == ' ')` widened to `<= ' '`
survived everything, because with an ordinary `B0 name.gco` the two agree exactly — one space,
then a letter. They separate on any byte below a space, and a **tab** is the reachable one: a host
sends bytes and nothing upstream turns a tab into a space. The tab is not skipped, so it stays in
the filename, which the card then refuses — and the assertion is bracketed against the space
rather than left as "no file was opened", because that alone is satisfied by every cause of
nothing.

`M485` remains unbuildable for the host, which is a porting job rather than a testing one.
