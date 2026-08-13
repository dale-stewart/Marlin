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

  // How far over the target autotune tolerates before giving up on the tune. The firmware
  // names this only where it uses it, so it is restated here rather than included.
  #ifndef MAX_OVERSHOOT_PID_AUTOTUNE
    #define MAX_OVERSHOOT_PID_AUTOTUNE 30
  #endif
  constexpr celsius_t MAX_OVERSHOOT_C = MAX_OVERSHOOT_PID_AUTOTUNE;

  // What a host driving the tune is told, as opposed to what the temperature log says.
  // The two carry the same news in different words, so a test that means one has to name
  // it exactly.
  #define HOST_NOTIFY_TOO_HIGH "Autotune failed! Temperature too high."
  #define HOST_NOTIFY_DONE     "PID tuning done"
  #define HOST_NOTIFY_HEATING  "Heating..."

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

  /**
   * The relay, watched from the heater rather than from the report.
   *
   * Everything above reads what autotune *said*. What it *did* is a square wave on the
   * heater: two power levels, alternating, held for a minimum time each. That wave is
   * where the period being measured actually comes from, so a test that never looks at
   * it can only check the report against itself — and mutants that break the switching
   * while still printing plausible numbers walk straight through.
   *
   * The heater model is already called by the ISR that drives the pin, so it is the
   * natural place to watch from: on every call it notes the power level now applied and
   * the temperature the model has reached, and keeps one entry per change of level. The
   * ISR only revisits the level once per soft-PWM period, so a timestamp here lags the
   * firmware's decision by up to that period — small against a relay hold measured in
   * seconds, and the same lag at both ends of an interval.
   */
  struct RelayChange { millis_t at; int level; float model_c; };

  class TracedHeater : public SimulatedHeater {
  public:
    using SimulatedHeater::SimulatedHeater;

    void interrupt(GpioEvent e) override { SimulatedHeater::interrupt(e); note(); }
    void update() override { SimulatedHeater::update(); note(); }

    // Start recording from whatever is applied now; the first entry is the level in
    // force when the tune began.
    void watch() { changes.clear(); watching = true; note(); }
    void stop() { watching = false; }

    std::vector<RelayChange> changes;

  private:
    void note() {
      if (!watching) return;
      const int level = int(thermalManager.temp_hotend[0].soft_pwm_amount);
      if (changes.empty() || changes.back().level != level)
        changes.push_back({ millis_t(millis()), level, temperature() });
    }
    bool watching = false;
  };

  // A traced hotend with the same model as SimulatedHotend, for tests that watch the relay.
  class TracedHotend : public TracedHeater {
  public:
    TracedHotend(const float full_power_c = 400.0f, const float tau_s = 20.0f)
      : TracedHeater(HEATER_0_PIN, TEMP_0_PIN, full_power_c, tau_s) {}
  };

  // The power level autotune applies before the first relay switch: half of full power,
  // in the units the soft-PWM comparator counts in.
  constexpr int START_LEVEL = int(PID_MAX >> 1);

  /**
   * Run a tune with the relay under observation.
   *
   * The last switch of a completed tune happens immediately before the loop's exit test,
   * so the ISR has not yet written the pin when the call returns and the trace would be
   * one entry short. The soft-PWM level is only revisited at a period boundary, so how
   * long that takes depends on where in the period the tune ended: advance the clock
   * until it has been seen, without running `Temperature::task()`, which would hand the
   * heater back to the PID and switch it off.
   */
  void observe_last_switch(TracedHeater &heater) {
    const size_t was = heater.changes.size();
    for (int i = 0; i < 200 && heater.changes.size() == was; i++) HAL_test_advance_millis(5);
    heater.stop();
  }

  void autotune_watched(AutotuneReport &out, TracedHeater &heater, const char * const command) {
    heater.watch();
    autotune_run(out, command);
    observe_last_switch(heater);
  }

  /**
   * Where each reported cycle appears in the trace.
   *
   * The relay's levels are, in order: nothing (the tune has not started), half power
   * while the first heat-up runs, and then one pair per relay cycle. Cycles are reported
   * from the second onwards, so the report for cycle `k` is printed at the switch that
   * begins trace interval `5 + 2k` — and the levels either side of it, and the two
   * switches before it, are that cycle's own history.
   */
  size_t heats_on_for_cycle(const size_t k) { return 5 + 2 * k; }   // the switch that reports cycle k
  size_t cools_off_for_cycle(const size_t k) { return 4 + 2 * k; }  // ...the one before it
  size_t heated_on_before_cycle(const size_t k) { return 3 + 2 * k; }

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

  // The whole line, not just the reason: the same words reach the host a second time as
  // a notification, so matching the reason alone would pass with the refusal itself
  // never reported.
  TEST_ASSERT_TRUE(reported::saw(text, STR_PID_AUTOTUNE STR_PID_TEMP_TOO_HIGH));
  TEST_ASSERT_FALSE(reported::saw(text, STR_PID_AUTOTUNE STR_PID_AUTOTUNE_START));
  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));

  // ...and the host is told separately, because a host driving a tune is not reading the
  // temperature log.
  TEST_ASSERT_TRUE(reported::saw(text, "//action:notification " HOST_NOTIFY_TOO_HIGH));
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
  TracedHeater barely(HEATER_0_PIN, TEMP_0_PIN, 200.0f, 20.0f);
  SavedPID saved;

  barely.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  {
    const millis_t started = millis();
    SerialCapture capture;
    barely.watch();
    thermalManager.PID_autotune(TUNE_TARGET, 0, 6, false);
    observe_last_switch(barely);
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

  /**
   * And the relay levels are still the pair the report describes.
   *
   * This is the only fixture where the two levels are genuinely different numbers: while
   * the bias sits below half power `d` equals it and the lower level is simply zero, so
   * a cycle down there cannot tell "bias - d" apart from "bias / d" or from a shift by
   * the wrong amount. Above half power the mirroring makes the lower level `bias - 127`
   * and the upper a constant 127, and every way of combining them is a different number.
   */
  for (size_t k = 0; k < report.bias.size(); k++) {
    const long bias = long(report.bias[k]), d = long(report.d[k]);
    TEST_ASSERT_EQUAL((bias + d) >> 1, barely.changes[heats_on_for_cycle(k)].level);
    const size_t off = heats_on_for_cycle(k) + 1;
    if (off < barely.changes.size())
      TEST_ASSERT_EQUAL((bias - d) >> 1, barely.changes[off].level);
  }
}

