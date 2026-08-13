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
 * What M109 and M190 tell the host while they wait.
 *
 * A host with a progress bar draws it from the `W:` field of the once-a-second report:
 * `?` while the heater is still on its way, then the number of seconds of residency
 * still to serve. So `W:` is a computed value with a contract, not a debug print — and
 * it is the only place the residency countdown is visible from outside, since the timer
 * it is derived from is a local of the wait loop.
 *
 * Asserting that a report appeared says nothing about that arithmetic. Asserting the
 * whole sequence pins it: the countdown starts one second in, falls by exactly one per
 * report, and reaches zero on the report before the command returns.
 *
 * Test-HAL only: these need a heater that responds to the firmware and a clock the test
 * advances.
 */


#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_heaters.h"
#include "../support/reported_values.h"
#include "../gcode/simulated_sensors.h"
#include "../gcode/serial_capture.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/temperature.h"
#include <string.h>

// The give-up thresholds are defaulted inside temperature.cpp rather than in the
// configuration, so a test that asserts against them has to default them the same way.
#ifndef MIN_COOLING_SLOPE_DEG
  #define MIN_COOLING_SLOPE_DEG 1.50
#endif
#ifndef MIN_COOLING_SLOPE_TIME
  #define MIN_COOLING_SLOPE_TIME 60
#endif
#ifndef MIN_COOLING_SLOPE_DEG_BED
  #define MIN_COOLING_SLOPE_DEG_BED 1.00
#endif
#ifndef MIN_COOLING_SLOPE_TIME_BED
  #define MIN_COOLING_SLOPE_TIME_BED 60
#endif

namespace {

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  void time_passes_ms(const uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) { HAL_test_advance_millis(1); thermalManager.task(); }
  }

  /**
   * The `W:` field of every report in a capture, in order.
   *
   * `?` is recorded as UNKNOWN so that "not there yet" and "zero seconds left" stay
   * distinguishable — the difference between them is the whole point of the field.
   */
  constexpr long UNKNOWN = -1;

  std::vector<long> w_fields(const std::string &text) {
    std::vector<long> seen;
    size_t from = 0;
    for (;;) {
      const size_t at = text.find(" W:", from);
      if (at == std::string::npos) return seen;
      from = at + 3;
      seen.push_back(text[from] == '?' ? UNKNOWN : strtol(text.c_str() + from, nullptr, 10));
    }
  }

  // The countdown part: everything from the first reported number onwards.
  std::vector<long> countdown_of(const std::vector<long> &w) {
    std::vector<long> nums;
    bool started = false;
    for (size_t i = 0; i < w.size(); i++) {
      if (w[i] != UNKNOWN) started = true;
      if (started) nums.push_back(w[i]);
    }
    return nums;
  }

}

//
// ---- M109 ----
//

/**
 * The seconds M109 says are left count down from the residency time to zero.
 *
 * The hotend starts outside the window, so the wait reports `?` until it arrives; the
 * residency timer then starts, and the next report — one second later — is the first
 * number. That is why the countdown opens at one less than TEMP_RESIDENCY_TIME rather
 * than at it. From there each report is one second later and one second smaller, and
 * the command returns on the report that reaches zero.
 */
MARLIN_TEST(heater_wait_reports, M109_counts_the_residency_seconds_down_to_zero) {
  SimulatedMachine machine;
  SimulatedHotend hotend;

  hotend.starts_at(197.0f);                  // short of the target by more than TEMP_WINDOW
  time_passes_ms(400);
  TEST_ASSERT_TRUE(ABS(200.0f - thermalManager.degHotend(0)) > float(TEMP_WINDOW));

  std::string text;
  {
    SerialCapture capture;
    host_sends("M109 S200");
    text = capture.finish();
  }
  thermalManager.setTargetHotend(0, 0);

  const std::vector<long> w = w_fields(text), counting = countdown_of(w);

  TEST_ASSERT_TRUE(w.size() > counting.size());        // it reported `?` first
  TEST_ASSERT_EQUAL(UNKNOWN, w[0]);
  TEST_ASSERT_EQUAL(size_t(TEMP_RESIDENCY_TIME), counting.size());

  for (size_t i = 0; i < counting.size(); i++)
    TEST_ASSERT_EQUAL(long(TEMP_RESIDENCY_TIME) - 1 - long(i), counting[i]);
}

/**
 * The report is once a second, so the countdown is also the elapsed time.
 *
 * Both come from the same `now`, and the loop is only reached because simulated time
 * advanced, so the two have to agree: the whole residency takes as long as the field
 * said was left when it started.
 */
