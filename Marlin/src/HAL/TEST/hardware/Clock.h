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
 * A clock that only moves when a test says so.
 *
 * The LINUX HAL reads a real clock because it is meant to run Marlin in real time. A
 * test wants the opposite: code that waits — `dwell()`, a temperature residency check,
 * a homing timeout — should finish instantly and identically on every run, which it can
 * only do if the test owns the passage of time.
 *
 * So nothing here reads the wall clock. Time advances when `advance()` is called, and
 * that is also when interrupts fire (see Timer.h). A test that never advances time sees
 * a machine frozen mid-instruction, which is exactly what makes assertions repeatable.
 */

// Pulled in early on purpose: several standard headers reach <chrono>, and Marlin's
// macros (abs, min, max) break it if they are defined first.
#include <chrono>
#include <stdint.h>

class Clock {
public:
  static uint32_t frequency;      // ticks per second of the simulated CPU

  // Move time forward. This is the only way time passes.
  static void advance_nanos(const uint64_t ns) { now_ns += ns; }
  static void advance_micros(const uint64_t us) { advance_nanos(us * 1000ULL); }
  static void advance_millis(const uint64_t ms) { advance_micros(ms * 1000ULL); }

  static void reset() { now_ns = 0; }

  static uint64_t nanos()   { return now_ns; }
  static uint64_t micros()  { return now_ns / 1000ULL; }
  static uint64_t millis()  { return now_ns / 1000000ULL; }
  static double   seconds() { return double(now_ns) / 1000000000.0; }

  static uint64_t ticks(const uint32_t freq = Clock::frequency) {
    return nanos() / (1000000000ULL / (freq ? freq : 1));
  }
  static uint64_t nanosToTicks(const uint64_t ns, const uint32_t freq = Clock::frequency) {
    return ns / (1000000000ULL / (freq ? freq : 1));
  }
  static uint64_t ticksToNanos(const uint64_t tick, const uint32_t freq = Clock::frequency) {
    return tick * (1000000000ULL / (freq ? freq : 1));
  }

  // Delays move the clock rather than blocking: a test must never actually wait.
  static void delayCycles(const uint64_t cycles) { advance_nanos((1000000000ULL / (frequency ? frequency : 1)) * cycles); }
  static void delayMicros(const uint64_t us)     { advance_micros(us); }
  static void delayMillis(const uint64_t ms)     { advance_millis(ms); }
  static void delaySeconds(const double s)       { advance_nanos(uint64_t(s * 1000000000.0)); }

  static void setFrequency(const uint32_t freq) { frequency = freq; }

private:
  static uint64_t now_ns;
};
