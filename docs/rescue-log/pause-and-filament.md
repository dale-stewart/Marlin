# Interrupting a print: `feature/pause.cpp`

The filament change — park, wait for a person, come back — and the fixture faults that had to be
cleared before any of it could run.

Part of the rescue log — see [README.md](README.md) for the index and `CLAUDE.md` for the rules
that apply to every session.

## `feature/pause.cpp`: 0% -> 44% (2026-08-14)

Picked by the [survey](survey-2026-08-14.md) as the largest fully-untested compiled file: 156
lines, built by `002` and `003`, never once executed. Now 69 of those lines, and the whole `003`
configuration 81.3% -> 83.7%.

Four tests, all asserted through behaviour because **`resume_position` is file-static** — which is
the right constraint rather than an obstacle. What matters is not that a number was stored but
that the tool comes back, so the tests move the machine, pause it, and look at where it ends up.

- **The tool parks away from the part and returns to where it was printing.** The park point is
  nowhere near the print position, so "it came back" cannot be satisfied by a machine that never
  moved, and the parked position is asserted in between so it cannot be satisfied by one that
  failed to park. Z is checked separately from XY because `resume_print()` restores them with two
  moves in a deliberate order — XY first at the parked height, then Z down. Reversing that would
  drag the nozzle across the part on the way back.
- **A second pause does not overwrite the first one's saved position.** `pause_print()` opens with
  `if (did_pause_print) return false`, which reads as tidiness and is not: the line after the
  guard is `resume_position = motion.position`, and by the time a second pause arrives the machine
  is *parked*. Without the guard the saved position becomes the park point and the rest of the
  object is extruded into thin air twenty millimetres away. The assertion is therefore not that
  the second call returned false, but that after two pauses the machine still knows where the
  print was.
- **The job clock stops while paused and runs again after.** A filament change can take an
  afternoon, and a timer left running makes every estimate for the rest of the job wrong —
  silently, and in a way that looks like the estimate was simply bad. Both directions, since a
  machine that never restarted the clock passes the first assertion perfectly.
- **An unhomed machine pauses without parking.** Coordinates mean nothing before homing, so the
  print still stops — that part is right — but the move does not happen.

## What it costs to run this file at all

Three fixture traps, each of which presented as a **stall rather than a failure**, and each of
which needed a backtrace rather than reasoning. The general shape is the transferable part:
**this file is mostly waits for a person**, so a test of it must state which of them it is not
exercising rather than inherit the defaults.

- **The nozzle must sit *at* its target, not above it.** `ensure_safe_temperature(false)` spins
  while `|current - target| > TEMP_WINDOW`, and `TEMP_WINDOW` is 1 °C. A fixture pinning the
  sensor at 220 against a target of 210 never enters the window.
- **Being paused is what arms the runout sensor.** `should_monitor_runout()` is
  `did_pause_print || printingIsActive()`, and a simulated pin reads LOW, which
  `FIL_RUNOUT_STATE` defines as *no filament*. The monitor then responds from inside `idle()` —
  which `planner.synchronize()` called — and calls `planner.synchronize()` again. The stack is
  `synchronize -> idle -> run -> synchronize -> idle`.
- **`resume_print()`'s defaults are written for a machine with somebody standing at it.**
  `show_lcd` defaults to true, which opens the "Purge More / Resume" menu and waits for an answer
  that on this build can only come from the *host* — `M600_PURGE_MORE_RESUMABLE` is enabled by
  `EMERGENCY_PARSER` + `HOST_PROMPT_SUPPORT`, not by any display. `purge_length` defaults to 50 mm
  at 3 mm/s. `pause_for_user` reaches the insert-filament wait.

Answering the purge prompt as a host would is a worthwhile test. It is a different one, and it is
not written yet.

## The fixture fault underneath it all (register #58)

Worth reading as a debugging exercise rather than as a fact about pause, because pause turned out
to have nothing to do with it.

`resume_print()` was consuming **15 hours of simulated time**. The chain, each step measured:

1. Probes localised it to between `ensure_safe_temperature()` and the return moves.
2. Timing a bare E move under five conditions — clean, extruder disabled, extruder enabled, after
   a pause, paused with the extruder re-enabled — gave **the same figure to within 1 ms**. That
   ruled out pause, the extruder disable, and the runout monitor in one measurement.
3. The planner reported E's max feedrate and acceleration as **0**: the fixture set
   steps-per-millimetre with `LOOP_LOGICAL_AXES` (includes E) and the limits with `LOOP_NUM_AXES`
   (excludes E). Fixing that took a 2 mm retract from 27,487,964 ms to 171,932 ms — 160× better
   and still wrong.
4. The remaining cost **did not scale with distance**: 2 mm and 20 mm both took ~172 s. A fixed
   wait, not a slow move.
5. It reproduced under `001-default`, so nothing to do with extruders, runout or pause. And the
   discriminator: **X+E took 152 ms while E alone took 172 s.**
6. That asymmetry names the mechanism. `planner.cpp:2369` picks one of *three* accelerations per
   move — `travel_acceleration` when nothing extrudes, **`retract_acceleration` when only the
   extruder moves**, `acceleration` otherwise. The fixture set two of the three. Adding an X
   component changes which constant is reached for, which is why the combined move was fast.

Both fixes are in `simulated_machine.h`. The measurements sit beside them, because the next person
to read `LOOP_NUM_AXES` there needs to know what it cost.

**Neither was a firmware defect.** A limit of zero does not refuse a move; the planner scales it
down, the stepper takes exactly the right number of steps, and the machine arrives in exactly the
right place — at about one step per second. Nothing fails. The suite stops finishing, which is
what the leaked-scale note in `quiesce_simulated_peripherals()` already warned about, and it took
the first E-only move in this suite's history to expose it.

## Harness changes that came with it

`quiesce_simulated_peripherals()` now also resets `did_pause_print` and returns the filament
sensors to their power-on state. The second is not hypothetical: the pause fixture drives those
pins, and `runout___poll_runout_states` read the leftovers two files away — expecting 7, reading 0.

## Not done

`feature/e_parser.cpp` — 69 lines, 0%, the emergency parser that recognises `M108`, `M112` and
`M410` in the serial stream *before* the queue, so a machine already stuck can still be stopped.
It was the second target named by the survey and is still untouched.
