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

Compiles, and runs every test that does not move the machine. Motion is **not
finished**, and the remaining problem is now narrower than it was.

The first diagnosis was wrong. `HAL_timer_interrupt_enabled` read false after
`stepper.init()` not because the HAL failed to arm the timer, but because the test
fixture disabled it four lines later — leftover code whose purpose under the LINUX HAL
was suppressing POSIX signals, and which has no business running here. That is fixed:
the suppression is now `#ifndef __PLAT_TEST__`.

What is left: with the step interrupt actually enabled, the suite hangs *before its
first assertion*, somewhere in the `SimulatedMachine` constructor — earlier than any
test body, and with no output to narrow it. Enabling interrupts is necessary for motion
here, so this has to be understood rather than worked around.

Worth checking first: whether an ISR fires re-entrantly through `HAL_test_advance_*`,
and whether `Stepper::isr()` setting the compare to `HAL_TIMER_TYPE_MAX` on entry
combines with `Timer::schedule()` to produce a zero or wrapped interval — a period of
zero would fire the handler forever inside one advance, which matches a hang with no
output. The `budget` argument in `Timer::advance()` was meant to bound exactly that;
it may need to bound re-entry too.

Until then the motion tests run under `linux_native_test`, which drives `stepper.isr()`
directly and stays green at 377.

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
