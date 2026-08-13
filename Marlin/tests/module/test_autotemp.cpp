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
 * Autotemp: the nozzle gets hotter when the print goes faster.
 *
 * Filament needs a certain amount of heat per millimetre, so a nozzle held at one
 * temperature is either too cold at speed or hotter than it needs to be when slow.
 * `M104 S<min> B<max> F<factor>` asks the firmware to track it instead: every pass it
 * looks at the fastest extrusion queued in the planner and sets the target to
 * `min + speed * factor`, capped at `max`.
 *
 * The whole feature was uncovered — 27 surviving mutants across
 * `autotemp_update()`, `autotemp_M104_M109()` and `autotemp_task()`, and the only
 * mention of it anywhere in the suite was a fixture restoring its `enabled` flag.
 *
 * Two things shape the tests below.
 *
 * **The arithmetic is asserted on `calculate()` directly, and the behaviour through
 * `M104`.** `calculate()` is a public method taking the speed as an argument, so the
 * relation between speed and temperature can be stated exactly rather than inferred from
 * a machine arranged to produce it. The command-level tests then check the parts
 * `calculate()` cannot see: which parameters enable it, and the two guards in
 * `autotemp_task()` that decide whether it runs at all.
 *
 * **`calculate()` keeps a function-local `static float oldt`**, so it is not a pure
 * function and one test's last call is the next test's starting point. That is a real
 * property of the code rather than an inconvenience — it is what smooths the temperature
 * on the way *down* — but it has to be handled deliberately, so every test here begins by
 * driving the value up to a known point. Going up is unsmoothed and therefore exact;
 * going down is not. `settled_at()` is that helper, and it is the reason the tests read
 * as they do.
 */

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../gcode/simulated_sensors.h"
#include "src/module/temperature.h"
#include "src/module/planner.h"
#include "src/module/motion.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include <string.h>

#if ENABLED(AUTOTEMP) && HAS_HOTEND

namespace {

  constexpr celsius_t CFG_MIN = AUTOTEMP_MIN, CFG_MAX = AUTOTEMP_MAX;
  constexpr float OLDWEIGHT = AUTOTEMP_OLDWEIGHT;

  struct SavedAutotemp {
    autotemp_cfg_t cfg;
    bool enabled, was_connected;
    celsius_t target;
    SavedAutotemp() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      cfg = thermalManager.autotemp.cfg;
      enabled = thermalManager.autotemp.enabled;
      target = thermalManager.degTargetHotend(0);
    }
    ~SavedAutotemp() {
      thermalManager.autotemp.cfg = cfg;
      thermalManager.autotemp.enabled = enabled;
      thermalManager.setTargetHotend(target, 0);
      MYSERIAL1.host_connected = was_connected;
    }
  };

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  /**
   * Put `calculate()`'s hidden `oldt` at a known value, and say what it is.
   *
   * Only the upward path is exact — `if (t < oldt)` is what smooths, so a call that
   * produces a value at or above the previous one stores it unchanged. Calling with a
   * speed that saturates the cap therefore leaves `oldt` at `cfg.max` whatever it was
   * before, which is the only starting point a test can rely on without reading a private
   * static.
   */
  void settled_at_the_cap() {
    thermalManager.autotemp.calculate(10000);        // far past anything the cap allows
  }

}

// ---------------------------------------------------------------------------
// The relation between speed and temperature
// ---------------------------------------------------------------------------

/**
 * With nothing extruding, the target is the floor and nothing else.
 *
 * `min` is what the user asked for at zero speed, so this is the one point on the line
 * that is not a matter of tuning. A machine that added something here would run every
 * idle nozzle hot.
 */
MARLIN_TEST(autotemp, a_stationary_extruder_asks_for_the_minimum) {
  SavedAutotemp saved;

  thermalManager.autotemp.cfg = { CFG_MIN, CFG_MAX, 0.1f };
  settled_at_the_cap();

  // Down from the cap, so the smoothing applies and the answer is not simply `min`.
  // Repeating drives it asymptotically to the floor; assert it converges there.
  for (uint16_t i = 0; i < 2000; i++) thermalManager.autotemp.calculate(0);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, float(CFG_MIN), thermalManager.autotemp.calculate(0),
    "with nothing extruding the target should settle at the configured minimum");
}

