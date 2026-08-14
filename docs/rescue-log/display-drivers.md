# The DWIN display driver, and `marlinui.cpp`

The first LCD driver made host-buildable, what it costs to test a driver whose only
observable is a byte stream, and the mutation measurement that put a number on that.

Part of the rescue log — see [README.md](README.md) for the index and
`CLAUDE.md` for the rules that apply to every session.

**`dwin/creality/dwin.cpp` opened (2026-08-13): the largest untouched file in the tree, and the
first slice found two defects.** 1985 countable lines at 8% under `010-dwin`. Six tests now,
and the value of them is not the percentage — it is which parts of a 4000-line menu driver turn
out to be assertable at all.

Three kinds of code live in it: drawing primitives that emit bytes at the panel, HMI handlers
that are encoder state machines, and a few pure helpers. The handlers are where the behaviour
is, because they are what **writes machine settings from the front panel**.

**The value editors are the reason this file matters to the `planner.settings` migration, and
they disagree with each other.** `hmiMaxFeedspeedXYZE()` and `hmiMaxAccelerationXYZE()` commit
through `planner.set_max_feedrate()` and `set_max_acceleration()` — the setters that keep the
derived limits in step. `hmiStepXYZE()`, three functions away in the same file, assigns
`planner.settings.axis_steps_per_mm` directly and refreshes nothing. That is register #33, and
**pinning the two correct siblings is what turns it from "this driver does not bother with
refreshes" into "this driver refreshes everywhere except one place"** — the difference between
a design decision and a bug. The acceleration test asserts
`max_acceleration_steps_per_s2`, the limit the stepper actually enforces, rather than the stored
millimetre value, which is right in both the working and the broken version.

**The menu walk is now a shared helper, which is the point of doing a second one.**
`walk_a_menu(knob, pump, screen, rows)` winds hard against the clamp, walks back pressing at
every row, and returns where each press led. Every remaining HMI handler in this driver is the
same shape — a `switch` on the cursor with one arm per row — so the pattern is the deliverable
rather than any single test.

Three things it deliberately does not assume, each of which was a bug in an earlier draft
somewhere in this file: **which way the knob turns** (inverted between the main menu and the
file list), **how far one turn moves the cursor**, and **that the knob is read at all between
turns** (the menus go through `get_encoder_state()` and its 20 ms gate; the value editors do
not).

First use: the Control menu, five rows to five distinct screens in drawn order. Worth asserting
again one level down rather than assuming, because this is a second hand-written `switch` with
its own row constants and the way these fail is two arms transposed — you press Motion and get
the temperature editor, and nothing about the code looks different when that happens.

Checked rather than assumed before walking it: `EEPROM_SETTINGS` is off under `010-dwin`, so
Save, Load and Reset are not compiled and the walk cannot fire `settings.reset()` part-way
through the suite. With EEPROM on this test would have to skip those rows rather than press them.

**Then the other three menus, and the helper paid for itself: 35% -> 55% from four tests**
(710 -> 1097 covered lines of 1985). Motion, Temperature and Move are each one call plus a list
of where the rows should lead, and the assertion loop is factored out too — `the_walk_visited()`
takes the direction from the walk rather than stating it, for the reason the helper already
gives.

**The row counts had to come from the test, not from the driver.** `MOTION_CASE_TOTAL` and
`TEMP_CASE_TOTAL` are `#define`s inside `dwin.cpp` and invisible to a test, which looked like an
obstacle and is not: the expected destination list already states how many rows there are, so it
*is* the count. `walk_a_menu(..., forwards.size())` says the same thing once instead of twice.
The lists are built with the same `ENABLED(CLASSIC_JERK)` / `HAS_HEATED_BED` / `PREHEAT_COUNT`
the driver uses, so a build with jerk or without a bed asserts its own shape rather than failing.

**Both new claims were probed, because both passed first time.** Transposing the Feedrate and
Acceleration arms of `hmiMotion()` fails `every_motion_menu_row_opens_its_own_limit` and nothing
else; replacing the cold-extrusion guard with `if (false)` fails
`a_cold_nozzle_will_not_open_the_extruder_mover` and nothing else. Worth doing here in particular
— a walk that lost its way would report a shorter list, and a shorter list compared against a
shorter expectation is a green test about nothing.

