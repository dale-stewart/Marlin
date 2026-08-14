# Interrupting a print: `pause.cpp` and `e_parser.cpp`

The two ways a print stops early — the filament change that means to come back, and the emergency
stop that does not. Both were at 0%, and both were named by the
[survey](survey-2026-08-14.md). Includes the fixture faults that had to be cleared before either
could run at all.

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

## `feature/e_parser.cpp`: 0% -> 63%, and defect #59

The other target the survey named, and the cheapest thing in this repository to test properly:
`EmergencyParser::update(state, c)` is a **pure state machine** over a state and one byte. No
hardware, no clock, no fixture — the tests feed it strings and read the flags. 44 of 69 lines from
eight tests, and the whole `003` configuration 83.7% -> 84.6%.

It is also the code with the least excuse for being untested. It reads the serial stream character
by character *ahead of the queue*, so that a machine wedged in a two-hour print with a full command
buffer can still be stopped. It is the last thing between a runaway printer and the mains.

Two properties that pull against each other: recognise the handful of emergency commands in a
stream of arbitrary G-code, and recognise as little else as possible. **Writing the second half is
what found the defect** — the test asserting that near-misses do nothing failed on its first run.

**Defect #59: a longer command number beginning with an emergency command triggers it.** `M1121`
halts the machine; `M4100` requests a quickstop; `M5240` abandons the print. Once the machine
reaches a terminal state such as `EP_M112`, the outer `switch` has no case for it, so every further
character falls to `default:` — which acts only `if (ISEOL(c))` and otherwise leaves the state
alone.

That stickiness is *wanted*. It is what lets `M112 ; stop now` and `M410 S1` work, and the same
code cannot tell a trailing digit from a trailing semicolon. So the test pins **both halves**: the
trailing digit that should not fire and the trailing comment that should, because a fix that broke
the commented form would be worse than the defect. Latent, because none of `M1121`, `M4100`,
`M1080`, `M5240` is a real command and no host sends one — but a file can contain one, and the
consequence is a print halted at a line that meant nothing.

**The flags are latches, and that cost a third leak.** `killed_by_M112` is read by the queue, which
halts the machine; nothing clears it but the code that acts on it. The one failing assertion above
skipped its fixture's destructor and left it set, and the run went from 767 tests in 13 s to **186
in eleven minutes** — a halted machine still answers, so it presents as the suite grinding rather
than failing. The reset now lives in `quiesce_simulated_peripherals()` alongside the others, and it
was verified by injection: with a deliberately failing parser test, the suite completes all 767 in
11.8 s.

That is the third time in one session that a fixture outliving its test presented as something
other than a failure — after `SerialCapture`'s thread (#57) and the runout pins. The pattern is
worth more than any of the three individually: **anything a test sets that the firmware treats as a
mode belongs in the between-tests hook, not in a destructor.**

## Not done

The `M876` prompt-answer path in the parser, and answering the "Purge More / Resume" menu as a host
would — both need a test standing in for a host rather than for a person.
