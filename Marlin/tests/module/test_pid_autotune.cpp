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
 * What PID autotune computes, rather than that it finished.
 *
 * `M303` is a measurement: it drives the heater as a relay, watches the oscillation
 * that results, and turns the amplitude and period of that oscillation into gains by
 * the Ziegler-Nichols relations. A test that only asserts the gains came out positive
 * says nothing about any of that arithmetic — every constant in it could be wrong and
 * still produce three positive numbers.
 *
 * Almost none of the intermediate work is reachable from outside: `bias`, `d`, `Ku` and
 * `Tu` are locals. They are, however, all *reported*, because an operator needs to see
 * them to judge the tune. So these tests read the numbers back out of the report the
 * host would have received and assert the relations between them — the report is the
 * published surface here, not an implementation detail.
 *
 * The chain each test asserts one link of:
 *
 *     d      = bias, mirrored about half power once bias passes it
 *     Ku     = 4d / (pi * (maxT - minT) / 2)      relay-feedback ultimate gain
 *     Tu     = t_high + t_low                     the period of the oscillation
 *     Kp     = 0.6 Ku, Ki = 2 Kp / Tu, Kd = Kp Tu / 8     classic Ziegler-Nichols
 *
 * Test-HAL only: autotune needs a heater that responds to the pin and a clock the test
 * advances, and neither exists under HAL/LINUX.
 */

#ifdef __PLAT_TEST__

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
#include <math.h>

#if ENABLED(PIDTEMP)

namespace {

  constexpr celsius_t TUNE_TARGET = 180;

  // The relay levels autotune works in. `bias` is the mid-point of the relay and `d`
  // its half-swing, both in units of PID_MAX; the pair is kept inside these bounds so
  // that neither relay level saturates.
  constexpr long MAX_POW = PID_MAX, HALF_POW = MAX_POW >> 1,
                 BIAS_MIN = 20, BIAS_MAX = MAX_POW - 20;

  // Both halves of a relay cycle are held for at least this long before the heater is
  // switched, so no measured period can be shorter than two of them.
  constexpr float HOTEND_RELAY_DELAY_S = 3.0f;

  // Restores whatever the hotend PID was before a test that lets autotune apply its own.
  struct SavedPID {
    raw_pid_t was;
    SavedPID() : was({ thermalManager.temp_hotend[0].pid.p(),
                       thermalManager.temp_hotend[0].pid.i(),
                       thermalManager.temp_hotend[0].pid.d() }) {}
    ~SavedPID() {
      thermalManager.temp_hotend[0].pid.set(was);
      thermalManager.updatePID();
      thermalManager.setTargetHotend(0, 0);
    }
  };

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

  // Everything one autotune run reported, in the order the cycles produced it. There is
  // one bias/d/min/max group per completed cycle and one Ku/Tu pair per cycle that was
  // late enough to produce gains, so the Ku entries line up with the *last* bias
  // entries.
  struct AutotuneReport {
    std::string text;
    std::vector<double> bias, d, minT, maxT, Ku, Tu, Kp, Ki, Kd;
    millis_t took = 0;

    void read(const std::string &captured, const millis_t elapsed) {
      text = captured;
      took = elapsed;
      bias = reported::all_numbers(text, " bias: ");
      d    = reported::all_numbers(text, " d: ");
      minT = reported::all_numbers(text, " min: ");
      maxT = reported::all_numbers(text, " max: ");
      Ku   = reported::all_numbers(text, " Ku: ");
      Tu   = reported::all_numbers(text, " Tu: ");
      Kp   = reported::all_numbers(text, " Kp: ");
      Ki   = reported::all_numbers(text, " Ki: ");
      Kd   = reported::all_numbers(text, " Kd: ");
    }

    // The cycle index that produced the k'th reported gain set.
    size_t cycle_of_gains(const size_t k) const { return bias.size() - Ku.size() + k; }
  };

  // Run one autotune against a heater the caller has already placed, and read the report.
  void autotune_run(AutotuneReport &out, const char * const command) {
    const millis_t started = millis();
    SerialCapture capture;
    host_sends(command);
    out.read(capture.finish(), millis() - started);
  }

}