/**
 * Faster extrusion asks for a hotter nozzle, and by exactly the factor.
 *
 * The claim worth pinning is the *slope*, not any single value: doubling the speed
 * difference doubles the temperature difference, and one degree per `1/factor` mm/s is
 * what the user set `F` to mean. Asserting a single temperature would pass with the
 * factor, the floor, or both, wrong in compensating directions.
 *
 * Measured upward from the floor so nothing is smoothed. `factor` is deliberately not the
 * configured default — a test that used it could not tell the parameter from the constant.
 */
MARLIN_TEST(autotemp, the_target_rises_with_speed_at_the_factor_that_was_set) {
  SavedAutotemp saved;

  constexpr float FACTOR = 0.25f;
  thermalManager.autotemp.cfg = { CFG_MIN, CFG_MAX, FACTOR };

  // Climb, so every value is stored unsmoothed.
  const float at_20  = thermalManager.autotemp.calculate(20);
  const float at_60  = thermalManager.autotemp.calculate(60);
  const float at_100 = thermalManager.autotemp.calculate(100);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, float(CFG_MIN) + 20 * FACTOR, at_20,
    "the target should be the floor plus speed times the factor");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 40 * FACTOR, at_60 - at_20,
    "a 40 mm/s increase should raise the target by 40 times the factor");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, at_60 - at_20, at_100 - at_60,
    "and equal increases in speed should give equal increases in temperature");
}

/**
 * The ceiling is a ceiling — bracketed either side of it.
 *
 * `B` is the temperature above which the filament degrades, so the cap is the parameter
 * with a physical consequence for exceeding it. Showing only that a huge speed is capped
 * would pass with the cap anywhere at or below `max`; the speed just short of it has to
 * come through untouched.
 */
MARLIN_TEST(autotemp, the_target_is_capped_at_the_maximum_and_not_before_it) {
  SavedAutotemp saved;

  constexpr float FACTOR = 0.1f;
  thermalManager.autotemp.cfg = { CFG_MIN, CFG_MAX, FACTOR };

  // The speed that lands one degree under the cap, and one that would blow past it.
  const float just_under = (float(CFG_MAX) - 1 - float(CFG_MIN)) / FACTOR;

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, float(CFG_MAX) - 1,
    thermalManager.autotemp.calculate(celsius_t(just_under)),
    "a speed short of the cap should not be capped");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, float(CFG_MAX),
    thermalManager.autotemp.calculate(celsius_t(just_under * 4)),
    "and one well past it should be held at the maximum");
}

/**
 * It goes up at once and comes down slowly, and that asymmetry is the point.
 *
 * A nozzle that chased every dip in extrusion speed would spend the print oscillating,
 * so the drop is weighted against the previous value while the rise is not. Both
 * directions are asserted from the same starting temperature, because "it changed
 * slowly" means nothing without "and the other way it did not".
 */
MARLIN_TEST(autotemp, the_target_rises_immediately_and_falls_gradually) {
  SavedAutotemp saved;

  constexpr float FACTOR = 0.1f;
  thermalManager.autotemp.cfg = { CFG_MIN, CFG_MAX, FACTOR };

  settled_at_the_cap();                       // oldt is now cfg.max, exactly

  // Straight down to the floor: the answer must be near the top still, not near the floor.
  const float after_one_slow_pass = thermalManager.autotemp.calculate(0);
  const float unsmoothed = float(CFG_MIN);
  const float expected = unsmoothed * (1.0f - OLDWEIGHT) + float(CFG_MAX) * OLDWEIGHT;

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, expected, after_one_slow_pass,
    "a drop should be weighted against the previous target, not taken in one step");
  TEST_ASSERT_TRUE_MESSAGE(after_one_slow_pass > unsmoothed + 10.0f,
    "and should still be a long way above the value it is heading for");

  // ...whereas going back up is taken in full, from that same smoothed value.
  const float back_up = thermalManager.autotemp.calculate(celsius_t((float(CFG_MAX) - float(CFG_MIN)) / FACTOR));
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, float(CFG_MAX), back_up,
    "a rise should be applied in full rather than weighted");
}

