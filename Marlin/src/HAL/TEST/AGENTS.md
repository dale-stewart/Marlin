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

**Commands that wait do not yet work**, and the reason is worth stating precisely
because it was mistaken twice. `planner.synchronize()` and `dwell()` both spin on
`marlin.idle()`, and *nothing inside `idle()` advances a clock that only moves when
asked*. So `M400`, `G4` with a non-zero pause, and arcs — which overrun the block buffer
and wait for space — never finish here. The motion tests work because `run_until_idle()`
advances time from the test, outside any command.

This is one level up from the bug that made motion work. `Timer::getCount()` had to
charge a tick per read because Marlin spins on the timer count and, on hardware, the
polling itself costs time. `millis()` has exactly the same problem in `dwell()` and the
same likely fix — but `millis()` is read all over the firmware, and making every read
advance time would change every timeout in the suite. That wants measuring, not
assuming, and it is the next piece of work.

Until then, blocking commands are tested nowhere: they cannot run under `HAL/LINUX`
either.

## Rules

1. **Never read a real clock here.** Anything that consults wall time defeats the point.
2. **Never install a signal handler.** If something appears to need one, it wants a
   timer that a test advances instead.
3. **Interrupts fire only from `HAL_test_advance_*`.** Calling an ISR directly is a
   LINUX-HAL habit; here it steps the machine while the clock stands still.
4. Keep the divergence from `HAL/LINUX` to `Clock`, `Timer` and `timers`. Everything
   else should stay copy-identical so fixes to one apply to the other.
