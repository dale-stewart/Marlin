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

**Commands that wait still do not complete.** With `idletask()` advancing, `M400` was
observed with the step timer enabled, the planner busy, and five `idle()` calls
producing zero steps. The cause is scheduling, not the clock: when a block finishes,
`Stepper::isr()` programs a long interval, and queueing a new block does not bring the
timer's next fire forward. Advancing in 100 µs slices then never reaches it. On hardware
`stepper.wake_up()` re-arms the interrupt for exactly this reason.

The next step is therefore in `Timer`/`timers.cpp`, not in the clock: enabling an
interrupt (or programming a shorter compare) must bring `next_fire_ns` forward rather
than leaving a stale far-future value. `Timer::enable()` currently calls `schedule()`,
which is close — but `Stepper::wake_up()` only calls `ENABLE_STEPPER_DRIVER_INTERRUPT()`,
and if the timer is already enabled that is a no-op.

## Rules

1. **Never read a real clock here.** Anything that consults wall time defeats the point.
2. **Never install a signal handler.** If something appears to need one, it wants a
   timer that a test advances instead.
3. **Interrupts fire only from `HAL_test_advance_*`.** Calling an ISR directly is a
   LINUX-HAL habit; here it steps the machine while the clock stands still.
4. Keep the divergence from `HAL/LINUX` to `Clock`, `Timer` and `timers`. Everything
   else should stay copy-identical so fixes to one apply to the other.
