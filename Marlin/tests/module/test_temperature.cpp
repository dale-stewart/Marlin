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

/**
 * Tests for the temperature manager.
 *
 * This is the module that decides how hot things get, so its limits matter more than
 * most: a target that is not clamped, or a cold-extrusion check that passes when it
 * should not, is a safety question rather than a correctness one.
 *
 * The sensor readings are substituted (see gcode/simulated_sensors.h) because nothing
 * advances a heater in this build.
 */

#include "../test/unit_tests.h"
#include "src/module/temperature.h"
#include "../gcode/simulated_sensors.h"

namespace {

  struct SavedTargets {
    celsius_t hotend, bed;
    bool cold_extrude;
    celsius_t extrude_min;
    SavedTargets() {
      hotend = thermalManager.degTargetHotend(0);
      bed = TERN0(HAS_HEATED_BED, thermalManager.degTargetBed());
      cold_extrude = TERN0(PREVENT_COLD_EXTRUSION, thermalManager.allow_cold_extrude);
      extrude_min = TERN0(PREVENT_COLD_EXTRUSION, thermalManager.extrude_min_temp);
    }
    ~SavedTargets() {
      thermalManager.setTargetHotend(hotend, 0);
      TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(bed));
      #if ENABLED(PREVENT_COLD_EXTRUSION)
        thermalManager.allow_cold_extrude = cold_extrude;
        thermalManager.extrude_min_temp = extrude_min;
      #endif
    }
  };

}

MARLIN_TEST(temperature, a_target_can_be_set_and_read_back) {
  SavedTargets restore;

  thermalManager.setTargetHotend(200, 0);
  TEST_ASSERT_EQUAL(200, thermalManager.degTargetHotend(0));

  thermalManager.setTargetHotend(0, 0);
  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));
}

// A target above what the hardware is configured to allow is capped, not obeyed.
MARLIN_TEST(temperature, a_target_above_the_maximum_is_capped) {
  SavedTargets restore;

  thermalManager.setTargetHotend(9999, 0);
  TEST_ASSERT_TRUE(thermalManager.degTargetHotend(0) < 9999);
  TEST_ASSERT_TRUE(thermalManager.degTargetHotend(0) <= thermalManager.hotend_max_target(0));
}

MARLIN_TEST(temperature, heating_and_cooling_are_told_apart) {
  SavedTargets restore;
  SimulatedSensors sensors;

  SimulatedSensors::hotend_reads(20.0f);
  thermalManager.setTargetHotend(200, 0);
  TEST_ASSERT_TRUE(thermalManager.isHeatingHotend(0));
  TEST_ASSERT_FALSE(thermalManager.isCoolingHotend(0));

  SimulatedSensors::hotend_reads(220.0f);
  thermalManager.setTargetHotend(180, 0);
  TEST_ASSERT_TRUE(thermalManager.isCoolingHotend(0));
  TEST_ASSERT_FALSE(thermalManager.isHeatingHotend(0));
}

MARLIN_TEST(temperature, the_measured_temperature_is_reported) {
  SimulatedSensors sensors;

  SimulatedSensors::hotend_reads(123.5f);
  TEST_ASSERT_EQUAL_FLOAT(123.5f, thermalManager.degHotend(0));
  TEST_ASSERT_EQUAL(124, thermalManager.wholeDegHotend(0));   // rounded, not truncated
}

#if ENABLED(PREVENT_COLD_EXTRUSION)

  /**
   * Cold extrusion is refused because pushing filament through an unheated nozzle
   * strips the drive gear and jams the hotend.
   */
  MARLIN_TEST(temperature, extruding_while_cold_is_refused) {
    SavedTargets restore;
    SimulatedSensors sensors;

    thermalManager.allow_cold_extrude = false;
    thermalManager.extrude_min_temp = 170;

    SimulatedSensors::hotend_reads(20.0f);
    TEST_ASSERT_TRUE(thermalManager.tooColdToExtrude(0));

    SimulatedSensors::hotend_reads(180.0f);
    TEST_ASSERT_FALSE(thermalManager.tooColdToExtrude(0));
  }

  MARLIN_TEST(temperature, the_cold_extrusion_limit_can_be_lifted) {
    SavedTargets restore;
    SimulatedSensors sensors;

    thermalManager.extrude_min_temp = 170;
    SimulatedSensors::hotend_reads(20.0f);

    thermalManager.allow_cold_extrude = true;
    TEST_ASSERT_FALSE(thermalManager.tooColdToExtrude(0));

    thermalManager.allow_cold_extrude = false;
    TEST_ASSERT_TRUE(thermalManager.tooColdToExtrude(0));
  }

  /**
   * The limit has a tolerance below it.
   *
   * The check is `temp < extrude_min_temp - TEMP_WINDOW`, so a nozzle sitting a degree
   * or two under its target still extrudes rather than refusing mid-print every time
   * the reading dips. Pinned as a relationship rather than a pair of numbers, so the
   * test follows the configured window instead of contradicting it.
   */
  MARLIN_TEST(temperature, the_cold_extrusion_limit_has_a_tolerance) {
    SavedTargets restore;
    SimulatedSensors sensors;

    thermalManager.allow_cold_extrude = false;
    thermalManager.extrude_min_temp = 170;

    SimulatedSensors::hotend_reads(170.0f);
    TEST_ASSERT_FALSE(thermalManager.tooColdToExtrude(0));

    // Just inside the tolerance: still allowed.
    SimulatedSensors::hotend_reads(float(170 - TEMP_WINDOW));
    TEST_ASSERT_FALSE(thermalManager.tooColdToExtrude(0));

    // Below the tolerance: refused.
    SimulatedSensors::hotend_reads(float(170 - TEMP_WINDOW - 1));
    TEST_ASSERT_TRUE(thermalManager.tooColdToExtrude(0));
  }

#endif

#if HAS_HEATED_BED

  MARLIN_TEST(temperature, the_bed_target_can_be_set_and_read_back) {
    SavedTargets restore;

    thermalManager.setTargetBed(60);
    TEST_ASSERT_EQUAL(60, thermalManager.degTargetBed());

    thermalManager.setTargetBed(0);
    TEST_ASSERT_EQUAL(0, thermalManager.degTargetBed());
  }

  MARLIN_TEST(temperature, a_bed_target_above_the_maximum_is_capped) {
    SavedTargets restore;
    thermalManager.setTargetBed(9999);
    TEST_ASSERT_TRUE(thermalManager.degTargetBed() < 9999);
  }

#endif

#if HAS_FAN

  MARLIN_TEST(temperature, fan_speed_is_set_and_read_back) {
    const uint8_t was = thermalManager.fan_speed[0];

    thermalManager.set_fan_speed(0, 128);
    TEST_ASSERT_EQUAL(128, thermalManager.fan_speed[0]);

    thermalManager.set_fan_speed(0, 0);
    TEST_ASSERT_EQUAL(0, thermalManager.fan_speed[0]);

    thermalManager.fan_speed[0] = was;
  }

#endif
