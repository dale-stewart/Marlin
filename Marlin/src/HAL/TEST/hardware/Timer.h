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
  }

  void start(const uint32_t freq) { setCompare(frequency / (freq ? freq : 1)); }

  void enable()  { active = true; schedule(); }
  void disable() { active = false; }
  bool enabled() const { return active; }

  void setCompare(const uint32_t c) { compare = c; schedule(); }
  uint32_t getCompare() const { return compare; }
  uint32_t getCount() const { return uint32_t(Clock::ticks(frequency)); }
  uint32_t getOverruns() const { return 0; }
  uint32_t getAvgError() const { return 0; }

  /**
   * Run any interrupts that fall inside the time just elapsed.
   *
   * `budget` bounds how many can fire in one advance, so a compare value of zero — or
   * a test advancing a long way — cannot spin forever. Hitting the budget is not an
   * error: the remaining interrupts fire on the next advance.
   */
  void advance(const uint64_t upto_ns, const uint32_t budget = 100000) {
    if (!active || !cbfn || !compare) return;
    const uint64_t period = Clock::ticksToNanos(compare, frequency);
    if (!period) return;
    for (uint32_t fired = 0; fired < budget && next_fire_ns <= upto_ns; fired++) {
      next_fire_ns += period;
      cbfn();
    }
  }

private:
  void schedule() { next_fire_ns = Clock::nanos() + Clock::ticksToNanos(compare, frequency); }

  bool active = false;
  uint32_t compare = 0;
  uint32_t frequency = 1;
  uint64_t next_fire_ns = 0;
  callback_fn *cbfn = nullptr;
};
