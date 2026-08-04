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
#include "../support/simulated_bed.h"


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
  SimulatedBed bed(x, y, SPM, /*height at origin*/ 0.2f, /*tilt X*/ 0.005f, /*tilt Y*/ 0.0f,
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
  SimulatedBed bed(x, y, SPM, 0.2f, 0.005f, 0.003f, 5.0f);

  const float at_10_50 = bed.height_at(10.0f, 50.0f),
              at_50_50 = bed.height_at(50.0f, 50.0f),
              at_10_90 = bed.height_at(10.0f, 90.0f);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.005f * 40.0f, at_50_50 - at_10_50,
    "40 mm of X should raise the surface by 40 times the X tilt");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.003f * 40.0f, at_10_90 - at_10_50,
    "40 mm of Y should raise the surface by 40 times the Y tilt");
}

#endif // __PLAT_TEST__ && HAS_BED_PROBE
