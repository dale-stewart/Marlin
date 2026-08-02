/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2024 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
#pragma once

/**
 * A timer that is state, not a signal.
 *
 * The LINUX HAL builds its timers on POSIX interval timers, so an interrupt can arrive
 * between any two instructions — including between two assertions in a test. That is
 * not a hazard a test can be written around.
 *
 * Here a timer is a compare value and an enabled flag. Interrupts fire from
 * `Timer::advance()`, when simulated time crosses the compare value, so the machine
 * moves because time passed rather than because a signal happened to arrive. Nothing
 * runs unless a test asks for it, and the same test always produces the same result.
 */

#include <stdint.h>
#include "Clock.h"

class Timer {
public:
  typedef void (callback_fn)();

  void init(const uint32_t sim_freq, callback_fn * const fn) {
    frequency = sim_freq;
    cbfn = fn;
    compare = 0;
    active = false;
    next_fire_ns = 0;
    count_base_ns = Clock::nanos();
  }

  void start(const uint32_t freq) { setCompare(frequency / (freq ? freq : 1)); }

  /**
   * Enable is not arm.
   *
   * Hardware sets an interrupt-enable bit and leaves the counter running, so a compare
   * match that was already pending still happens when it always would have. Restarting
   * the period here instead would starve any timer that is disabled and re-enabled more
   * often than its own period — which `Stepper::endstop_triggered()` does, since its
   * ATOMIC_SECTION is a suspend/wake_up pair on the step timer and `Endstops::poll()`
   * calls it from every temperature interrupt.
   */
  void enable()  { active = true; }
  void disable() { active = false; }
  bool enabled() const { return active; }

  void setCompare(const uint32_t c) { compare = c; schedule(); }
  uint32_t getCompare() const { return compare; }
  /**
   * Ticks since the timer last restarted — and reading it costs time.
   *
   * Two things about this counter are load-bearing, and both were wrong before.
   *
   * 1. It counts from the last restart, not from the epoch. Hardware counters reset to
   *    zero on a compare match, and the LINUX HAL reproduces that by counting from the
   *    last setCompare(). An absolute count breaks Stepper::isr(), which computes
   *    `min_ticks = getCount() + margin` and compares it against an *interval*: an
   *    ever-growing count makes every interval look too short, forcing ten
   *    multistepping passes per ISR and a compare value of "now", which is not a period.
   *
   * 2. Reading it advances simulated time by one tick. Marlin times its step pulses by
   *    spinning on this counter (`AWAIT_TIMED_PULSE` in stepper.cpp), which on hardware
   *    ends because the CPU burns cycles while the counter runs. Here nothing else moves
   *    the clock inside that loop, so a counter that answered the same value every time
   *    would spin forever — and did: it hung the suite on the first move, before any
   *    interrupt was even enabled. Charging a tick per read is the simulated equivalent
   *    of the cycles the poll would have cost, and it is deterministic: the same spin
   *    always takes the same number of iterations.
   */
  uint32_t getCount() {
    Clock::advance_nanos(nanos_per_tick());
    return uint32_t(Clock::nanosToTicks(Clock::nanos() - count_base_ns, frequency));
  }
  uint32_t getOverruns() const { return 0; }
  uint32_t getAvgError() const { return 0; }

  /**
   * When this timer is next due, if it is armed at all.
   *
   * The scheduler in timers.cpp asks both timers, moves the clock to the earliest
   * answer, and fires that one — so a handler runs with the clock reading the instant it
   * was due, rather than the end of whatever interval the test asked for.
   */
  bool pending(uint64_t &when) const {
    if (!active || !cbfn) return false;
    when = next_fire_ns;
    return true;
  }

  /**
   * Run the handler, as a compare match would.
   *
   * A match resets the counter, so the count the handler reads is time since *this*
   * interrupt rather than since the last reprogramming. The default reschedule is one
   * period later; handlers that program a new compare (Stepper::isr() always does)
   * overwrite it from inside the call.
   */
  void fire() {
    count_base_ns = Clock::nanos();
    next_fire_ns = count_base_ns + period_nanos();
    cbfn();
  }

private:
  uint64_t nanos_per_tick() const { return 1000000000ULL / (frequency ? frequency : 1); }

  // A compare of zero would otherwise mean "due now, forever". One tick is the shortest
  // interval the hardware could express, so that is the floor.
  uint64_t period_nanos() const {
    const uint64_t ns = Clock::ticksToNanos(compare, frequency);
    return ns ? ns : nanos_per_tick();
  }

  void schedule() {
    count_base_ns = Clock::nanos();
    next_fire_ns = count_base_ns + period_nanos();
  }

  bool active = false;
  uint32_t compare = 0;
  uint32_t frequency = 1;
  uint64_t next_fire_ns = 0;
  uint64_t count_base_ns = 0;
  callback_fn *cbfn = nullptr;
};