MARLIN_TEST(heater_wait_reports, M109_reports_once_a_second_while_it_waits) {
  SimulatedMachine machine;
  SimulatedHotend hotend;

  hotend.starts_at(197.0f);
  time_passes_ms(400);

  const millis_t started = millis();
  std::string text;
  {
    SerialCapture capture;
    host_sends("M109 S200");
    text = capture.finish();
  }
  const millis_t took = millis() - started;
  thermalManager.setTargetHotend(0, 0);

  const std::vector<long> w = w_fields(text);

  // One report per second of the whole wait, give or take the report that ends it.
  TEST_ASSERT_TRUE(w.size() >= took / 1000UL);
  TEST_ASSERT_TRUE(w.size() <= took / 1000UL + 2);
  TEST_ASSERT_TRUE(took >= SEC_TO_MS(TEMP_RESIDENCY_TIME));
}

/**
 * A hotend that will not cool is given up on rather than waited for indefinitely.
 *
 * `M109 R` waits for a temperature to be reached from either direction, so a printer
 * with no part-cooling fan could sit at an R below its current reading forever. The
 * loop therefore samples the drop every MIN_COOLING_SLOPE_TIME and abandons the wait
 * if the hotend did not fall MIN_COOLING_SLOPE_DEG over that interval. The sensor here
 * reads a constant temperature, which is the extreme of that case: the first interval
 * ends with no drop at all.
 */
MARLIN_TEST(heater_wait_reports, M109_R_gives_up_on_a_hotend_that_is_not_cooling) {
  SimulatedMachine machine;
  SimulatedSensors sensors;                  // a fixed reading: nothing cools

  const celsius_float_t stuck_at = SimulatedSensors::hotend_reads(200.0f);
  TEST_ASSERT_TRUE(stuck_at > 190.0f);

  const millis_t started = millis();
  host_sends("M109 R150");
  const millis_t took = millis() - started;
  thermalManager.setTargetHotend(0, 0);

  // It waited one slope interval and then stopped, still nowhere near the target.
  TEST_ASSERT_TRUE(took >= SEC_TO_MS(MIN_COOLING_SLOPE_TIME));
  TEST_ASSERT_TRUE(took < SEC_TO_MS(2 * MIN_COOLING_SLOPE_TIME));
  TEST_ASSERT_TRUE(thermalManager.degHotend(0) > 190.0f);
}

#if HAS_HEATED_BED

  //
  // ---- M190 ----
  //

  MARLIN_TEST(heater_wait_reports, M190_counts_the_residency_seconds_down_to_zero) {
    SimulatedMachine machine;
    SimulatedBed bed;

    bed.starts_at(50.0f);
    time_passes_ms(400);
    TEST_ASSERT_TRUE(thermalManager.degBed() < 60.0f - float(TEMP_BED_WINDOW));

    std::string text;
    {
      SerialCapture capture;
      host_sends("M190 S60");
      text = capture.finish();
    }
    thermalManager.setTargetBed(0);

    const std::vector<long> w = w_fields(text), counting = countdown_of(w);

    TEST_ASSERT_TRUE(w.size() > counting.size());
    TEST_ASSERT_EQUAL(UNKNOWN, w[0]);
    TEST_ASSERT_EQUAL(size_t(TEMP_BED_RESIDENCY_TIME), counting.size());

    for (size_t i = 0; i < counting.size(); i++)
      TEST_ASSERT_EQUAL(long(TEMP_BED_RESIDENCY_TIME) - 1 - long(i), counting[i]);
  }

  MARLIN_TEST(heater_wait_reports, M190_R_gives_up_on_a_bed_that_is_not_cooling) {
    SimulatedMachine machine;
    SimulatedSensors sensors;

    SimulatedSensors::bed_reads(80.0f);
    TEST_ASSERT_TRUE(thermalManager.degBed() > 70.0f);

    const millis_t started = millis();
    host_sends("M190 R40");
    const millis_t took = millis() - started;
    thermalManager.setTargetBed(0);

    TEST_ASSERT_TRUE(took >= SEC_TO_MS(MIN_COOLING_SLOPE_TIME_BED));
    TEST_ASSERT_TRUE(took < SEC_TO_MS(2 * MIN_COOLING_SLOPE_TIME_BED));
    TEST_ASSERT_TRUE(thermalManager.degBed() > 70.0f);
  }

#endif // HAS_HEATED_BED

