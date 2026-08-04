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
 * Measuring the bed.
 *
 * A probe is a limit switch that answers a different question. Instead of "where does the
 * axis end", it answers "where is the bed here" — and the answer is different at different
 * places over the bed, which is the entire reason levelling exists.
 *
 * So the fixture is a *surface* rather than a switch: `support/simulated_bed.h` closes the
 * probe when the nozzle reaches a plane whose height depends on the carriage's X and Y. The
 * plane is stated as a formula, which is what lets these tests assert a measurement against
 * the surface that produced it rather than against a number from a previous run.
 *
 * Test-HAL only: probing moves the machine and waits for it, which needs time that advances.
 */

#include "src/inc/MarlinConfig.h"

#if defined(__PLAT_TEST__) && HAS_BED_PROBE

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "../support/simulated_bed_surface.h"
#include "src/module/probe.h"
#include "src/module/motion.h"
#include "src/module/planner.h"


#include "src/module/stepper.h"

namespace {

  constexpr float SPM = SimulatedMachine::STEPS_PER_MM;

  struct XRail : SimulatedAxisWithLimit {
    XRail(const float at_mm)
      : SimulatedAxisWithLimit(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                               X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE,
                               int32_t(-1000.0f * SPM), int32_t(at_mm * SPM)) {}
  };

  struct YRail : SimulatedAxisWithLimit {
    YRail(const float at_mm)
      : SimulatedAxisWithLimit(Y_STEP_PIN, Y_DIR_PIN, ENABLED(INVERT_Y_DIR),
                               Y_MIN_PIN, Y_MIN_ENDSTOP_HIT_STATE,
                               int32_t(-1000.0f * SPM), int32_t(at_mm * SPM)) {}
  };

}

/**
 * The surface is where the plane says it is, and the probe closes there.
 *
 * This is the fixture proving itself before anything is tested through it: the probe pin
 * must follow the *surface*, not a fixed height, or every levelling assertion built on it
 * would be measuring a constant. A tilted plane and two points 40 mm apart in X make that
 * visible — the switch closes at a different nozzle height at each, by exactly the tilt.
 *
 * Probing proper is not reached yet. `probe_at_point()` crashes the run in this build, and
 * AddressSanitizer says why: a stale peripheral pointer left behind by an earlier test, not
 * anything to do with probing. See defect register #26 — the fixture below is ready and the
 * tests that use it are blocked on that.
 */
MARLIN_TEST(probe, the_simulated_bed_closes_the_probe_at_the_surface) {
  SimulatedMachine machine;

  XRail x(10.0f); YRail y(50.0f);
  // 0.5 mm of rise across 100 mm in X: a badly levelled machine, and well inside what a
  // planar fit is meant to correct.
  SimulatedBedSurface bed(x, y, SPM, /*height at origin*/ 0.2f, /*tilt X*/ 0.005f, /*tilt Y*/ 0.0f,
                   /*nozzle*/ 5.0f);

  TEST_ASSERT_FALSE_MESSAGE(bed.touching(), "the nozzle starts well clear of the bed");

  // At X10 the surface is at 0.2 + 0.05 mm. Just above it the probe is open; just below,
  // closed — and the assertion is against the plane, not against a remembered number.
  const float here = bed.height_at(10.0f, 50.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, here);

  // One step either side. Asking from closer than that is asking the carriage to be
  // somewhere it cannot be, and the answer would be about rounding rather than about the
  // switch.
  bed.place_nozzle_at(here + bed.step_mm());
  TEST_ASSERT_FALSE_MESSAGE(bed.touching(), "the probe should be open one step above the surface");
  bed.place_nozzle_at(here - bed.step_mm());
  TEST_ASSERT_TRUE_MESSAGE(bed.touching(), "the probe should be closed one step below the surface");
}

/**
 * Moving across the bed moves the surface with it.
 *
 * The whole reason levelling exists. Asserting the *difference* between two places rather
 * than the height at either is what says the surface is a plane and not a constant with an
 * offset baked into the fixture.
 */
MARLIN_TEST(probe, the_surface_height_follows_the_carriage) {
  SimulatedMachine machine;

  XRail x(10.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.2f, 0.005f, 0.003f, 5.0f);

  const float at_10_50 = bed.height_at(10.0f, 50.0f),
              at_50_50 = bed.height_at(50.0f, 50.0f),
              at_10_90 = bed.height_at(10.0f, 90.0f);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.005f * 40.0f, at_50_50 - at_10_50,
    "40 mm of X should raise the surface by 40 times the X tilt");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.003f * 40.0f, at_10_90 - at_10_50,
    "40 mm of Y should raise the surface by 40 times the Y tilt");
}