//
// ---- What the relay actually did, watched from the heater ----
//

/**
 * The relay applies exactly the two power levels the report describes.
 *
 * `bias` and `d` are only meaningful as the levels they produce: the heater is driven at
 * `(bias + d) / 2` for the heating half and `(bias - d) / 2` for the cooling half, so the
 * report's mid-point and half-swing are a claim about the square wave on the pin. Reading
 * that wave back and checking it against the report is what makes those two numbers mean
 * anything — a report is otherwise free to describe a relay that was never applied.
 *
 * The structure of the wave is a claim too. Autotune runs one settling cycle before the
 * first cycle it reports, so `ncycles` reported cycles take `ncycles + 1` relay cycles,
 * with half power applied throughout the initial heat-up.
 */
MARLIN_TEST(pid_autotune, the_relay_applies_the_two_power_levels_the_report_describes) {
  SimulatedMachine machine;
  TracedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  constexpr int8_t CYCLES = 3;
  AutotuneReport report;
  autotune_watched(report, hotend, "M303 E0 S180 C3");

  // Nothing was being applied, then half power for the heat-up, then two switches per
  // relay cycle — one settling cycle plus one per reported cycle.
  TEST_ASSERT_EQUAL(size_t(2 + 2 * (CYCLES + 1)), hotend.changes.size());
  TEST_ASSERT_EQUAL(0, hotend.changes[0].level);
  TEST_ASSERT_EQUAL(START_LEVEL, hotend.changes[1].level);

  // From the heat-up onwards the level alternates down, up, down, up: no two consecutive
  // switches in the same direction, which is what "relay" means.
  for (size_t i = 2; i < hotend.changes.size(); i++) {
    const bool falling = (i % 2) == 0;
    if (falling) TEST_ASSERT_TRUE(hotend.changes[i].level < hotend.changes[i-1].level);
    else         TEST_ASSERT_TRUE(hotend.changes[i].level > hotend.changes[i-1].level);
  }

  TEST_ASSERT_EQUAL(size_t(CYCLES), report.bias.size());
  for (size_t k = 0; k < report.bias.size(); k++) {
    const long bias = long(report.bias[k]), d = long(report.d[k]);

    // The level applied at the moment this cycle was reported is the upper relay level.
    TEST_ASSERT_EQUAL((bias + d) >> 1, hotend.changes[heats_on_for_cycle(k)].level);

    // ...and the next switch, still on this cycle's bias, is the lower one.
    const size_t off = heats_on_for_cycle(k) + 1;
    if (off < hotend.changes.size())
      TEST_ASSERT_EQUAL((bias - d) >> 1, hotend.changes[off].level);
  }
}

