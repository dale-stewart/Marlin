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

Complete. Motion works: the step timer is armed and enabled, `run_until_idle()` advances
the clock instead of calling `stepper.isr()`, and both `testhal_native_test` and
`linux_native_test` are green at 377.

Two diagnoses along the way were wrong, and both are worth remembering.

The first was that `HAL_timer_interrupt_enabled` read false after `stepper.init()`
because the HAL failed to arm the timer. It was the fixture disabling it four lines
later - leftover code whose purpose under the LINUX HAL was suppressing POSIX signals.
That is now `#ifndef __PLAT_TEST__`.

The second was that enabling the interrupt hung the suite in the `SimulatedMachine`
constructor, and that the cause was re-entrancy or a zero-length period. Neither was
true, and the premise was not either: the binary's stdout is block-buffered when piped,
so a hang anywhere loses *all* output. It was hanging in the first motion test, and it
hung there with interrupts still disabled.

The cause was `Timer::getCount()`, in two ways - see the comment on it. It returned an
absolute tick count where the hardware (and the LINUX HAL) return ticks since the timer
last restarted, which wrecks `Stepper::isr()`'s interval arithmetic: `min_ticks` is
computed as `getCount() + margin` and compared against an *interval*. And, fatally, it
answered without moving the clock. Marlin times its step pulses by spinning on that
counter (`AWAIT_TIMED_PULSE` in stepper.cpp), a loop that on hardware ends because the
CPU burns cycles while the counter runs; here nothing else moved the clock inside it, so
it spun forever. Reading the counter now costs one tick, which is the simulated
equivalent of the cycles the poll would have taken, and is deterministic.

`Timer::advance()` has been replaced by `pending()`/`fire()`, and the scheduler in
`timers.cpp` now walks the clock to each interrupt in turn rather than jumping to the end
of the interval and firing afterwards - a handler that reads the clock has to see the
instant it was due. Re-entry is guarded there: a handler that advances time gets the
time, but not nested interrupts.

The two environments deliberately share fixtures: `Marlin/tests/support/` selects the
mechanism per platform, so a test reads the same either way and the LINUX build stays
green while this one is finished.

## Rules

1. **Never read a real clock here.** Anything that consults wall time defeats the point.
2. **Never install a signal handler.** If something appears to need one, it wants a
   timer that a test advances instead.
3. **Interrupts fire only from `HAL_test_advance_*`.** Calling an ISR directly is a
   LINUX-HAL habit; here it steps the machine while the clock stands still.
4. Keep the divergence from `HAL/LINUX` to `Clock`, `Timer` and `timers`. Everything
   else should stay copy-identical so fixes to one apply to the other.