**The cold-nozzle test is the one worth reading.** Pressing the extruder row with the nozzle at
room temperature must *not* open the mover: filament that is not molten does not go through the
nozzle, the drive gear chews a flat into it instead, and the machine then cannot print until
somebody dismantles the extruder. It asserts both halves — the editor did not open **and** the
panel was told `Nozzle is too cold` — because a refusal with no explanation reads as a dead
button, and the next thing a person does with a dead button is press it harder. That is the rule
now in `assertion-patterns.md`: where the subject is a refusal, the announcement is the positive
assertion that no other cause of nothing can satisfy.

Which end of the list the extruder sits at is *found*, not assumed, and found with the guard
lifted — probing for the row must not trip the behaviour under test before the test starts. The
probe then asserts it reached a clamp at all, since one that stopped mid-list would aim the rest
of the test at whatever row it happened to land on.

**Then the Prepare menu, 55% -> 60%, and the useful part is that `walk_a_menu()` is the wrong
instrument for it.** The helper answers *where did each row lead*, and five of Prepare's rows do
not lead anywhere — they release the motors, start a homing move, preheat for a material, cool
everything down, change the language. `checkkey` is untouched by all five, so a walk collapses
them into one entry and then compares a short list against a short expectation. That is a green
test making a much weaker claim than it looks like. **A shared probe has a question it answers,
and a menu that does not answer that question needs a different probe rather than a looser
assertion.**

What separates these rows is their *effect*, so `walk_the_prepare_menu()` records that instead:
each press is preceded by putting both heaters at a marker value no row in the menu can produce,
which turns "this row changed no temperature" into a positive observation rather than an absence.

**The rows are located by effect, not by index, and that is forced rather than stylistic.**
`PREPARE_CASE_PLA` is a `#define` inside the driver computed from four `ENABLED()` terms, so a
test naming a number would be asserting against arithmetic it cannot see. Finding the row that
heats to `ui.material_preset[0]` states the real claim anyway — *one row preheats for the first
material and a different one for the second, in drawn order* — which is exactly the transposition
worth catching. The presets are asserted to differ first, or the test is satisfied by any wiring.

**The walk runs from the far clamp down to Back and stops there.** Back is the only row whose
outcome is unambiguous from outside, so it is the anchor; stopping at it discovers the row count
rather than stating it, and stops the walk pressing a clamped row twice — which for the language
row would mean toggling it an unpredictable number of times.

Three tests, each probed by injection: transposing the two preheat arms fails the material test
and nothing else, cooling only the hotend fails the cooldown test and nothing else, and assigning
instead of toggling the language fails the language test and nothing else.

- **The cooldown test asserts no row half-cools.** The bed is the heater people forget — out of
  sight under the print, silent, and holding 60 C into an empty room all night. A row that zeroed
  only the nozzle would look right on the panel, because the number a person watches is the
  nozzle's.
- **The language row is asserted twice over, because assignment and a toggle agree on the way
  out.** A driver that assigned would strand anyone who pressed it once in a menu they cannot
  read, with no other control that helps. Two walks, there and back.
- **And the first draft got the knob backwards again** — the fourth time in this file. The
  fixture's `turn_clockwise()` is a phase sequence; which `ENCODER_DIFF` the firmware derives
  from it is the firmware's business, and the walk reported a menu one row long. It now presses
  at one clamp and asks whether that was Back, which is free because pressing Back only returns
  to the main menu.

**`dwin.cpp` mutation tested at last (2026-08-14): 65% line, and 9.3% mutation.**

| | |
|---|---|
| covered lines | 1305 of 1985 countable |
| mutants generated | 32585 |
| ...on covered lines | 11148 |
| build failures | 2765 (25%, excluded — never produced a testable program) |
| testable | 8383 |
| killed | 769 + 10 timeouts |
| **score** | **779/8383 = 9.3%** |

Measured alone under `010-dwin`, coverage build rebuilt immediately beforehand — the runner
reported "on 1305 covered lines" rather than "on all lines", which is the tell that the
restriction applied. Lowest score anywhere in this fork by a wide margin, and **the gap between
65% and 9.3% is the point of having run it at all.**

**83% of the 7604 survivors are on lines that call a drawing primitive** — pixel coordinates,
colour constants, font ids, `MBASE`/`EBASE` row arithmetic. Every earlier note here said the
primitives were "covered without being pinned, which is exactly the right relationship"; that was
correct and this is its cost stated as a number rather than as a preference. The top fifteen
clusters are without exception expressions like `(DWIN_WIDTH - w) / 2` and `x + 8 * 3` inside a
`dwinDraw*` call.

