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
 * A display that only remembers what it was told.
 *
 * `EXTENSIBLE_UI` is a library, not a feature: `ui_api.cpp` compiles on its own and links only
 * against a concrete UI supplying the twenty-odd `ExtUI::on*` callbacks. Every real one is a
 * hardware display, so the file was built by nothing here and tested by nothing — while writing
 * `axis_steps_per_mm`, which is one of the consumers holding that field public.
 *
 * This is the smallest client that satisfies the linker, and it records rather than discards.
 * Empty bodies would compile just as well and would make the interesting half untestable: the
 * contract of ExtUI is that the firmware *tells* the display when things happen, and a display
 * that is never told is the failure worth catching. A homing move that never reports it started,
 * a kill that never reaches the screen — those are silent, and only an observer sees them.
 *
 * `reset()` in a fixture, because a static recorder outlives the test that filled it.
 */

#include "src/inc/MarlinConfig.h"

#if ENABLED(EXTENSIBLE_UI)

#include <string>

struct RecordedUI {
  // How many times each notification arrived.
  static uint16_t startups, idles, homing_starts, homing_dones;
  static uint16_t steppers_enabled, steppers_disabled, factory_resets, postprocesses;
  static uint16_t print_started, print_paused, print_stopped;
  static uint16_t heating_errors, min_temp_errors, max_temp_errors;
  static uint16_t killed, confirms_required, axes_enabled, axes_disabled;

  // ...and the last thing said, where there was something to say.
  static std::string last_status;
  static std::string last_confirm;

  static void reset();
};

#endif // ENABLED(EXTENSIBLE_UI)
