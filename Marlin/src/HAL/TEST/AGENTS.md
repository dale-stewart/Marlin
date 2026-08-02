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

Compiles and runs the suite. Motion under this HAL is **not finished**: the stepper
timer is not being armed as expected (`HAL_timer_interrupt_enabled` reads false after
`stepper.init()`, and the compare value is not what `HAL_timer_start` was given), so a
queued move does not step when time advances. Until that is resolved the motion tests
run under `linux_native_test`, which drives `stepper.isr()` directly.

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