/**
 * The period reported is the time the relay actually took, and neither half is shorter
 * than the relay's minimum hold.
 *
 * `Tu` is the sum of the two half-periods the firmware timed for itself. Timing the same
 * two halves from the heater gives an independent measurement of the same quantity, so
 * the two must agree — which pins the units, the arithmetic, and the choice of which
 * instants are being subtracted. Each half is separately bounded below by the 3 s hold
 * for a hotend, and here the hold is the binding constraint rather than the temperature,
 * so each half is that hold plus at most the interval between temperature samples.
 */
MARLIN_TEST(pid_autotune, the_reported_period_is_the_time_the_relay_actually_took) {
  SimulatedMachine machine;
  TracedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_watched(report, hotend, "M303 E0 S180 C4");

  // No half after the initial heat-up is shorter than the hold. The hold is the binding
  // constraint here rather than the temperature — the heater crosses the target well
  // inside 3 s — so each half is that hold plus at most the wait for the next
  // temperature sample, less the moment the ISR takes to notice the switch.
  for (size_t i = 3; i < hotend.changes.size(); i++) {
    const millis_t held = hotend.changes[i].at - hotend.changes[i-1].at;
    TEST_ASSERT_TRUE(held >= millis_t(HOTEND_RELAY_DELAY_S * 1000.0f) - 200);
    TEST_ASSERT_TRUE(held <= 2 * millis_t(HOTEND_RELAY_DELAY_S * 1000.0f));
  }

  // ...and the period reported for a cycle is the two halves that made it up.
  TEST_ASSERT_TRUE(report.Tu.size() > 0);
  for (size_t k = 0; k < report.Tu.size(); k++) {
    const size_t c = report.cycle_of_gains(k);
    const millis_t measured = hotend.changes[heats_on_for_cycle(c)].at
                            - hotend.changes[heated_on_before_cycle(c)].at;
    TEST_ASSERT_FLOAT_WITHIN(0.4f, float(measured) / 1000.0f, float(report.Tu[k]));
  }
}

/**
 * The extremes reported are the temperatures the heater actually reached.
 *
 * The ultimate gain is inversely proportional to the swing between them, so a `maxT` or
 * `minT` carried over from an earlier cycle — or never reset — widens the swing and
 * understates Ku, while every relation between the *reported* numbers still holds. Only a temperature measured
 * outside the firmware can catch that, and the model has one: it is driven by the same
 * pin and knows nothing of what the firmware read.
 *
 * Each extreme belongs to a known instant. `maxT` is reset when the relay switches off,
 * with the heater at its peak, so the peak recorded is the temperature at that switch;
 * `minT` is reset when it switches on, at the bottom of the cooling half, so the trough
 * recorded is the temperature at the *previous* switch-on. Both are read one sample
 * before the model's own record of the same instant, which at this heater's rate of a few
 * degrees a second is worth a degree or so.
 */