**That judgement should be recorded as a judgement, not as a fact about the file.** A display
driver's output *is* its behaviour: a nozzle temperature drawn at the wrong coordinate, or in the
background colour, is a panel that lies, and nothing in this suite would notice. What makes
pinning it the wrong trade here is that the assertion would be a transcription of the code — the
self-consistency trap at scale — not that the fault would be harmless. The three assertion shapes
the tests in this file already use are the affordable substitutes: **content not position** (`???`,
`Nozzle is too cold`), **volume not content** (`updateVariable` sends 0 bytes when nothing
changed), and **destination not appearance** (the menu walks).

**The other 17% is 1281 survivors spread over 371 lines with no cluster larger than 17**, which
by this fork's own convention is the signal to stop. Four things in it are genuinely addressable
and worth knowing before anyone reads the 9.3% as neglect:

- **The four scroll guards** (`select_X.now > MROWS && > index_X`, 68 survivors across the file,
  menu and file lists) are *not* addressable. They move `index_X`, which decides what is drawn;
  `select_X.now` — the row a press dispatches on — is unaffected either way. That is exactly why
  the menu walks pressed every row and killed none of them. Same category as the drawing.
- **The up-directory entry in the file list** (`:1887`, `:2252`, 28 survivors) is real and
  untested: inside a subdirectory one row means "up", and pressing it must leave the directory
  rather than open a file. The existing file-list test only ever runs at the card root.
- **The home-offset limits** (`:3698-3700`, 36 survivors) are real and cheap. `±500, ±500, ±20`
  in tenths — Z is clamped to 2 mm where X and Y get 50 — and `LIMIT()` runs only on the *turn*
  path, which the editor test never takes because it clicks without turning. Z's tighter clamp is
  a safety property worth a test in its own right.
- **The change-detection cache** (`:1757` and neighbours) is reachable by the volume assertion
  already written for `updateVariable`, extended per field.

**A quarter of the mutants would not compile**, which is high and worth expecting on this kind of
file: the driver is dense with macro-built identifiers and `constexpr` table initialisers, and a
source-level mutator edits text without regard for whether the result is still C++. They are
excluded from the denominator, correctly — a mutant that never became a program was never a test
of anything.

Cost, for planning: 32585 mutants took **4.5 GB** on `/mnt/md0` and the run took about an hour at
a steady 200 mutants/minute across 31 workers. Each mutant recompiles a 4356-line translation
unit *and* relinks a binary of 817 tests, so this is several times the per-mutant cost of any
other target measured here. The first rate reading was taken during the workers' cold start and
suggested six hours; **do not extrapolate a mutation ETA from the first minute.**

**And then it was closed, measured (2026-08-14): 83 survivors killed, 9.3% -> 10.3%.**

Four tests written against the survivor list — the home-offset clamps, the up-directory row, and
the per-field change cache. Counted the only way that counts: **re-running the previous run's
survivor list**, because that population is fixed. Subtracting one run's kill count from another's
would have been wrong, since new tests cover new lines and change the denominator.

83 killed, over 22 lines, and every one of them inside a region a test aimed at — 23 on the
home-offset limits, 21 on the up-directory arithmetic, ~39 on the change-detection guards. No
collateral, which is the sign the tests are precise rather than broad.

**Read the 10.3% carefully: it is over the *baseline* population.** A rerun reuses the stored
mutant list and does not re-derive coverage, which is exactly what makes it the right instrument
for "what did these tests kill" and the wrong one for "what is this file's score now". The new
tests certainly cover lines whose mutants were never generated, so a fresh full run would have a
larger denominator. That run was not taken: it costs an hour, and the residue is already
classified.

**`dwin.cpp` is closed.** The four addressable items from the survivor list are done and the top
fifteen clusters are unchanged — every one a drawing primitive with pixel arithmetic, led by 69
on a `dwinFrameAreaCopy` call. What is left is the presentation trade recorded above, and there
is no cluster a test would help with.