// ---------------------------------------------------------------------------
// What switches it on, and what stops it
// ---------------------------------------------------------------------------

/**
 * `F` is what turns it on, and `F0` is what turns it off again.
 *
 * The enable is derived from the factor rather than stored separately — `enabled =
 * factor != 0` — so the off switch and the "no adjustment" setting are the same thing.
 * Both directions are checked because the natural test only sets it.
 */
MARLIN_TEST(autotemp, the_F_parameter_switches_autotemp_on_and_off) {
  SimulatedMachine machine;
  SavedAutotemp saved;

  // Deliberately *not* AUTOTEMP_MIN and AUTOTEMP_MAX. The first version of this test used
  // 210 and 250, which are the configured defaults — so deleting either assignment left the
  // field holding the value the test was asserting, and six mutants of these two lines
  // survived. A parameter test whose value coincides with the default asserts nothing about
  // the parameter.
  host_sends("M104 S215 B245 F0.2");
  TEST_ASSERT_TRUE_MESSAGE(thermalManager.autotemp.enabled, "F with a factor should enable autotemp");
  TEST_ASSERT_EQUAL_MESSAGE(215, thermalManager.autotemp.cfg.min, "S should set the floor");
  TEST_ASSERT_EQUAL_MESSAGE(245, thermalManager.autotemp.cfg.max, "B should set the ceiling");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.2f, thermalManager.autotemp.cfg.factor, "F should set the factor");

  host_sends("M104 S215 B245 F0");
  TEST_ASSERT_FALSE_MESSAGE(thermalManager.autotemp.enabled, "a zero factor should switch it off again");

  // Any non-zero factor is "on", including one that asks for a cooler nozzle at speed.
  // The enable is written `factor != 0`, and `> 0` would agree with it everywhere else.
  host_sends("M104 S215 B245 F-0.2");
  TEST_ASSERT_TRUE_MESSAGE(thermalManager.autotemp.enabled,
    "a negative factor is still a factor, and should enable autotemp");
}

/**
 * An M104 without `F` leaves autotemp off.
 *
 * This is the ordinary case — every print that never asks for autotemp sends plenty of
 * M104s — and it is the one a mutant that ignored the parameter would break. Asserting
 * only the enabling direction would pass against firmware that enabled it on every
 * temperature change, which would silently override the temperature the user asked for.
 */
MARLIN_TEST(autotemp, an_ordinary_M104_does_not_switch_autotemp_on) {
  SimulatedMachine machine;
  SavedAutotemp saved;

  host_sends("M104 S210 B250 F0.2");
  TEST_ASSERT_TRUE(thermalManager.autotemp.enabled);

  host_sends("M104 S205");
  TEST_ASSERT_FALSE_MESSAGE(thermalManager.autotemp.enabled,
    "an M104 with no F parameter should leave autotemp off");
}

/**
 * The whole path, once: a queued extrusion actually moves the nozzle target.
 *
 * Everything above tests `calculate()` in isolation or the guards that decide whether to
 * call it. None of it says the machine ever *applies* the answer — `autotemp_task()` could
 * return early on every pass and every one of those tests would still pass. It is the
 * difference between a formula that is right and a feature that works, and one mutant
 * (`if (1==1) return;`) sits exactly in the gap.
 *
 * The assertion is that the target *rose* rather than that it reached a particular value:
 * the speed the planner reports depends on how the move was split and accelerated, which is
 * not the subject here. What is pinned is that extrusion in the queue reaches the target at
 * all, and the size of the step is pinned by the `calculate()` tests above.
 *
 * Note where the call comes from, because it is not where it looks like it should be:
 * `autotemp_task()` is invoked by `Planner::check_axes_activity()`, which
 * `manage_inactivity()` runs at 10 Hz — not by `Temperature::task()`. The first version of
 * this test drove the temperature task and failed against perfectly working firmware.
 */
