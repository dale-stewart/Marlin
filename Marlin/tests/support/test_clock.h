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
 * Let time pass, whichever HAL the tests are built against.
 *
 * Under the test HAL time is a counter and advancing it is exact. Under the LINUX HAL
 * time is the wall clock, and the nearest equivalent is to accelerate it and sleep for
 * a scaled-down interval — approximate, and the reason the test HAL exists. Tests use
 * this rather than either mechanism directly, so they read the same in both builds.
 */

#include "src/inc/MarlinConfig.h"

#ifdef __PLAT_TEST__

  #include "src/HAL/TEST/timers.h"

  // Declared in a test as `TestClock clock;` — under this HAL there is nothing to set
  // up, because advancing time is already exact.
  struct TestClock {
    static void advance_seconds(const uint32_t s) { HAL_test_advance_millis(s * 1000); }
    static void advance_millis(const uint32_t ms) { HAL_test_advance_millis(ms); }
  };

#else

  #include "src/HAL/LINUX/hardware/Clock.h"

  /**
   * Accelerates the wall clock for as long as it is in scope.
   *
   * The acceleration has to cover the whole test, not just the sleeps: this HAL reports
   * elapsed time as (now - startup) * multiplier, so putting the multiplier back while
   * a stopwatch is still running would shrink the time it has already measured.
   */
  struct TestClock {
    static constexpr double ACCELERATION = 1000.0;
    TestClock() { Clock::setTimeMultiplier(ACCELERATION); }
    ~TestClock() { Clock::setTimeMultiplier(1.0); }
    static void advance_millis(const uint32_t ms) { Clock::delayMillis(ms); }
    static void advance_seconds(const uint32_t s) { advance_millis(s * 1000); }
  };

#endif
