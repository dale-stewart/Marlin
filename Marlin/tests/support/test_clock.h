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
 * Let time pass.
 *
 * Time here is a counter, so advancing it is exact and costs nothing. That is the whole
 * reason this HAL exists, and it is why a test can say "a second goes by" and mean it.
 *
 * This used to have a second implementation for HAL/LINUX, where time is the wall clock:
 * it accelerated the clock by a thousand and slept for a scaled-down interval, which was
 * approximate, slow, and could not be used to wait for anything the firmware had to do
 * first. Unit tests no longer build against that HAL, so the approximation is gone and
 * `advance_millis()` means exactly what it says.
 */

#include "src/inc/MarlinConfig.h"
#include "src/HAL/TEST/timers.h"

// Declared in a test as `TestClock clock;` — there is nothing to set up, because
// advancing time is already exact.
struct TestClock {
  static void advance_seconds(const uint32_t s) { HAL_test_advance_millis(s * 1000); }
  static void advance_millis(const uint32_t ms) { HAL_test_advance_millis(ms); }
};