MARLIN_TEST(pid_autotune, the_reported_extremes_are_the_temperatures_the_heater_reached) {
  SimulatedMachine machine;
  TracedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_watched(report, hotend, "M303 E0 S180 C3");

  TEST_ASSERT_TRUE(report.maxT.size() > 0);
  for (size_t k = 0; k < report.maxT.size(); k++) {
    TEST_ASSERT_FLOAT_WITHIN(2.5f, hotend.changes[cools_off_for_cycle(k)].model_c, float(report.maxT[k]));
    TEST_ASSERT_FLOAT_WITHIN(2.5f, hotend.changes[heated_on_before_cycle(k)].model_c, float(report.minT[k]));
  }
}

/**
 * A heater that cools far more slowly than it heats pins the bias to its lower limit.
 *
 * The bias moves towards whichever half of the cycle is longer, so a block that sheds
 * heat slowly spends far longer falling back to the target than climbing above it and the
 * bias is driven down. It cannot go below 20 of 255: below half power the swing equals the
 * bias, so the upper relay level is the bias itself, and a bias of a few counts would
 * barely drive the heater at all — there would be nothing left to oscillate. This is the
 * mirror of the barely-adequate heater above, and reaches the other end of the same
 * clamp — the two together are what say the limits are 20 and MAX-20 rather than
 * anything else.
 */
MARLIN_TEST(pid_autotune, a_slow_cooling_heater_pins_the_bias_to_its_lower_limit) {
  SimulatedMachine machine;
  // Reaches 320 C at full power with a 20 s time constant, but takes 400 s to cool: a
  // well insulated block. Full power is kept low enough that the first cycle's overshoot
  // stays inside both the autotune limit and MAXTEMP.
  TracedHeater insulated(HEATER_0_PIN, TEMP_0_PIN, 320.0f, 20.0f, 400.0f);
  SavedPID saved;

  insulated.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_watched(report, insulated, "M303 E0 S180 C4");

  TEST_ASSERT_TRUE(report.bias.size() > 0);
  bool saw_the_limit = false;
  for (size_t i = 0; i < report.bias.size(); i++) {
    const long bias = long(report.bias[i]), d = long(report.d[i]);
    TEST_ASSERT_TRUE(bias >= BIAS_MIN);
    TEST_ASSERT_EQUAL(bias, d);                   // below half power the swing equals the bias
    if (bias == BIAS_MIN) saw_the_limit = true;
  }
  TEST_ASSERT_TRUE(saw_the_limit);

  /**
   * The peaks belong to their own cycle, and here that can be seen.
   *
   * Once the bias settles at its limit the relay barely disturbs the temperature, so each
   * cycle peaks *lower* than the one before — the opposite of a heater still warming up.
   * A `maxT` that was never reset would therefore keep reporting the first, highest peak,
   * which a rising sequence of peaks hides completely.
   */
  for (size_t k = 0; k < report.maxT.size(); k++) {
    TEST_ASSERT_FLOAT_WITHIN(1.0f, insulated.changes[cools_off_for_cycle(k)].model_c, float(report.maxT[k]));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, insulated.changes[heated_on_before_cycle(k)].model_c, float(report.minT[k]));
  }
  TEST_ASSERT_TRUE(report.maxT.back() < report.maxT.front());   // the peaks really do fall
}

/**
 * A heater too fast to control stops the tune rather than measuring it.
 *
 * The relay holds each half for at least 3 s, so a heater that can add 30 C in that time
 * is one autotune cannot bring back to the target — its overshoot would keep growing.
 * Rather than report a measurement taken from a runaway, the tune stops as soon as it
 * sees the target exceeded by more than the overshoot allowance, and produces no gains at
 * all. The heater here reaches its first switch-off point before it can switch, which is
 * the only way to overshoot that far.
 */