//
// ---- The guards, which cost nothing to reach ----
//

/**
 * Fewer than three cycles cannot produce gains, so the routine declines to start.
 *
 * The relay needs one cycle to settle the bias and a second before the amplitude is
 * meaningful; gains are only computed from the third onwards. Asked for two, autotune
 * returns before it has switched a heater on or said anything at all — which is what
 * makes this observable without a heater or a clock.
 */
MARLIN_TEST(pid_autotune, fewer_than_three_cycles_is_declined_before_anything_is_heated) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  SavedPID saved;

  std::string text;
  {
    SerialCapture capture;
    thermalManager.PID_autotune(TUNE_TARGET, 0, 2, true);
    text = capture.finish();
  }

  TEST_ASSERT_EQUAL(0u, reported::occurrences(text, STR_PID_AUTOTUNE));
  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));
  TEST_ASSERT_EQUAL_FLOAT(saved.was.p, thermalManager.temp_hotend[0].pid.p());
}

// Three is enough, and is the boundary: the same call one cycle higher does start.
MARLIN_TEST(pid_autotune, three_cycles_is_accepted) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  TEST_ASSERT_TRUE(reported::saw(report.text, STR_PID_AUTOTUNE STR_PID_AUTOTUNE_START));
  TEST_ASSERT_TRUE(reported::saw(report.text, STR_PID_AUTOTUNE_FINISHED));
}

/**
 * A target the hotend is not allowed to reach is refused before the relay starts.
 *
 * `hotend_max_target()` is MAXTEMP less the overshoot allowance, and autotune has to
 * overshoot to measure anything, so a target at or above it is rejected outright rather
 * than clamped.
 */
MARLIN_TEST(pid_autotune, a_target_above_the_hotend_maximum_is_refused) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  SavedPID saved;

  char cmd[32];
  snprintf(cmd, sizeof(cmd), "M303 E0 C3 S%d", int(Temperature::hotend_max_target(0)) + 1);

  std::string text;
  {
    SerialCapture capture;
    host_sends(cmd);
    text = capture.finish();
  }

  TEST_ASSERT_TRUE(reported::saw(text, STR_PID_TEMP_TOO_HIGH));
  TEST_ASSERT_FALSE(reported::saw(text, STR_PID_AUTOTUNE STR_PID_AUTOTUNE_START));
  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));
}

//
// ---- What a completed tune measured ----
//

/**
 * One relay cycle is reported per cycle, and gains only from the third.
 *
 * The first cycle has no previous half-period to compare against, so no bias is
 * adjusted and nothing is reported for it; the second adjusts the bias but the
 * amplitude is not yet trustworthy. Three cycles therefore report three relay states
 * and exactly one set of gains — the count is the visible form of the "not before the
 * third" rule.
 */
MARLIN_TEST(pid_autotune, gains_are_measured_only_from_the_third_cycle) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  TEST_ASSERT_EQUAL(3u, report.bias.size());
  TEST_ASSERT_EQUAL(1u, report.Ku.size());
  TEST_ASSERT_EQUAL(1u, report.Tu.size());
  TEST_ASSERT_EQUAL(1u, report.Kp.size());
}

/**
 * The relay's two levels stay a mirrored pair inside the power limits.
 *
 * `d` is the half-swing of the relay and is held equal to `bias` while the bias is in
 * the lower half of the power range, and mirrored about full power once it passes the
 * half-way point — which is what keeps the upper relay level from saturating. The bias
 * itself is clamped so neither level reaches an end of the range.
 */
MARLIN_TEST(pid_autotune, the_relay_levels_reported_are_a_mirrored_pair_within_the_power_limits) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  TEST_ASSERT_TRUE(report.bias.size() > 0);
  TEST_ASSERT_EQUAL(report.bias.size(), report.d.size());

  for (size_t i = 0; i < report.bias.size(); i++) {
    const long bias = long(report.bias[i]), d = long(report.d[i]);
    TEST_ASSERT_TRUE(bias >= BIAS_MIN && bias <= BIAS_MAX);
    TEST_ASSERT_EQUAL(bias > HALF_POW ? MAX_POW - 1 - bias : bias, d);
    // Both relay levels are expressible, so the swing really is symmetric about bias.
    TEST_ASSERT_TRUE(bias - d >= 0);
    TEST_ASSERT_TRUE(bias + d <= MAX_POW - 1);
  }
}

