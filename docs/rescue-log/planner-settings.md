# The `planner.settings` seam, and how far its migration got

The blocked design correction in `planner.h`, what it is waiting on, and the measured state
of every consumer. Includes the closures of `motion.cpp` and `planner.cpp`.

Part of the rescue log — see [README.md](README.md) for the index and
`CLAUDE.md` for the rules that apply to every session.

### State of the `planner.settings` seam

The blocked correction in `planner.h` — removing the public `settings`/`mm_per_step` and the raw
`block_buffer` — needs its *callers* covered first. Measured against the default build (word
boundaries; a prefix grep wrongly puts `marlinui.cpp` in this list, because its only reference is
the `block_buffer_runtime()` accessor):

| caller | line | mutation | note |
|---|---|---|---|
| `module/tool_change.cpp` | 97% | 41.2% | measured under `extruders_3_runout`; not compiled by default |
| `gcode/config/M92.cpp` | 100% | 66.1% | |
| `gcode/config/M200-M205.cpp` | 94% | 57.6% | |
| `module/stepper.cpp` | 96% | 68.9% | |
| `gcode/calibrate/G28.cpp` | 91% | 66.4% | |
| `gcode/motion/G2_G3.cpp` | 91% | 58.4% | |
| `module/settings.cpp` | 95% | 38.1% raw / **60.8% killable** | measured under `006-eeprom`; 51 covered lines become 303 |
| `module/motion.cpp` | 76% | 57.8% raw / 75.3% killable | |
| `module/temperature.cpp` | — | — | **not a blocker**: its three references are inside `MPCTEMP`, which is off here |

**`settings.cpp` is closed at 38.1% raw / 60.8% killable.** The raw figure is low and stays low
for a reason worth knowing: **256 of its 424 survivors are placeholder constants**. When a
feature is compiled out, `save()` still writes something in its slot to keep the block layout
stable across builds — `const xyze_pos_t planner_max_jerk = LOGICAL_AXIS_ARRAY(5, 10, 10, …)`
with `CLASSIC_JERK` off, `autoretract_defaults` with `FWRETRACT` off — and `load()` reads the
same slot into `dummyf` and discards it. Verified by changing the values wholesale: nothing
observes them. They are killable only by a build that has the feature, which is the whole point
of writing them.

What is left after that is reporting: `report()` on boot, `report_position()` when a load moved
the machine, and the debug lines around the CRC and version messages. No cluster above eight.

The behaviours that matter are pinned by `test_settings_storage.cpp`: the round trip, a second
save replacing the first, a reset leaving the stored block alone, and three ways a bad block is
refused — checksum, version, and a stored value that is not a number. Each failure is asserted to
be *reported* as well as refused, because a machine that quietly forgets its calibration is worse
than one that says so.

### Where the `planner.settings` migration stopped, and why

Every write to `axis_steps_per_mm`, `max_acceleration_mm_per_s2` and `max_feedrate_mm_s` in this
build now goes through `Planner`, single-axis or bulk. Reads too, for resolution. What stops the
fields becoming private is two things, and only one of them is about coverage.