**Acting on that measurement found a harness defect worth more than the tests (#57).**

The first item from the survivor list — the home-offset clamps — was written, and the first
version of it failed. The suite then reported **490 tests instead of 818, ending in SIGSEGV**.

The obvious suspect was the leaked home offset: a failing test skips its own destructor, so the
origin stays shifted for everything after it, which is register #47's exact shape. That
explanation fits the evidence completely and is **wrong**. Adding a home-offset restore to the
quiesce changed nothing at all.

The backtrace named something else: **`SerialCapture`'s drainer thread, appending to a
`std::string` in the stack frame the `longjmp` had just discarded.** The destructor that would
have joined it never runs on a failing test, the frame is reused by whatever comes next, and the
process dies. It is the half of register #40 that the earlier fix did not cover — marking the
port unattached in the quiesce stops the *hang*, but nothing stopped the *thread*, and a thread
writing into a dead frame is by far the more serious of the two.

Every test in `test_dwin.cpp` holds a `SerialCapture`, so **any** failure in that file killed the
process and hid every later failure. That had been true for as long as the file has existed, and
it was invisible because the file had never had a failing test outside a deliberate injection.

The fix is that the drainer owns its state through a `shared_ptr` and never touches `this`, so a
skipped destructor leaks a session rather than corrupting memory, and
`SerialCapture::release_live_captures()` stops and joins the leftovers from the quiesce. Sessions
are held in a list rather than one slot because captures nest.

Three things to carry:

- **A plausible mechanism that fits the evidence is not a diagnosis.** The home-offset story
  explained every symptom and was the wrong answer; one backtrace settled it. That restore is
  kept — it is correct hygiene — but its comment now says it was added defensively and was *not*
  the cause, so nobody later reads it as the fix for this.
- **"The suite dies at test N" is rarely about test N.** It is the third time in this fork that a
  fixture outliving its test presented as a fault somewhere unrelated, after registers #26 and
  #40. The pattern is always the same: `longjmp` skips a destructor, and something that should
  have stopped keeps running.
- **The 9.3% mutation score is unaffected, and this was checked rather than assumed.**
  `suite_failed()` in `mutation_test.py` treats any non-zero exit as a failure, so a mutant that
  crashed the binary scored KILLED — correctly, since the suite did detect it. The baseline was
  green, so no survivor could have been mis-scored the other way.

**The up-directory row, and two wrong arms before a right one (2026-08-14).**

Second item off the survivor list: 28 mutants on the two lines computing `hasUpDir`, alive
because **every existing test of that screen runs at the card root**, where the term is zero and
disappears. A test that never leaves the default state cannot see a correction that only applies
outside it.

Two arms now, and both earlier attempts at the second one are the useful part:

- **A root arm that asserted "pressing the row after Back does not climb above the root" was
  dropped, not fixed.** It is satisfied by every implementation, because `cdup()` at the root is
  already a no-op — so the mutant it was aimed at survives it. It also *failed against correct
  firmware*, because the row after Back at the root is simply the card's first entry and the test
  had just put a folder there. Two faults in one assertion: unable to fail for the right reason,
  able to fail for the wrong one.
- **Pressing the far clamp inside the folder also killed nothing.** Dropping `hasUpDir` from
  `filenum = select_file.now - 1 - hasUpDir` makes the index at the clamp one *past* the end, and
  `selectFileByIndexSorted()` leaves `card.filename` untouched rather than reporting a different
  file — so the wrong answer and the right answer read identically. **An off-by-one is only
  visible on a row where both answers are in range**, which here is two rows below Back: Back,
  then `..`, then the first file. That version fails against the injection.

Both arms are separately probed and catch different faults: forcing `hasUpDir` to 0 fails the
go-up arm, dropping it from the index fails the offset arm.

**Home offsets and Advanced Settings, 60% -> 65%, and a new defect (2026-08-14).**

The home-offset menu is pure navigation, so the shared walk is the right instrument again — the
distinction the Prepare menu made is a real one, not a reason to stop using the helper.

**The editor's one division is the whole test.** `hmiHomeOffN()` commits `posScaled / 10`, which
is the entire relationship between the number a person reads off the screen and the number the
firmware acts on. Lose it and a 0.2 mm correction becomes 2 mm — on Z, the difference between a
nudge and driving the nozzle into the bed, with the panel still showing what the operator meant.
**Two points, not one**, because a single value cannot separate a scale factor from an offset: a
driver storing `posScaled - 225` satisfies any test that only ever edits 250. The other axes are
asserted unchanged in the same test, since a driver that wrote every edit into X would pass every
assertion about X.

**Defect #56: Advanced Settings draws a "Bed PID" row that does nothing.** The row's *position*
and its *behaviour* are decided by two different conditions that disagree — `ADVSET_CASE_BEDPID`
is `ADVSET_CASE_HEPID + ENABLED(HAS_HEATED_BED)`, keyed on *having* a bed, `itemAdvBedPID()` is
drawn with no guard at all, and the arm that acts on it is `#if ENABLED(PIDTEMPBED)`, keyed on
*regulating* the bed with PID. The same mismatch sits one row up between `HAS_HOTEND` and
`PIDTEMP`. A thermostat-switched bed is Marlin's default and is every configuration here that
builds this driver, so the row is inert on every machine measured. Selecting it does nothing at
all — no message, no screen, nothing to tell it from a dead encoder.

Two things about how that one is tested are worth carrying:

- **It presses the far clamp rather than walking.** One row below is Nozzle PID, whose arm *is*
  compiled and starts a ten-cycle autotune that would rewrite the hotend's gains for every test
  after it. Winding to the clamp passes over that row without pressing it. **A walk is only safe
  where every row is safe to press**, and this is the menu where that stops being true.
- **The test is guarded on the defect's own precondition** (`HAS_HEATED_BED && DISABLED(PIDTEMPBED)`),
  so a build that resolves the mismatch stops running it rather than starting to fail it. And
  because the assertion is a negative one, it was verified by giving the row an arm: the test
  fails, which is what says it can see the difference at all.

**`dwin.cpp`: 8% -> 35% from eleven tests, and the plan for the rest was wrong.**
161 -> 710 covered lines; the whole `010-dwin` build 60.2% -> 67.4%.

**The drawing primitives turned out not to need tests of their own — they are already executed
as a consequence of the decisions being tested.** That was the standing recommendation here
("what is left is overwhelmingly drawing primitives, where the only observable is the byte
stream and asserting it would pin the protocol") and it does not survive measurement. Driving a
menu draws it; the primitives are covered without being pinned, which is exactly the right
relationship — they are exercised, and nothing asserts their coordinates.

What the remaining 1275 uncovered lines actually are, by the largest spans: the
`ENCODER_DIFF_ENTER` arms of the *other* HMI handlers — Prepare, Control, Motion, Temperature,
AxisMove — each a dispatch switch of the same shape as `hmiMainMenu()`, and each now having a
proven pattern to test it with. Plus the `say_*_en()` label helpers, which are pure output.

So the next slice is **more menu handlers, not primitives**, and the recommendation that said
otherwise was made by reading the function list rather than the coverage. Same mistake as
counting a survivor bucket by lines instead of reading mutants, one level up.

**The stop confirmation, and the position readout (2026-08-13).** Eleven tests in `dwin.cpp` now.

**`hmiPrinting()` / `hmiPauseOrStop()` — pressing Stop asks before it stops.** A print is hours
of work and a knob is easy to knock, so the press opens a confirmation rather than acting. Three
tests: it asks; confirming aborts; **declining leaves the print running**. The last is the one
that matters — a confirmation that stops the print whichever button you choose looks like a
safeguard and is a second way to lose the job.

**Two of those three were passing against a machine that was not printing**, and the precondition
assertion is what caught it. `abortFilePrintSoon()` sets its flag to `isFileOpen()`, so "the
print was not aborted" is true for the wrong reason when nothing is open — and both the
asks-first and the declining test assert exactly that negative. `card.openAndPrintFile()` looked
like the direct route and left no file open at all; `M23` then `M24`, the sequence a host uses,
is what the media tests already recommend and what works. **A negative assertion needs its
fixture's preconditions stated, or it is satisfied by the fixture failing.**

**`_draw_xyz_position()` — an unhomed axis is shown as `???.?`, not as a number.** The most
consequential thing on the status bar: a coordinate implies the machine knows where the tool is,
and before homing it does not. Somebody reading `0.0` off an unhomed Z and lowering the nozzle
"just a little" is the failure the display exists to prevent. Both arms asserted, because a
driver that printed question marks for every axis for ever would pass the first alone and read
as correct.

Reached through `updateVariable()` rather than by exporting `_draw_xyz_position()`, and the
assertion is on the *characters* in the byte stream — `???` is content the panel was told to
show, while the coordinates and font ids around it are protocol no test here has business
pinning.

**`hmiSelectFile()` — both ends of the file list, and three assumptions that were wrong.**
The list is one row of "Back" followed by the card's items, so the file under row `n` is
`n - 1`, and `n - 1 - hasUpDir` once you are inside a folder. Every index is offset by something
that is not always the same, which is the arithmetic that goes wrong quietly: the panel
highlights the name you wanted and opens the one above or below it.

The test leans on the knob to each extreme and presses. One end must be "Back" — the only way
off that screen, and a panel you cannot leave is a panel you power-cycle; the other must be the
card's *last* entry, because `select_file.inc(1 + fullCnt)` clamps there.

**Three drafts, three wrong assumptions, and they are the useful part:**

- **The knob's direction is inverted here relative to the main menu.** I had just written the
  rule about not encoding the harness's conventions and then encoded them again. The fix is the
  same: wind hard against a clamp and let the panel decide which end is which.
- **The simulated card is shared across the whole run.** Other tests leave files on it, so "the
  last file" is whatever the card says, never what this test wrote — and a fixed number of
  detents that crossed the list today stops short tomorrow. The sweep is now derived from
  `card.get_num_items()`. An earlier draft pressed on a file in the middle and reported it as
  the last.
- **The expectation has to be read before the action, not after.** `card.filename` is what the
  driver reached for, and computing what it *should* have been calls `selectFileByIndexSorted`
  again — which overwrites it. That draft compared the value against itself and would have
  passed against any file at all.

**`updateVariable()` — the panel is redrawn only where something changed, and the assertion is
traffic rather than pixels.** It runs on every UI pass and holds a `static` cache of each
displayed value: two temperatures, their targets, the fan, the flow, the feedrate, the babystep
offset. That is not an optimisation to trade away — the link to the panel is a 128-byte buffer
the firmware busy-waits on, so a screen redrawn wholesale every pass would spend the print
blocking on it, which is the same property `SerialCapture` needs a second thread for.

**Measured: a settled machine costs 0 bytes and a changed hotend target costs 42.** The first
version asserted only `changed > quiet`, which a wasteful implementation would also satisfy;
probing the actual numbers turned it into `quiet == 0`, which is the real claim. Worth doing
whenever a comparison passes on the first try — the strong form is often available and the
weak one looks identical in a green run.

Asserting volume rather than content is deliberate. Decoding the DWIN wire format would pin
pixel positions and font ids, none of which is the behaviour under test; "a pass with nothing
to say sends nothing" is.

**The menus rate-limit the knob and the value editors do not, which is why a navigation test
needs the clock.** `hmiMainMenu()` reads through `get_encoder_state()`, which ignores everything
for `ENCODER_WAIT_MS` (20 ms) after each accepted event; `hmiStepXYZE()` and its siblings call
`encoderReceiveAnalyze()` directly and have no such gate. Under a HAL where time only moves when
a test says so, that means **consecutive turns are simply swallowed** — the first navigation test
walked the whole menu and never left the first page. The pump has to advance the clock past the
gate, which is what a person's hand does without thinking about it.

**And the navigation test assumes neither the knob's direction nor its gearing**, because two
earlier versions did and failed in two different ways: clockwise turned out to *decrease* the
selection, and one `turn_clockwise()` call did not reliably move it one page. Neither is a
property of the firmware worth pinning — both are properties of how this fixture and this panel
happen to agree — so the test winds hard to one end, walks back one detent at a time clicking as
it goes, and asserts the **sequence of distinct destinations**. That is the claim worth making:
four screens, reachable, adjacent in the order the icons are drawn, no two pages leading to the
same place. It would survive rewiring the encoder.

**Two new defects, both in `make_name_without_ext()` — the function that decides what a file is
called on the menu you pick a print from.**

- **#54: it takes its starting index from a global rather than from the string it was passed.**
  `strlen(card.longest_filename())` indexed into `src`. All three callers happen to pass exactly
  that, so today it is redundant rather than dangerous — but the signature says the function
  works on `src`, and it half does.
- **#55: a name with no dot comes out empty.** The backwards search walks to zero and zero is
  taken as the length, so the row is blank. **Latent**: `is_visible_entity()` only lists a file
  whose 8.3 extension begins with `G`, and the 8.3 name is derived from the long one, so a
  dotless file never reaches the menu. Directories skip the search entirely, which is why a
  folder called `v1.2` keeps its dots.

#55 is the one worth remembering how it was found: **the test was written for the sensible
answer and the code did something else.** Both are pinned to what the code does, with the folder
arm alongside the file arm so the pair says where the fault is rather than merely that there is
one.

**`marlinui.cpp`: the message-template expander (2026-08-13). 44% -> 67% under `010-dwin`.**

The default configuration's 22% was mostly *not compiled* — the file is 72 countable lines
without a display and 179 with one, so `010-dwin` is where it can be measured at all.

`expand_u8str_P()` is what turns a localised template into a message, and its contract is
written at the top of `language_en.h`: `$` inserts a string, `{` gives the tool's zero-based
index, `~` the same tool one-based, `*` that with an `E` in front, `@` an axis letter. **Four
different numberings of the same tool**, which is exactly the kind of thing that is easy to get
wrong and impossible to notice — a fault here does not crash anything, it tells somebody to
check the wrong nozzle or moves the wrong axis in a prompt they are about to agree to.

Nine tests, every one an exact string rather than a substring, because `contains("E1")` is
satisfied by `"E11"` and the off-by-one between the three tool forms is precisely what a loose
assertion misses. The templates are written as literals rather than taken from `language_en.h` —
asserting `MSG_MOVE_N` against `MSG_MOVE_N` would be comparing the code to itself.

Two process findings on the way, both now gotchas in `CLAUDE.md`: `pio test -f <name>` is a *test-name*
filter and not a configuration selector, and **`MAX_MESSAGE_SIZE` is 1 without a display**. The
second is what made the first visible — the tests passed under a run I believed was the default
config and failed the moment `pio run -t marlin_default` rebuilt it properly.

### What a host-buildable display driver actually buys

`SerialCapture` now takes the port to watch, so the drainer the host-facing tests already
used works on `LCD_SERIAL` too. That matters because a DWIN panel is not an interface the
firmware calls — it is a screen the firmware writes bytes at, so there is no `stub_extui`
equivalent to record and the byte stream is the only observable. The drain has to run on
another thread for the documented reason: the write busy-waits for room in 128 bytes, a
screen refresh is longer than that, and draining afterwards means draining a buffer whose
producer is already wedged.

**Half the seam works.** `a_status_message_reaches_the_panel` is the first assertion any LCD
driver has ever had here.

**The other half needed a hand, and now has one.** Register #33's write lives in
`hmiStepXYZE()` and happens only on an encoder click; `encoderReceiveAnalyze()` gates it on
`BUTTON_PRESSED(ENC)`, which reads `BTN_ENC` — and `BOARD_SIMULATED` defined **no encoder pins
at all**, because `HAS_DWIN_E3V2` is neither `HAS_WIRED_LCD` nor a TFT UI and fell through both
branches of `pins_RAMPS_NATIVE.h`. So the macro was a compile-time false and no call sequence
reached the assignment. Making a driver host-buildable is not the same as making it drivable.

Three GPIOs in the pins file and `tests/support/simulated_encoder.h` close that, and
**#33 is now confirmed by behaviour**:
`changing_the_resolution_at_the_panel_leaves_the_reciprocal_stale` turns the knob, clicks, and
asserts the stored resolution changed while `mm_per_step` did not follow. Nothing is set behind
the driver's back — the starting value is below the driver's own lower limit, so the committed
value is the driver's choice and not the test's.

The fixture is worth reading before writing another like it. The firmware, not the fixture,
does the sampling: a phase change is only seen by a call into the driver and the delta is
consumed by that same call, so every motion takes the caller's pump. And a phase change costs
two passes and 3 ms of simulated time, because `MarlinUI::get_encoder_delta()` debounces —
the first pass after an edge only starts the timer.

**Validated by injecting the fix.** Adding `planner.refresh_positioning()` after `dwin.cpp:1647`
makes that test — and only that test — fail. Doing it also broke the suite twice over and found
two harness defects worth more than the test (registers #39, #40): panel buttons power up held,
which cost nothing but turned an 8-second run into ten minutes with everything still passing;
and `SerialCapture`'s destructor is skipped by Unity's `longjmp`, leaving a port connected with
nothing draining it, so the *next* long write anywhere in the suite never returns. Both are
fixed in `quiesce_simulated_peripherals()`, which runs on the far side of the jump.