/**
 * The temperature extremes reported are the extremes of a cycle straddling the target.
 *
 * The whole method depends on the heater actually crossing the target in both
 * directions: `maxT` is reset to the target when the relay switches off and `minT` when
 * it switches on, so every reported pair brackets the target. A cycle that did not
 * cross would give an amplitude that means nothing.
 */
MARLIN_TEST(pid_autotune, every_reported_cycle_straddles_the_target) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  TEST_ASSERT_EQUAL(report.bias.size(), report.minT.size());
  TEST_ASSERT_EQUAL(report.bias.size(), report.maxT.size());
  for (size_t i = 0; i < report.minT.size(); i++) {
    TEST_ASSERT_TRUE(report.minT[i] < double(TUNE_TARGET));
    TEST_ASSERT_TRUE(report.maxT[i] > double(TUNE_TARGET));
  }
}

/**
 * Ku is the relay-feedback ultimate gain of the swing that was reported alongside it.
 *
 * For a relay of half-amplitude `d` producing a peak-to-peak temperature swing `a`, the
 * describing-function estimate is Ku = 4d / (pi * a / 2). Every term of that is in the
 * report, so the identity can be checked directly — and each reported cycle is a
 * separate check of it.
 */
MARLIN_TEST(pid_autotune, ku_is_the_relay_gain_of_the_swing_it_reported) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  TEST_ASSERT_TRUE(report.Ku.size() > 0);
  for (size_t k = 0; k < report.Ku.size(); k++) {
    const size_t c = report.cycle_of_gains(k);
    const double swing = report.maxT[c] - report.minT[c],
                 expected = (4.0 * report.d[c]) / (M_PI * swing * 0.5);
    TEST_ASSERT_TRUE(swing > 0.0);
    // The report carries two decimals, which is what bounds how closely this can be
    // checked: the swing is a difference of two of them, and Ku is inversely
    // proportional to it, so a narrow swing widens the tolerance in proportion.
    TEST_ASSERT_FLOAT_WITHIN(float(expected * 0.01 / swing + 0.01), float(expected), float(report.Ku[k]));
  }
}

/**
 * Tu is the period of the oscillation, which cannot be shorter than the two relay holds.
 *
 * Each half of the cycle is held for at least the relay delay before the heater is
 * switched again, so the measured period has a hard floor of two of them — and it is a
 * single cycle, so it cannot exceed the whole command. Those two bounds are what pin
 * the units: a period read in the wrong scale falls outside both.
 */
MARLIN_TEST(pid_autotune, the_measured_period_is_a_single_cycle_of_at_least_two_relay_holds) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  TEST_ASSERT_TRUE(report.Tu.size() > 0);
  for (size_t k = 0; k < report.Tu.size(); k++) {
    TEST_ASSERT_TRUE(report.Tu[k] >= 2.0 * HOTEND_RELAY_DELAY_S);
    TEST_ASSERT_TRUE(report.Tu[k] <= double(report.took) / 1000.0);
  }
}

/**
 * The gains applied are the classic Ziegler-Nichols relations of the Ku and Tu measured.
 *
 * `M303 U1` writes the result into the live PID, so the applied values can be read back
 * exactly rather than at the two decimals of the report. Each of the three has its own
 * dependence on the measurement — Kp on Ku alone, Ki inversely on the period, Kd
 * proportionally — so asserting all three separates constants that a single check would
 * conflate.
 */