MARLIN_TEST(pid_autotune, a_heater_too_fast_to_control_stops_the_tune_on_overshoot) {
  SimulatedMachine machine;
  TracedHotend runaway(400.0f, 4.0f);         // ~50 C/s at the target: five times a hotend
  SavedPID saved;

  runaway.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  // No settling advance here: giving up switches the heater off inside the call, so the
  // last trace entry is the moment it stopped — and a heater this fast sheds 50 C a
  // second afterwards, which would lose the very temperature under test.
  AutotuneReport report;
  runaway.watch();
  autotune_run(report, "M303 E0 S180 C3");
  runaway.stop();

  TEST_ASSERT_TRUE(reported::saw(report.text, STR_PID_AUTOTUNE STR_PID_TEMP_TOO_HIGH));
  TEST_ASSERT_TRUE(reported::saw(report.text, "//action:notification " HOST_NOTIFY_TOO_HIGH));
  TEST_ASSERT_FALSE(reported::saw(report.text, STR_PID_AUTOTUNE_FINISHED));
  TEST_ASSERT_EQUAL(0u, report.Ku.size());      // nothing was measured
  TEST_ASSERT_EQUAL(0u, report.bias.size());

  // It stopped past the allowance and not far past it — the heater is still climbing, so
  // the reading it stopped on is a sample or so behind the model.
  const float stopped_at = runaway.changes.back().model_c;
  TEST_ASSERT_TRUE(stopped_at > float(TUNE_TARGET + MAX_OVERSHOOT_C));
  TEST_ASSERT_TRUE(stopped_at < float(TUNE_TARGET + 2 * MAX_OVERSHOOT_C));

  // The heater is switched off on the way out, not left where the runaway found it.
  TEST_ASSERT_EQUAL(0, runaway.changes.back().level);
}

//
// ---- What the host was told while it ran ----
//

/**
 * Heater states are reported every two seconds for as long as the tune runs.
 *
 * A tune takes minutes on a real machine and the host has to be able to watch it, so the
 * same periodic report as `M105` is emitted throughout — once at the start and then every
 * two seconds. The count is therefore fixed by the duration rather than by the number of
 * cycles, and each report is a line of its own.
 */
MARLIN_TEST(pid_autotune, heater_states_are_reported_every_two_seconds_throughout) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  const size_t reports = reported::occurrences(report.text, " T:"),
               expected = size_t(report.took / 2000) + 1;
  TEST_ASSERT_TRUE(report.took > 10000);        // the tune ran long enough for this to mean something
  TEST_ASSERT_TRUE(reports + 1 >= expected && reports <= expected + 1);

  // Each of them ends its line.
  TEST_ASSERT_TRUE(reported::occurrences(report.text, "\n") >= reports);
}

/**
 * The configuration lines printed at the end carry the gains that were measured.
 *
 * The tune's product is three lines an operator pastes into Configuration.h, so they have
 * to name the right constant and carry the right value: a KP line holding Ki is a working
 * printer tuned wrongly, and nothing else in the report would show it. Each line is
 * checked against the gain the cycle report gave for the same tune, and — because `U1`
 * applies the result — against what the live PID ended up with.
 */
MARLIN_TEST(pid_autotune, the_configuration_lines_printed_carry_the_measured_gains) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3 U1");

  const std::vector<double> kp = reported::all_numbers(report.text, "#define DEFAULT_KP "),
                            ki = reported::all_numbers(report.text, "#define DEFAULT_KI "),
                            kd = reported::all_numbers(report.text, "#define DEFAULT_KD ");
  TEST_ASSERT_EQUAL(1u, kp.size());
  TEST_ASSERT_EQUAL(1u, ki.size());
  TEST_ASSERT_EQUAL(1u, kd.size());

  TEST_ASSERT_TRUE(report.Kp.size() > 0);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, float(report.Kp.back()), float(kp[0]));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, float(report.Ki.back()), float(ki[0]));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, float(report.Kd.back()), float(kd[0]));

  TEST_ASSERT_FLOAT_WITHIN(0.005f, thermalManager.temp_hotend[0].pid.p(), float(kp[0]));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, thermalManager.temp_hotend[0].pid.i(), float(ki[0]));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, thermalManager.temp_hotend[0].pid.d(), float(kd[0]));

  // A hotend is tuned by the classic relations, and the report says so.
  TEST_ASSERT_TRUE(reported::saw(report.text, STR_CLASSIC_PID));

  // A host is told when the tune starts heating and when it is over, rather than being
  // left to infer either from the temperature log.
  TEST_ASSERT_TRUE(reported::saw(report.text, "//action:notification " HOST_NOTIFY_HEATING));
  TEST_ASSERT_TRUE(reported::saw(report.text, "//action:notification " HOST_NOTIFY_DONE));
}