MARLIN_TEST(autotemp, a_queued_extrusion_raises_the_nozzle_target) {
  SimulatedMachine machine;
  SavedAutotemp saved;
  SimulatedSensors sensors;

  // Extrusion is dropped by the planner while the nozzle is cold, silently — see the
  // cold-extrude gotcha — so the E steps have to be allowed through for the planner to
  // report any speed at all.
  REMEMBER(cold, thermalManager.allow_cold_extrude, true);

  // Set up through the command rather than by hand, because the order matters and doing
  // it by hand gets it wrong: `setTargetHotend()` *disables* autotemp (temperature.h:1059),
  // which is how an explicit M104 overrides it. `M104 S<min> B<max> F<factor>` sets the
  // target first and re-enables from F afterwards; a test that set the flag and then the
  // target would silently switch the feature off, which is what the first draft did.
  host_sends("M104 S210 B250 F0.5");
  TEST_ASSERT_TRUE(thermalManager.autotemp.enabled);
  TEST_ASSERT_EQUAL(210, thermalManager.degTargetHotend(0));

  host_sends("G91");
  host_sends("G1 X20 E4 F3000");                 // a fast extruding move, left in the queue
  host_sends("G90");
  TEST_ASSERT_TRUE_MESSAGE(planner.movesplanned() > 0, "the move should still be queued");
  TEST_ASSERT_TRUE_MESSAGE(planner.get_high_e_speed() > 0.0f,
    "the planner should report an extrusion speed for a queued extruding move");

  planner.check_axes_activity();                 // autotemp's actual caller

  TEST_ASSERT_TRUE_MESSAGE(thermalManager.degTargetHotend(0) > 210,
    "a queued extrusion should raise the nozzle target above the autotemp floor");
}

/**
 * While it is off, the target the user set is the target that stays.
 *
 * `autotemp_task()` runs on every pass of the temperature task whether the feature is on
 * or not, so the guard is doing work continuously. Losing it would let autotemp
 * overwrite the target on a machine that never asked for it — and the symptom would be a
 * nozzle sitting at `AUTOTEMP_MIN` regardless of what was commanded.
 */
MARLIN_TEST(autotemp, a_disabled_autotemp_never_touches_the_target) {
  SimulatedMachine machine;
  SavedAutotemp saved;
  SimulatedSensors sensors;

  thermalManager.autotemp.enabled = false;
  thermalManager.setTargetHotend(180, 0);      // deliberately below AUTOTEMP_MIN

  for (uint16_t i = 0; i < 200; i++) { HAL_test_advance_millis(1); thermalManager.task(); }

  TEST_ASSERT_EQUAL_MESSAGE(180, thermalManager.degTargetHotend(0),
    "autotemp is off, so the commanded target should be untouched");
}

/**
 * A nozzle below the autotemp range is left alone even when autotemp is on.
 *
 * The guard is `degTargetHotend() < cfg.min - 2`, and it exists so that a deliberate
 * cool-down is not fought by the feature: without it, `M104 S0` at the end of a print
 * would be immediately overridden back up to `AUTOTEMP_MIN` and the nozzle would never
 * cool. The two-degree margin is what stops it triggering on the rounding at the edge of
 * the range, so the test sits well below rather than exactly at the boundary.
 */
MARLIN_TEST(autotemp, a_target_below_the_autotemp_range_is_left_alone) {
  SimulatedMachine machine;
  SavedAutotemp saved;
  SimulatedSensors sensors;

  thermalManager.autotemp.cfg = { CFG_MIN, CFG_MAX, 0.1f };
  thermalManager.autotemp.enabled = true;
  thermalManager.setTargetHotend(CFG_MIN - 50, 0);

  for (uint16_t i = 0; i < 200; i++) { HAL_test_advance_millis(1); thermalManager.task(); }

  TEST_ASSERT_EQUAL_MESSAGE(CFG_MIN - 50, thermalManager.degTargetHotend(0),
    "a nozzle already below the autotemp range should not be dragged back up into it");
}

#endif // AUTOTEMP && HAS_HOTEND