MARLIN_TEST(pid_autotune, the_applied_gains_are_the_classic_pid_relations_of_ku_and_tu) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3 U1");

  TEST_ASSERT_TRUE(report.Ku.size() > 0);
  const double Ku = report.Ku.back(), Tu = report.Tu.back();

  const float p = thermalManager.temp_hotend[0].pid.p(),
              i = thermalManager.temp_hotend[0].pid.i(),
              d = thermalManager.temp_hotend[0].pid.d();

  // Against the measurement, to the precision the report was given in.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, float(0.6 * Ku), p);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, float(2.0 * (0.6 * Ku) / Tu), i);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, float((0.6 * Ku) * Tu / 8.0), d);

  /**
   * And between themselves, with the period eliminated.
   *
   * Ki is 2Kp/Tu and Kd is Kp*Tu/8, so their product is Kp^2/4 whatever the period was
   * — an identity that holds at full precision rather than at the two decimals Tu was
   * reported to, and one that no single pairing of the three could stand in for.
   */
  TEST_ASSERT_FLOAT_WITHIN(0.01f, p * p / 4.0f, i * d);

  // The same numbers are what the report told the operator to put in Configuration.h.
  TEST_ASSERT_FLOAT_WITHIN(0.005f, float(report.Kp.back()), p);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, float(report.Ki.back()), i);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, float(report.Kd.back()), d);
}

/**
 * Without U the gains are reported and not applied.
 *
 * M303 is a measurement first; applying the result is opt-in, because a tune that came
 * out badly should not silently replace a working PID.
 */
MARLIN_TEST(pid_autotune, the_measured_gains_are_not_applied_unless_asked_for) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  TEST_ASSERT_TRUE(report.Kp.size() > 0);
  TEST_ASSERT_TRUE(report.Kp.back() > 0.0);           // it did measure something
  TEST_ASSERT_EQUAL_FLOAT(saved.was.p, thermalManager.temp_hotend[0].pid.p());
  TEST_ASSERT_EQUAL_FLOAT(saved.was.i, thermalManager.temp_hotend[0].pid.i());
  TEST_ASSERT_EQUAL_FLOAT(saved.was.d, thermalManager.temp_hotend[0].pid.d());
}

/**
 * A heater that needs most of its power drives the bias up into its limit.
 *
 * The bias moves towards whichever half of the cycle is taking longer, so a heater
 * whose full power is barely above the target spends longer heating than cooling and
 * the bias climbs. That is the only way to reach two things the tests above cannot: the
 * mirrored branch of `d`, which only applies once the bias passes half power, and the
 * clamp that stops the bias reaching the top of the range.
 *
 * This is a heater of its own rather than a change to `SimulatedHotend`, whose 400 C at
 * full power is what the rest of the temperature tests are written against. The one
 * here reaches 200 C — 20 C over the target — so heating is the slow half.
 */
MARLIN_TEST(pid_autotune, a_barely_adequate_heater_pushes_the_bias_to_its_upper_limit) {
  SimulatedMachine machine;
  SimulatedHeater barely(HEATER_0_PIN, TEMP_0_PIN, 200.0f, 20.0f);
  SavedPID saved;

  barely.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  {
    const millis_t started = millis();
    SerialCapture capture;
    thermalManager.PID_autotune(TUNE_TARGET, 0, 6, false);
    report.read(capture.finish(), millis() - started);
  }

  TEST_ASSERT_TRUE(report.bias.size() > 0);

  bool saw_above_half = false, saw_the_limit = false;
  for (size_t i = 0; i < report.bias.size(); i++) {
    const long bias = long(report.bias[i]), d = long(report.d[i]);
    TEST_ASSERT_TRUE(bias <= BIAS_MAX);
    if (bias > HALF_POW) {
      saw_above_half = true;
      TEST_ASSERT_EQUAL(MAX_POW - 1 - bias, d);       // mirrored, not equal to bias
      TEST_ASSERT_TRUE(d < bias);
    }
    if (bias == BIAS_MAX) saw_the_limit = true;
  }
  TEST_ASSERT_TRUE(saw_above_half);
  TEST_ASSERT_TRUE(saw_the_limit);

  // The measurement still holds together at the limit.
  for (size_t k = 0; k < report.Ku.size(); k++) {
    const size_t c = report.cycle_of_gains(k);
    const double swing = report.maxT[c] - report.minT[c],
                 expected = (4.0 * report.d[c]) / (M_PI * swing * 0.5);
    TEST_ASSERT_FLOAT_WITHIN(float(expected * 0.01 / swing + 0.01), float(expected), float(report.Ku[k]));
  }
}

#endif // PIDTEMP

#endif // __PLAT_TEST__