//
// ---- The edges of what will be tuned ----
//

/**
 * The highest target the hotend allows is tuned, not refused.
 *
 * The limit is MAXTEMP less the overshoot allowance, and the test above shows one degree
 * past it is refused. The boundary itself is the interesting half of that rule: a target
 * exactly at the limit is still allowed, so the check is "above the limit" and not "at
 * it". The heater here is weak enough that tuning near MAXTEMP does not trip the
 * hardware's own limit, and slow enough to cool that it never falls the 30 C below the
 * target that thermal protection reads as a runaway: a heater with little power left at
 * 260 C could not climb back from a fast fall inside the time that check allows.
 */
MARLIN_TEST(pid_autotune, the_highest_allowed_target_is_tuned_rather_than_refused) {
  SimulatedMachine machine;
  SimulatedHeater gentle(HEATER_0_PIN, TEMP_0_PIN, 285.0f, 20.0f, 400.0f);
  SavedPID saved;

  gentle.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  char cmd[32];
  snprintf(cmd, sizeof(cmd), "M303 E0 C3 S%d", int(Temperature::hotend_max_target(0)));

  AutotuneReport report;
  autotune_run(report, cmd);

  TEST_ASSERT_FALSE(reported::saw(report.text, STR_PID_TEMP_TOO_HIGH));
  TEST_ASSERT_TRUE(reported::saw(report.text, STR_PID_AUTOTUNE STR_PID_AUTOTUNE_START));
  TEST_ASSERT_TRUE(report.Ku.size() > 0);
}

/**
 * A swing of less than a degree is still measured.
 *
 * The check the gains are guarded by exists to keep a zero swing out of a division, not
 * to reject a small one: a heavy, well insulated block barely moves inside one relay
 * hold, and the ultimate gain that comes out of that — a large one, since Ku rises as the
 * swing falls — is exactly what the tune is for. The narrowest swing this can be asked
 * about is one ADC count, about half a degree at this temperature, so a swing under a
 * degree is close to the floor of what the sensor can express.
 */
MARLIN_TEST(pid_autotune, a_swing_of_less_than_a_degree_still_produces_gains) {
  SimulatedMachine machine;
  // Heats slowly enough that a 3 s hold moves it well under a degree once the bias has
  // settled, and cools slower still.
  SimulatedHeater heavy(HEATER_0_PIN, TEMP_0_PIN, 320.0f, 80.0f, 1500.0f);
  SavedPID saved;

  heavy.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C4");

  TEST_ASSERT_TRUE(report.Ku.size() > 0);
  for (size_t k = 0; k < report.Ku.size(); k++) {
    const size_t c = report.cycle_of_gains(k);
    const double swing = report.maxT[c] - report.minT[c];
    TEST_ASSERT_TRUE(swing > 0.0);
    TEST_ASSERT_TRUE(swing < 1.0);
    // ...and it is the same relay-gain relation as any other swing, just a larger gain.
    const double expected = (4.0 * report.d[c]) / (M_PI * swing * 0.5);
    TEST_ASSERT_FLOAT_WITHIN(float(expected * 0.01 / swing + 0.01), float(expected), float(report.Ku[k]));
  }
}

/**
 * A cycle that never completes ends the tune rather than waiting for ever.
 *
 * The relay only advances when the temperature crosses the target in each direction. A
 * heater that overshoots and then holds — a block with nowhere to lose its heat to —
 * never comes back down, so the cycle it is in the middle of would never finish and the
 * printer would sit hot indefinitely. Twenty minutes without a completed crossing ends
 * the tune with nothing measured.
 *
 * The timeout is counted from the earlier of the two switch instants, both of which start
 * at the beginning of the tune, so it is twenty minutes from the start here rather than
 * from the one switch that did happen.
 */
