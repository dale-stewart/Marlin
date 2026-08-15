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
 * Which axis moved first.
 *
 * Some of what this firmware does is not a matter of where the tool ends up but of the order
 * it got there in. Raising Z before crossing the bed and lowering it only after arriving is
 * the difference between clearing a printed part and ploughing through it, and both orderings
 * finish at exactly the same coordinates — so no assertion about the final position can tell
 * them apart. The evidence has to be the sequence.
 *
 * Every axis is one pin, and each rising edge on it is one step, so recording the first and
 * last edge per axis is enough: "Z had finished before X began" is `z.last < x.first`. That is
 * a claim about the shape of the whole sequence and not about any one timestamp, which is what
 * makes it stable — the durations depend on feedrates and acceleration, the ordering does not.
 *
 * `IOLogger` is a single global hook, so only one of these (or one `StepTimeline`) can exist at
 * a time; it attaches on construction and detaches on destruction for the reason given in
 * `test_step_timing.cpp` — a dangling logger writes through a destroyed object.
 */

#include "src/inc/MarlinConfig.h"


#include "src/HAL/TEST/hardware/Gpio.h"

class StepOrder : public IOLogger {
public:

  StepOrder() { Gpio::attachLogger(this); }
  ~StepOrder() { Gpio::attachLogger(nullptr); }

  void log(GpioEvent ev) override {
    if (ev.event != GpioEvent::RISE) return;
    switch (ev.pin_id) {
      #if HAS_X_AXIS
        case X_STEP_PIN: note(x, ev.timestamp); break;
      #endif
      #if HAS_Y_AXIS
        case Y_STEP_PIN: note(y, ev.timestamp); break;
      #endif
      #if HAS_Z_AXIS
        case Z_STEP_PIN: note(z, ev.timestamp); break;
      #endif
      #if HAS_EXTRUDERS
        case E0_STEP_PIN: note(e, ev.timestamp); break;
      #endif
      default: break;
    }
  }

  // When an axis started and stopped stepping, and how much it did. Timestamps are the
  // simulated clock's nanoseconds; `steps` is there so a test can say "this axis moved at all"
  // without having to reason about whether zero is a valid timestamp.
  struct Span {
    uint64_t first = 0, last = 0;
    size_t steps = 0;
    bool moved() const { return steps > 0; }
  };

  /**
   * The extruder is counted here for a reason the position axes do not have: code that moves
   * filament often **re-bases the E counter afterwards**, so `stepper.position(E_AXIS)` reads the
   * same before and after and reports that nothing happened. Pulses on the pin are not re-based
   * by anything, which makes this the only instrument that can measure a retract or a purge that
   * ends in a `sync`.
   */
  Span x, y, z;
  #if HAS_EXTRUDERS
    Span e;
  #endif

  // Start again from here, for a test interested in one phase of a longer sequence.
  void forget() { x = Span(); y = Span(); z = Span(); TERN_(HAS_EXTRUDERS, e = Span()); }

  // Did `a` finish everything it was going to do before `b` started?
  //
  // An axis that never moved cannot come before or after anything, so this says so rather
  // than silently reporting true off a pair of zeroes — which is how an assertion about
  // ordering ends up passing against a machine that stood still.
  static bool finished_before(const Span &a, const Span &b) {
    return a.moved() && b.moved() && a.last < b.first;
  }

private:

  static void note(Span &s, const uint64_t at) {
    if (!s.steps) s.first = at;
    s.last = at;
    s.steps++;
  }
};