namespace {
  // A machine standing over the bed, ready to probe, with everything agreeing about where
  // it is. Homing is asserted rather than performed: `probe_at_point()` refuses to move an
  // axis it does not trust, and homing for real is a separate sequence with its own tests.
  void standing_at(const float x, const float y, const float z) {
    xyze_pos_t here = { 0 }; here.x = x; here.y = y; here.z = z;
    motion.position = here;
    planner.set_position_mm(here);
    motion.set_all_homed();
  }
}

/**
 * A probe reports the height of the bed underneath it.
 *
 * Checked against the plane the fixture was given, not against a number from a previous run,
 * so the assertion says the probe measured *the surface* rather than that it repeated
 * itself. The tolerance is a probe's worth of overshoot: the trigger is noticed inside the
 * stepper interrupt, so the axis stops within a few steps of the switch rather than on it.
 */
MARLIN_TEST(probe, a_probe_reports_the_height_of_the_bed_under_it) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, /*at origin*/ 0.2f, /*tilt X*/ 0.005f, /*tilt Y*/ 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const float measured = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_NONE, 0, false);

  TEST_ASSERT_FALSE_MESSAGE(isnan(measured), "the probe never reached the bed");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, bed.height_at(50.0f, 50.0f), measured,
    "the measurement should be the height of the surface at that point");
}

/**
 * Two points on a tilted bed differ by the tilt, and by nothing else.
 *
 * This is the measurement levelling is actually built on: not where the bed is, but how much
 * it changes across the machine. Asserting the *difference* removes the probe's offset and
 * any constant error in the fixture from the comparison, so what is left is the slope — and
 * the slope is what the plane fit will later have to recover.
 */
MARLIN_TEST(probe, the_difference_between_two_points_is_the_tilt) {
  SimulatedMachine machine;
  XRail x(30.0f); YRail y(50.0f);
  constexpr float TILT_X = 0.005f;
  SimulatedBedSurface bed(x, y, SPM, 0.2f, TILT_X, 0.0f, 5.0f);
  standing_at(30.0f, 50.0f, 5.0f);

  const float near_side = probe.probe_at_point(30.0f, 50.0f, PROBE_PT_RAISE, 0, false);
  TEST_ASSERT_FALSE_MESSAGE(isnan(near_side), "the first point did not probe");

  const float far_side = probe.probe_at_point(70.0f, 50.0f, PROBE_PT_RAISE, 0, false);
  TEST_ASSERT_FALSE_MESSAGE(isnan(far_side), "the second point did not probe");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, TILT_X * 40.0f, far_side - near_side,
    "40 mm across a bed tilted by 0.005 should read 0.2 mm higher");
}

/**
 * A flat bed reads the same everywhere.
 *
 * The companion to the test above, and the one that says the difference came from the tilt
 * rather than from the move: same two points, same distance, no slope, no change.
 */
MARLIN_TEST(probe, a_flat_bed_reads_the_same_at_every_point) {
  SimulatedMachine machine;
  XRail x(30.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.3f, 0.0f, 0.0f, 5.0f);
  standing_at(30.0f, 50.0f, 5.0f);

  const float here = probe.probe_at_point(30.0f, 50.0f, PROBE_PT_RAISE, 0, false);
  const float there = probe.probe_at_point(70.0f, 50.0f, PROBE_PT_RAISE, 0, false);

  TEST_ASSERT_FALSE(isnan(here));
  TEST_ASSERT_FALSE(isnan(there));
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 0.0f, there - here,
    "a flat bed should not change across the machine");
}

// A point outside the machine cannot be probed, and the firmware says so rather than
// driving at it.
MARLIN_TEST(probe, a_point_the_probe_cannot_reach_is_refused) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.2f, 0.0f, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const float measured = probe.probe_at_point(X_MAX_POS + 100.0f, 50.0f, PROBE_PT_NONE, 0, false);

  TEST_ASSERT_TRUE_MESSAGE(isnan(measured), "a point off the machine should not be probed");
}

#endif // __PLAT_TEST__ && HAS_BED_PROBE