MARLIN_TEST(pid_autotune, a_cycle_that_never_completes_times_the_tune_out) {
  SimulatedMachine machine;
  // Reaches 220 C at full power, and effectively never cools: once it is over the target
  // it stays there.
  SimulatedHeater stuck(HEATER_0_PIN, TEMP_0_PIN, 220.0f, 20.0f, 1.0e6f);
  SavedPID saved;

  stuck.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  TEST_ASSERT_TRUE(reported::saw(report.text, STR_PID_AUTOTUNE STR_PID_TIMEOUT));
  TEST_ASSERT_FALSE(reported::saw(report.text, STR_PID_AUTOTUNE_FINISHED));
  TEST_ASSERT_EQUAL(0u, report.Ku.size());

  // Twenty minutes from the start of the tune, not from the single switch it managed
  // some thirty seconds in.
  constexpr millis_t TIMEOUT_MS = 20UL * 60UL * 1000UL;
  TEST_ASSERT_TRUE(report.took > TIMEOUT_MS);
  TEST_ASSERT_TRUE(report.took < TIMEOUT_MS + 10000UL);
}

/**
 * A tune abandons whatever the print had been heating to.
 *
 * Autotune drives the heater as a relay, which it cannot do while a target is still
 * being regulated, so every heater is switched off and un-targeted before the first
 * cycle. The target does not come back when the tune ends: the machine is left cold and
 * the caller has to ask for a temperature again.
 */
MARLIN_TEST(pid_autotune, a_tune_abandons_the_target_the_print_had_set) {
  SimulatedMachine machine;
  SimulatedHotend hotend;
  SavedPID saved;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  thermalManager.setTargetHotend(150, 0);
  TEST_ASSERT_EQUAL(150, thermalManager.degTargetHotend(0));

  AutotuneReport report;
  autotune_run(report, "M303 E0 S180 C3");

  TEST_ASSERT_TRUE(reported::saw(report.text, STR_PID_AUTOTUNE_FINISHED));
  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));
}


// ---------------------------------------------------------------------------
// The bed — see test/014-pid_bed.ini
// ---------------------------------------------------------------------------

/**
 * `PID_autotune()` is one function serving three heaters, and the heater it is serving
 * changes the constants it uses.
 *
 *     const millis_t relay_delay = (isbed || ischamber) ? 5000UL : 3000UL;
 *     pf = (ischamber || isbed) ? 0.2f      : 0.6f,
 *     df = (ischamber || isbed) ? 1.0f/3.0f : 1.0f/8.0f;
 *
 * With `PIDTEMPBED` off — which is every other configuration here — `isbed` is a
 * compile-time false and each of those true arms is unreachable. Not untested:
 * unreachable. Forty-seven mutants across eleven lines survived for that one reason,
 * thirteen of them on the `df` line alone.
 *
 * The distinction is physical rather than cosmetic. A bed has far more thermal mass than a
 * nozzle, so it is held at each relay level for five seconds instead of three, and it is
 * given the *conservative* Ziegler-Nichols factors: a proportional gain a third of the
 * hotend's and a derivative term nearly three times larger relative to it. Tune a bed with
 * the hotend's numbers and you get a bed that oscillates around its target for the whole
 * print.
 *
 * These tests do not repeat the hotend's twenty; they assert the three things that differ.
 */
#if ENABLED(PIDTEMPBED) && HAS_HEATED_BED

namespace {

  struct SavedBedPID {
    raw_pid_t was;
    SavedBedPID() : was({ thermalManager.temp_bed.pid.p(),
                          thermalManager.temp_bed.pid.i(),
                          thermalManager.temp_bed.pid.d() }) {}
    ~SavedBedPID() {
      thermalManager.temp_bed.pid.set(was);
      thermalManager.setTargetBed(0);
    }
  };

}

