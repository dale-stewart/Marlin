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
 * Substitute temperature sensor readings.
 *
 * Commands that wait for a temperature (M109, M190) loop until the measured value
 * reaches the target. Nothing advances a heater in the unit test build, so without a
 * stand-in those commands never return.
 *
 * No production seam was needed for this: `temp_hotend` and `temp_bed` are already
 * public, and the ADC pipeline that would normally overwrite them is dormant here —
 * it only refreshes from raw values when the temperature ISR has produced a full set
 * of samples, and that ISR does not run. So a test can simply say what the sensor
 * reads, and it stays said.
 *
 * Restores the previous readings on destruction so one test cannot leave another
 * believing the printer is hot.
 */

#include "src/module/temperature.h"

class SimulatedSensors {
public:
  SimulatedSensors() {
    #if HAS_HOTEND
      was_hotend = thermalManager.temp_hotend[0].celsius;
    #endif
    #if HAS_HEATED_BED
      was_bed = thermalManager.temp_bed.celsius;
    #endif
  }

  ~SimulatedSensors() {
    #if HAS_HOTEND
      thermalManager.temp_hotend[0].celsius = was_hotend;
    #endif
    #if HAS_HEATED_BED
      thermalManager.temp_bed.celsius = was_bed;
    #endif
  }

  // "The hotend sensor reads this."
  static void hotend_reads(const celsius_float_t c) {
    TERN_(HAS_HOTEND, thermalManager.temp_hotend[0].celsius = c);
  }

  static void bed_reads(const celsius_float_t c) {
    TERN_(HAS_HEATED_BED, thermalManager.temp_bed.celsius = c);
  }

private:
  celsius_float_t was_hotend = 0, was_bed = 0;
};