**Consumers outside every test configuration.** Four LCD drivers and the I2C position encoder
assign these arrays and are compiled by no configuration under `test/`. Two of them are already
suspected wrong (register #33). Covering them needs configurations that build them, which is the
same problem `006-eeprom` solved for `settings.cpp` and the same solution.

**The setters conflated two operations — now separated.** `set_max_acceleration(axis, v)` and
`set_max_feedrate(axis, v)` clamp and warn under `LIMITED_MAX_*_EDITING`, which is right for *a
user naming a limit* and wrong for *the firmware restating one*. `override_max_acceleration()` and
`override_max_feedrate()` are the second operation: taken as given, not announced, and still
keeping the derived step-rate limits in step. `set_*` delegates to `override_*`, so the invariant
lives in one place and the difference between the two is exactly the clamping.

`G28`'s `begin_slow_homing()` and `M92`'s low-`E` compensation are the two firmware overrides, and
both now say so. Enabling `improve_homing_reliability` in `005-bed_leveling` is what made the
first of those safe to touch: that block was compiled by no configuration, and the existing homing
tests exercise it the moment it is built (G28 96% there). It is executed rather than strongly
asserted — nothing measures homing acceleration — which is worth knowing before leaning on it.

With that, **every raw write to the three per-axis arrays in compiled code is gone**.

**And there the migration ends, permanently, short of making the fields private.** The five
remaining consumers — `sovol_rts`, `creality/dwin`, `mks_ui/draw_number_key`, `extui/ui_api` and
`encoder_i2c` — are not merely untested. They cannot be built for the host at all, which was
checked rather than assumed:

- `SOVOL_SV06_RTS` fails to compile: it wants Arduino's `String`, and its `sendData(int, …)` and
  `sendData(int32_t, …)` overloads are the same signature on a 64-bit target.
- `EXTENSIBLE_UI` **is now buildable**. `ui_api.cpp` links only against a concrete UI supplying
  twenty-two `ExtUI::on*` callbacks; `tests/support/stub_extui.cpp` is the smallest one that
  satisfies the linker, and it *records* rather than discards — empty bodies would compile just as
  well and would make the interesting half untestable, since the contract of ExtUI is that the
  firmware tells the display when things happen and silence is the failure that matters. Config
  `008-extui`.

  Note the two directions need different tests. Homing, resets and status messages are the
  firmware calling *out*, and those live at the firmware's call sites, not in `ui_api.cpp` — three
  scenarios of that left the file at 0%. `ui_api.cpp` is what a display calls *in*, so covering it
  means a test standing in for the display. Only the migrated setters are covered here (2%);
  covering the rest of that 226-line API is a separate and much larger job with no defect behind
  it.
- `I2C_POSITION_ENCODERS` **is now buildable and tested** — it needed `<Wire.h>` (a stub bus, on
  the same footing as `HAL/TEST/spi.cpp`) and Arduino's legacy `Bxxxxxxxx` binary-literal macros,
  both now in `HAL/TEST/include/`. Config `007-i2c_encoders`. The bus grew an `I2CDevice`
  attachment seam and `tests/support/simulated_i2c_encoder.h` answers on it, reading its count
  from a `SimulatedAxisWithLimit` — the carriage, not `stepper.position()`, because an encoder
  driven by the firmware's own belief could never disagree with it, which is the one thing an
  encoder is for. 2% → 13%.

  Note `passes_test()` reports the field strength recorded by the last `get_raw_count()` and does
  not fetch one itself. A test that asks without reading gets "never seen" whatever is on the bus,
  which is how the first version of the bad-field test here passed against a perfectly healthy
  encoder. `ConfiguredEncoder::passes_its_test()` in `test_i2c_encoders.cpp` reads first.

Worth knowing before picking the next one: **effort and value are anti-correlated here.** The
cheapest to make buildable — `extui/ui_api.cpp` — is the one that already gets the refresh right.
The ones with suspected defects (#33, #34) are the display drivers, which are the expensive ones,
and `mks_ui` needs a third-party graphics library.

So this is not a coverage gap and no amount of testing closes it. Making those drivers
host-portable is a real project and a separate one, and it would have to be sequenced *before*
this migration rather than inside it. Until then the fields stay public with the reason written
where they are declared, and register #33 records the two drivers already suspected of the
mistake the encapsulation exists to prevent.

**Migration in progress — slice 1 of the `planner.settings` correction is done.**
`Planner::steps_per_mm(axis)` and `Planner::set_steps_per_mm(axis, value)` now exist alongside
the public array, and `M92` uses them. The defect being corrected is a public mutable field with
a derived cache: `mm_per_step` is the reciprocal and the stepper counts in steps, so changing the
resolution invalidates both — and until now keeping them in step was the caller's job to
remember, with nothing connecting the array to `refresh_positioning()` but a comment. Forgetting
it does not fail; the machine keeps moving at a scale that no longer matches what it reports.

The array is still public because `settings.cpp` still writes it during a bulk restore, which is
a different pattern (set everything, finalise once) and a separate consumer. That is the
sequenced migration working as prescribed: new API alongside the old, old retired as consumers
arrive.

The refactor touched **three production files and no test file**, and the 24 acceptance scenarios
stayed green and unedited throughout — which is the only thing that makes it evidence.

**What the acceptance suite protects on its own (Step 7).** Measured with the unit tests
excluded, `pio run -t marlin_eeprom -e acceptance_native_coverage`:

| consumer of `planner.settings` | acceptance-only line coverage |
|---|---|
| `module/settings.cpp` | 90% |
| `gcode/config/M92.cpp` | 68% |
| `gcode/config/M200-M205.cpp` | 50% |
| `module/stepper.cpp` | 74% |
| `module/motion.cpp` | 57% |
| `gcode/calibrate/G28.cpp` | 91% |
| `module/planner.cpp` | 62% |

Those motion figures were 13%, 5%, 0% and 11% until `moving_the_tool.feature` was written, and
two separate things had to change to fix that. The obvious one was six scenarios. The other was
that **`acceptance_native_test` extended `env:linux_native_test`** — the LINUX HAL, where time is
the wall clock, so a scenario could not wait for the machine to arrive anywhere and motion was
not expressible at all. Everything else in this fork moved to the test HAL; the acceptance envs
were left behind, and the effect was a silent ceiling on what the acceptance suite was allowed to
be about. They now extend `testhal_*`, and `[acceptance_only]` pulls in `tests/support` because a
scenario that homes needs rails and switches to home against.

`keeping_its_settings.feature`, `moving_the_tool.feature` and their steps name no C++ symbol,
teardown included. That is what lets them stay unedited while `planner.settings` is migrated
underneath them — the unit tests cannot do that job, because 117 of their references name the
symbol being removed and a net that moves with the code is not a net.

**`motion.cpp` is closed at 57.8% raw / 75.3% killable** (204/353; 82 of 149 survivors are
equivalent), 76% line coverage. Reason categories, all checked: 24 preprocessor-erased because
this build has no probe, 23 outside their variable's reachable range (`axis_home_dir` is always
`-1`, and no `HOMING_BUMP_DIVISOR` entry is below 1), 14 where an early-return shortcut agrees
with the general formula it skips — `get_move_distance` returns `ABS(diff.z)` for a Z-only move
and `SQRT(sq(dx)+sq(dy)+sq(dz))` gives the same — 12 masked by the duplicated extrusion guard
(register #30), 5 zero-length moves filtered by `MIN_STEPS_PER_SEGMENT`, and 4 on
`final_approach`, which is read only inside a `HOMING_Z_WITH_PROBE` block. The remaining 67 are
spread across 44 lines with **no cluster larger than three**, which is the signal to stop.

**`planner.cpp` is closed at 60.5% raw / 73.2% killable** (612/1012; 176 of the 400 survivors
are equivalent). The reason categories, all checked rather than inferred: 76 masked by the
junction-deviation cap (register #29), 47 erased by the preprocessor (the fan guards and the
`TERN0(FTM_CONSTANT_JOLT, …)` at `:1777`, which never evaluates true here), 19 outside their
variable's reachable range — `:834`'s `accel` was measured across the whole suite and spans
120000..640000000, so every relational mutant of `!= 0` agrees with it — 15 direction pins for
axes that are not moving, 11 guards that only skip redundant work, and 8 at `:2432`, where the
integer and floating-point acceleration limits are two forms of one formula and **both arms
run** (209 and 40 times), so the split is a shortcut, not dead code. What is left has no cluster
larger than seven.