/**
 * The bed is tuned with the bed's factors, not the hotend's.
 *
 * `Ku` and `Tu` are the measurement; `pf` and `df` are the judgement applied to it, and
 * they are the whole difference between the two heaters. Asserting the applied gains
 * against the *reported* `Ku` and `Tu` separates the judgement from the measurement — a
 * mutant that changed how the oscillation was measured would move both sides together and
 * be invisible here, which is what the hotend's own relay tests are for.
 *
 * The hotend's factors are named in the failure messages so that a run using them is
 * reported as what it is rather than as an unexplained number.
 */
MARLIN_TEST(pid_autotune, the_bed_is_tuned_with_the_beds_own_factors) {
  SimulatedMachine machine;
  SimulatedBed bed;
  SavedBedPID saved;

  bed.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E-1 S70 C3 U1");

  TEST_ASSERT_TRUE_MESSAGE(report.Ku.size() > 0, "the bed tune should report a Ku");
  const double Ku = report.Ku.back(), Tu = report.Tu.back();

  const float p = thermalManager.temp_bed.pid.p(),
              i = thermalManager.temp_bed.pid.i(),
              d = thermalManager.temp_bed.pid.d();

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, float(0.2 * Ku), p,
    "the bed's proportional gain should be 0.2*Ku, not the hotend's 0.6*Ku");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, float(2.0 * (0.2 * Ku) / Tu), i,
    "and its integral gain 2Kp/Tu");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, float((0.2 * Ku) * Tu / 3.0), d,
    "and its derivative gain Kp*Tu/3, not the hotend's Kp*Tu/8");

  // Between themselves, with the period eliminated: Ki*Kd is Kp^2 * 2/3 for the bed where
  // it is Kp^2/4 for the hotend, and that identity holds at full precision rather than at
  // the two decimals Tu was reported to.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, p * p * 2.0f / 3.0f, i * d,
    "the bed's three gains should stand in the bed's relation to each other");
}

/**
 * ...and the numbers it publishes are the numbers it applied.
 *
 * The report is what an operator copies into their configuration, so a tune that applied
 * one set and printed another would be worse than one that failed.
 */
MARLIN_TEST(pid_autotune, the_bed_reports_the_gains_it_applied) {
  SimulatedMachine machine;
  SimulatedBed bed;
  SavedBedPID saved;

  bed.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E-1 S70 C3 U1");

  TEST_ASSERT_TRUE(report.Kp.size() > 0);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, float(report.Kp.back()), thermalManager.temp_bed.pid.p());
  TEST_ASSERT_FLOAT_WITHIN(0.005f, float(report.Ki.back()), thermalManager.temp_bed.pid.i());
  TEST_ASSERT_FLOAT_WITHIN(0.005f, float(report.Kd.back()), thermalManager.temp_bed.pid.d());
}

/**
 * The bed holds each relay level for five seconds, where the hotend holds three.
 *
 * `relay_delay` is the minimum time at one power level before the controller is allowed to
 * switch, and it exists because a heater with a lot of thermal mass lags: switch on the
 * temperature alone and the measured period is the sensor's noise rather than the bed's
 * response. Asserting it needs the *time* the tune took, since the delay is a floor on how
 * short a half-cycle can be — three cycles cannot be done in less than six half-periods of
 * five seconds each.
 */
MARLIN_TEST(pid_autotune, the_bed_is_held_at_each_relay_level_for_its_own_minimum) {
  SimulatedMachine machine;
  SimulatedBed bed;
  SavedBedPID saved;

  bed.starts_at(SimulatedHeater::AMBIENT_C);
  time_passes_ms(400);

  AutotuneReport report;
  autotune_run(report, "M303 E-1 S70 C3 U0");

  TEST_ASSERT_TRUE_MESSAGE(report.Tu.size() > 0, "the bed tune should report a period");

  // Each reported period is two relay holds, so no period can be shorter than twice the
  // bed's own delay. The hotend's 3000 ms would allow 6 s periods; the bed's 5000 does not.
  for (const double tu : report.Tu)
    TEST_ASSERT_TRUE_MESSAGE(tu >= 2.0 * 5.0,
      "a bed relay period should be at least twice the bed's five second hold");
}

#endif // PIDTEMPBED && HAS_HEATED_BED

#endif // PIDTEMP
