# HAL/TEST — a HAL for unit tests

## What it is for

The LINUX HAL runs Marlin on a workstation, so it reads a real clock and builds its
timers on POSIX interval timers. That is right for the simulator and wrong for tests:
an interrupt can arrive between any two assertions, and code that waits — `dwell()`, a
temperature residency check, a homing timeout — waits in real time or, in a test build
where `main()` never starts the timers, forever.

This HAL inverts that. **Time only moves when a test says so, and interrupts fire
because time moved.**

- `hardware/Clock.h` — a counter, not a clock. `advance_nanos/micros/millis` are the
  only way time passes. Delays advance it rather than blocking.
- `hardware/Timer.h` — a compare value and an enabled flag. No signals, no handlers.
- `timers.cpp` — `HAL_test_advance_*()` moves the clock and runs whichever interrupts
  fall inside the interval.

Everything else — `Gpio`, pin mapping, serial, EEPROM — is carried over from the LINUX
HAL unchanged.

## Status

**Motion works.** The five `simulated_motion` tests pass with exact step counts, driven
by advancing the clock rather than by calling the ISR.

**`idletask()` advances the clock**, which is where waiting should cost time: Marlin
waits by spinning on `idle()`, and on hardware each pass burns microseconds while
interrupts fire. This is a HAL responsibility, so it needed no change to Marlin itself.

**Commands that wait now complete.** `M400`, `G4` and an arc are covered by
`tests/gcode/test_blocking_commands.cpp`.

The earlier diagnosis recorded here — that `Stepper::isr()` left `next_fire_ns` stale in
the far future and `Timer::enable()` had to pull it forward — was **wrong**, and
`Timer`/`timers.cpp` needed no change. The "five `idle()` calls, zero steps" observation
was simply too short a look: `Planner::get_current_block()` holds the first block back
for `BLOCK_DELAY_FOR_1ST_MOVE` (100) interrupts while fewer than three moves are queued,
and each of those interrupts programs a ~25 ms "nothing to run" interval, so the first
step lands a few hundred milliseconds of simulated time in. Advancing the clock in 100 µs
slices does reach it, and always did — that is why `run_until_idle()` worked.

The real cause was a pin, not a timer. `KILL_PIN` is an input with a pull-up on a board,
so it reads HIGH — released — from reset. Every simulated pin reads LOW at reset, and LOW
is `KILL_PIN_STATE`, so the firmware sees the kill button held down.
`Marlin::manage_inactivity()` debounces it over 250 passes, so the 250th `marlin.idle()`
call in the process reached `kill()`, which spins forever waiting for the button to be
released. Anything that waits by calling `idle()` more than 250 times hit it; nothing
else did, which is why only the blocking commands hung.

`Marlin::setup()` — which configures that pull-up — does not run in a test build, so
`SimulatedHardware` stands in for it and releases the button. That is the general shape of
this HAL's remaining hazards: pins power up in a state no board would ever be in.

**Temperature works too.** `Temperature::init()` arms and enables `MF_TIMER_TEMP`, so
advancing the clock runs the real `Temperature::isr()` and the real ADC pipeline.
`tests/support/simulated_hardware.h` does the bring-up once per process.

Two things had to be true first, and both were pins or timers rather than logic:

- `thermalManager.init()` was recorded as crashing with SIGFPE. It does — under the
  LINUX HAL, and only if `HAL_timer_init()` has not run. `Temperature::init()` calls
  `HAL_timer_start(MF_TIMER_TEMP, …)`, and `LINUX/hardware/Timer.cpp` divides by the
  timer's own `frequency`, which is zero until `Timer::init()` has set it. Exactly the
  cause that once broke `stepper.init()`. This HAL guards the division, so it never
  crashed here at all.
- The thermistor inputs are the ADC's version of `KILL_PIN`. A simulated pin reads zero,
  zero is a shorted sensor, and a shorted sensor converts to 320 °C — over
  `HEATER_0_MAXTEMP` and `BED_MAXTEMP`. The first `Temperature::task()` after the ISR
  starts producing readings would call `kill()`. `SimulatedHardware` drives every analog
  input to a room-temperature count before anything can read one.

**Endstops and homing work, with one exception.** Tripping a simulated endstop needed no
HAL change at all — the recorded blocker ("`endstops.update()` only records a hit while
the axis is moving") was simply the absence of motion, and motion now works. A limit
switch is a `Peripheral` on the STEP pin (`tests/support/simulated_endstops.h`): it counts
pulses, moves a carriage, and drives the endstop pin when the carriage reaches a chosen
place. `G28` homes on that. `tests/module/test_homing.cpp` has the tests.

Note what the endstop pins did *not* need. A simulated pin reads LOW and
`X_MIN_ENDSTOP_HIT_STATE` is HIGH, so every switch reads *open* from reset — which is the
state a board powers up in. `SET_INPUT_PULLUP` here only records a mode; it does not
change the value, which is why `release_kill_button()` has to write the pin as well. The
hazard that bit `KILL_PIN` and the thermistors does not exist for endstops.

**The exception is `Timer::enable()`, and it is this HAL's own bug.** `enable()` calls
`schedule()`, which restarts the period — `next_fire_ns = now + period`. Hardware does not
do that: enabling a compare interrupt sets an interrupt-enable bit and leaves the counter
running. So under this HAL, anything that disables and re-enables a timer *more often than
that timer's own period* pushes its due time away forever.

One thing does. `Stepper::endstop_triggered()` wraps its body in
`ATOMIC_SECTION_START/END`, which is a `suspend()`/`wake_up()` pair on `MF_TIMER_STEP`.
While a closed switch sits in front of an axis that is moving towards it,
`Endstops::poll()` calls that from *every* temperature interrupt (~1 ms), and the step
interval at the head of a block is longer (~2.8 ms). The step ISR never fires, so the
abort it was just asked to perform is never carried out, `axis_did_move` is never cleared,
and the next temperature interrupt asks again. Simulated time keeps advancing throughout,
so it presents as a hang rather than a failure — and the backtrace points at
`planner.synchronize()`, which is the symptom, not the cause.

A switch that closes *during* a move is unaffected: by then the step interval is far
shorter than the temperature period. The one behaviour this costs is homing an axis that
is already sitting on its switch — a second `G28` with no `HOMING_BACKOFF_POST_MM`.
Deleting `schedule()` from `enable()` was verified to fix it with the whole suite still
green; it is left for the owner of this HAL to make, and is pinned meanwhile by
`endstops___a_move_that_starts_against_a_closed_switch_stalls` and recorded as #18 in
`docs/defect-register.md`.

The general shape is worth keeping in mind for the rest of this HAL: **`enable` is not
`arm`**. Only `start()` and `setCompare()` should move a timer's due time.

Note also that `Stepper::init()` ends by starting *and enabling* the step timer. Under
the LINUX HAL that is a live POSIX timer delivering SIGRTMIN at 122 Hz into the real
stepper ISR, so anything that initialises the hardware there must mask it immediately —
`SimulatedHardware` does, rather than leaving it to whichever fixture happens to follow.

## Rules

1. **Never read a real clock here.** Anything that consults wall time defeats the point.
2. **Never install a signal handler.** If something appears to need one, it wants a
   timer that a test advances instead.
3. **Interrupts fire only from `HAL_test_advance_*`.** Calling an ISR directly is a
   LINUX-HAL habit; here it steps the machine while the clock stands still.
4. Keep the divergence from `HAL/LINUX` to `Clock`, `Timer` and `timers`. Everything
   else should stay copy-identical so fixes to one apply to the other.
