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

#if HAS_BED_PROBE

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "../support/simulated_bed_surface.h"
#include "../gcode/serial_capture.h"
#include "src/module/probe.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/endstops.h"
#include "src/module/stepper.h"

#include <string.h>
#include <stdlib.h>
#include <string>

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

/**
 * A probe that is told to stop above the bed stops, and says it found nothing.
 *
 * `z_min_point` is how far below the expected trigger the probe may go before giving up,
 * and it exists to stop a probe that never triggers from driving the nozzle into the bed.
 * The pair below is the boundary either side of a known surface: the same machine, the same
 * bed, and the only difference is whether the limit is above the bed or below it.
 *
 * Stated that way the assertion is derivable — the surface height comes from the fixture's
 * formula, and the two limits are placed relative to *it* rather than being numbers that
 * happened to work.
 */
MARLIN_TEST(probe, a_probe_stopped_above_the_bed_finds_nothing) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.2f, 0.0f, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const float surface = bed.height_at(50.0f, 50.0f);

  // Stop half a millimetre short of the bed: nothing to find, and nothing found.
  const float stopped_short = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false,
                                                   true, surface + 0.5f);
  TEST_ASSERT_TRUE_MESSAGE(isnan(stopped_short),
    "a probe forbidden from reaching the bed should report failure, not a height");

  // Let it go half a millimetre past, and the same machine measures the same bed it always did.
  const float allowed = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false,
                                             true, surface - 0.5f);
  TEST_ASSERT_FALSE_MESSAGE(isnan(allowed), "the probe should reach a bed it is allowed to reach");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, surface, allowed,
    "the measurement should still be the height of the surface");
}

/**
 * A probe that does not know where Z is gets ten millimetres more rope.
 *
 * The limit above is measured down from where the trigger is *expected*, which only means
 * anything once Z has been homed. Before that the firmware has no idea how far away the bed
 * is, so it allows a further 10 mm before giving up — and that allowance is the whole reason
 * homing Z, which is itself a probe taken with Z untrusted, can find the bed at all.
 *
 * The pair is the same machine and the same limit, differing only in whether Z is trusted:
 * the limit is deliberately set above the bed, so the trusted run must fail and the untrusted
 * run can only succeed by spending the extra allowance.
 */
MARLIN_TEST(probe, an_untrusted_z_is_allowed_to_probe_deeper) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.2f, 0.0f, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const float surface = bed.height_at(50.0f, 50.0f);
  constexpr float ALLOWANCE = 10.0f;

  // Half a millimetre short of the bed: out of reach for a machine that trusts its Z.
  const float trusted = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false,
                                             true, surface + 0.5f);
  TEST_ASSERT_TRUE_MESSAGE(isnan(trusted),
    "with Z trusted the probe should stop at the limit and report failure");

  // Untrusted, the same machine reaches a limit set most of the allowance above the bed —
  // and still stops at one set just past it. Bracketing the allowance is what pins it to ten
  // millimetres; "further than before" would be satisfied by any number at all.
  motion.set_axis_untrusted(Z_AXIS);
  const float within = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false,
                                            true, surface + ALLOWANCE - 0.5f);
  const float beyond = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false,
                                            true, surface + ALLOWANCE + 0.5f);
  motion.set_axis_trusted(Z_AXIS);

  TEST_ASSERT_FALSE_MESSAGE(isnan(within),
    "an untrusted Z should spend its allowance to reach a bed just inside it");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, surface, within,
    "and what it finds should still be the surface");
  TEST_ASSERT_TRUE_MESSAGE(isnan(beyond),
    "the allowance is ten millimetres, not an excuse to probe indefinitely");
}

/**
 * Failing to find the bed is reported to the host, not swallowed.
 *
 * The operator has to know: a levelling run that quietly treats a missed probe as a
 * measurement fits a plane through a number that means nothing. Asserting the message rather
 * than only the NAN is what says the failure left the firmware.
 */
MARLIN_TEST(probe, a_failed_probe_tells_the_host) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.2f, 0.0f, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const float surface = bed.height_at(50.0f, 50.0f);

  SerialCapture failed;
  const float missed = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false,
                                            true, surface + 0.5f);
  const std::string on_failure = failed.finish();
  TEST_ASSERT_TRUE(isnan(missed));
  // "Error:", not merely the words. The same text also goes to the LCD status line, which
  // this build echoes to the host — so an assertion on the words alone passes with the error
  // report deleted, and a mutation run says exactly that.
  TEST_ASSERT_TRUE_MESSAGE(on_failure.find("Error:" STR_ERR_PROBING_FAILED) != std::string::npos,
    "a probe that never reached the bed should report an error, not just a status message");

  SerialCapture worked;
  const float found = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false,
                                           true, surface - 0.5f);
  const std::string on_success = worked.finish();
  TEST_ASSERT_FALSE(isnan(found));
  TEST_ASSERT_TRUE_MESSAGE(on_success.find(STR_ERR_PROBING_FAILED) == std::string::npos,
    "a probe that measured the bed should not report a failure by any route");
}

/**
 * A probe that triggers far too high is a fault, not a measurement.
 *
 * A trigger well above where the bed can be means something is wrong — a probe left
 * deployed, a wire shorted, a bed clip under the nozzle — and treating it as a reading is
 * how a levelling run ends up correcting for damage it is about to cause. `Z_PROBE_ERROR_TOLERANCE`
 * is how high is too high, and `sanity_check` is whether anyone is looking.
 *
 * The bed here is placed *just past* the tolerance rather than at some safely large height,
 * so the assertion is about the limit and not merely about a big number. The companion with
 * the check switched off is what says the rejection came from the check.
 */
MARLIN_TEST(probe, a_trigger_far_above_the_bed_is_rejected) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  // Beyond the tolerance by half a millimetre. `zoffs` is zero for a probe with no Z offset,
  // so the limit is the tolerance itself.
  constexpr float TOO_HIGH = Z_PROBE_ERROR_TOLERANCE + 0.5f;
  SimulatedBedSurface bed(x, y, SPM, TOO_HIGH, 0.0f, 0.0f, TOO_HIGH + 5.0f);
  standing_at(50.0f, 50.0f, TOO_HIGH + 5.0f);

  const float checked = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false, true);
  TEST_ASSERT_TRUE_MESSAGE(isnan(checked),
    "a trigger past the error tolerance should be refused as a fault");

  // The same trigger, with nobody checking, is returned as an ordinary measurement — which
  // is what says the refusal above came from the sanity check rather than from the probe
  // failing to trigger at all.
  const float unchecked = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false, false);
  TEST_ASSERT_FALSE_MESSAGE(isnan(unchecked), "with the check off the trigger is a measurement");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, TOO_HIGH, unchecked,
    "and the measurement is where the surface is");
}

/**
 * A trigger inside the tolerance is an ordinary measurement.
 *
 * The other side of the boundary, and the reason the test above cannot pass by rejecting
 * everything: the same sanity check, the same machine, a bed half a millimetre the *right*
 * side of the limit, and the reading is accepted.
 */
MARLIN_TEST(probe, a_trigger_within_the_tolerance_is_accepted) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  constexpr float HIGH_ENOUGH = Z_PROBE_ERROR_TOLERANCE - 0.5f;
  SimulatedBedSurface bed(x, y, SPM, HIGH_ENOUGH, 0.0f, 0.0f, HIGH_ENOUGH + 5.0f);
  standing_at(50.0f, 50.0f, HIGH_ENOUGH + 5.0f);

  const float measured = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false, true);

  TEST_ASSERT_FALSE_MESSAGE(isnan(measured), "a trigger inside the tolerance is not a fault");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, HIGH_ENOUGH, measured,
    "and it reads as the height of the surface");
}

/**
 * The "too high" limit is measured from the probe's trigger point, not from zero.
 *
 * A probe mounted below the nozzle triggers at a Z the nozzle never reaches, so the height
 * that counts as suspicious has to move with it — otherwise fitting a probe with any Z offset
 * would make every ordinary measurement look like a fault. The tests above all run with no
 * offset, which is the one case where "from the trigger point" and "from zero" agree.
 *
 * Both beds here are above the unoffset limit and either side of the offset one, so the pair
 * can only pass if the limit moved by the offset and in the right direction.
 */
MARLIN_TEST(probe, the_error_tolerance_moves_with_the_probe_offset) {
  constexpr float OFFSET_Z = -1.5f;                       // trigger point below the nozzle
  constexpr float LIMIT = -OFFSET_Z + Z_PROBE_ERROR_TOLERANCE;
  float inside = 0.0f, outside = 0.0f;

  // Both beds sit above Z_PROBE_ERROR_TOLERANCE, so without the offset both are faults.
  TEST_ASSERT_TRUE_MESSAGE(LIMIT - 0.5f > Z_PROBE_ERROR_TOLERANCE,
    "the accepted bed must be one an unoffset probe would have refused");

  {
    SimulatedMachine machine;
    XRail x(50.0f); YRail y(50.0f);
    SimulatedBedSurface bed(x, y, SPM, LIMIT - 0.5f, 0.0f, 0.0f, LIMIT + 4.0f);
    standing_at(50.0f, 50.0f, LIMIT + 4.0f);
    const float was = probe.offset.z;
    probe.offset.z = OFFSET_Z;
    inside = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_NONE, 0, false, true);
    probe.offset.z = was;
  }
  {
    SimulatedMachine machine;
    XRail x(50.0f); YRail y(50.0f);
    SimulatedBedSurface bed(x, y, SPM, LIMIT + 0.5f, 0.0f, 0.0f, LIMIT + 4.0f);
    standing_at(50.0f, 50.0f, LIMIT + 4.0f);
    const float was = probe.offset.z;
    probe.offset.z = OFFSET_Z;
    outside = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_NONE, 0, false, true);
    probe.offset.z = was;
  }

  TEST_ASSERT_FALSE_MESSAGE(isnan(inside),
    "a trigger inside the offset limit is an ordinary measurement");
  TEST_ASSERT_TRUE_MESSAGE(isnan(outside),
    "a trigger past the offset limit is still a fault");
}

/**
 * A verbose probe says where it measured, and what it read.
 *
 * The report is the only thing an operator watching a levelling run actually sees, and it is
 * the firmware's own statement of what it just did. Asserting it against the *surface* —
 * rather than against the value the same call returned — is what makes it a check on the
 * measurement rather than a check that a number was printed twice.
 *
 * The quiet run underneath is the boundary: verbosity has to be a threshold, or the report
 * is either always on or never on, and neither would be noticed by an assertion on one level.
 */
MARLIN_TEST(probe, a_verbose_probe_reports_the_point_and_the_reading) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.3f, 0.004f, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  SerialCapture capture;
  const float measured = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 3, false);
  const std::string reply = capture.finish();

  TEST_ASSERT_FALSE_MESSAGE(isnan(measured), "the probe never reached the bed");

  const size_t at = reply.find("Bed X:");
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, "a verbose probe should report the point");

  // Read the three numbers back out of the report and check each against what the fixture
  // knows independently: the point asked for, and the height of the surface there.
  const char *p = reply.c_str() + at + strlen("Bed X:");
  char *end = nullptr;
  const float rx = strtof(p, &end);
  const float ry = strtof(strstr(end, "y:") + 2, &end);
  const float rz = strtof(strstr(end, "Z:") + 2, &end);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 50.0f, rx, "the report should name the X it probed");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 50.0f, ry, "the report should name the Y it probed");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, bed.height_at(50.0f, 50.0f), rz,
    "the reported height should be the height of the surface at that point");
}

MARLIN_TEST(probe, a_probe_below_the_verbose_threshold_says_nothing) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.3f, 0.0f, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  // Level 2 is the last quiet one, so probing there is what pins the threshold from below.
  // Silence at level 0 as well, because a threshold pinned only at 2 is still free to become
  // "every level except 2" — which is one character's difference in the source.
  for (const uint8_t level : { uint8_t(0), uint8_t(2) }) {
    SerialCapture capture;
    const float measured = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, level, false);
    const std::string reply = capture.finish();

    TEST_ASSERT_FALSE(isnan(measured));
    char why[80];
    snprintf(why, sizeof(why), "verbose level %u should not report the point", unsigned(level));
    TEST_ASSERT_TRUE_MESSAGE(reply.find("Bed X:") == std::string::npos, why);
  }
}

/**
 * A probe that found nothing is left where it stopped, not raised as though it had.
 *
 * The raise after a probe is part of measuring one — clearance for the move to the next
 * point. A failed probe has no next point until someone deals with the failure, and lifting
 * the carriage anyway would report the machine as being somewhere it was never commanded to,
 * on the strength of a measurement that does not exist.
 */
MARLIN_TEST(probe, a_failed_probe_is_not_raised_afterwards) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.2f, 0.0f, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const float stopped_at = bed.height_at(50.0f, 50.0f) + 0.5f;
  constexpr float CLEARANCE = 4.0f;

  const float measured = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false,
                                              true, stopped_at, CLEARANCE);

  TEST_ASSERT_TRUE_MESSAGE(isnan(measured), "this probe is meant to find nothing");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, stopped_at, motion.position.z,
    "a failed probe should be left at the limit it stopped at, not at the clearance height");
}

/**
 * Raising after a probe is a height or a distance, and the two are not the same.
 *
 * `raise_after_is_rel` chooses between "go to this Z" and "go up by this much", which differ
 * by exactly where the probe stopped — so the assertion is the difference between the two
 * runs, predicted from the bed the fixture built.
 */
MARLIN_TEST(probe, raising_after_a_probe_is_absolute_or_relative) {
  constexpr float CLEARANCE = 3.0f;
  float surface = 0.0f, after_absolute = 0.0f, after_relative = 0.0f;

  // Each run gets its own machine. `standing_at()` tells the firmware where it is without
  // moving the carriage, so using it a second time inside one test would leave the two out
  // of register by however far the previous raise had lifted — and the second probe would
  // then trigger at a Z that is real, consistent, and about the wrong thing.
  {
    SimulatedMachine machine;
    XRail x(50.0f); YRail y(50.0f);
    SimulatedBedSurface bed(x, y, SPM, 0.6f, 0.0f, 0.0f, 5.0f);
    standing_at(50.0f, 50.0f, 5.0f);
    surface = bed.height_at(50.0f, 50.0f);

    const float measured = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false, true,
                                                Z_PROBE_LOW_POINT, CLEARANCE, /*relative*/ false);
    TEST_ASSERT_FALSE_MESSAGE(isnan(measured), "the absolute run did not reach the bed");
    after_absolute = motion.position.z;
  }
  {
    SimulatedMachine machine;
    XRail x(50.0f); YRail y(50.0f);
    SimulatedBedSurface bed(x, y, SPM, 0.6f, 0.0f, 0.0f, 5.0f);
    standing_at(50.0f, 50.0f, 5.0f);

    const float measured = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_RAISE, 0, false, true,
                                                Z_PROBE_LOW_POINT, CLEARANCE, /*relative*/ true);
    TEST_ASSERT_FALSE_MESSAGE(isnan(measured), "the relative run did not reach the bed");
    after_relative = motion.position.z;
  }

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, CLEARANCE, after_absolute,
    "an absolute raise should end at the clearance height");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, surface + CLEARANCE, after_relative,
    "a relative raise should end the clearance above where the probe triggered");
}

namespace {
  // How long a probe from a given height takes, in simulated seconds. Time under the test
  // HAL advances because the machine is waiting, so a blocking move costs exactly what it
  // would cost the machine.
  float seconds_to_probe_from(const float start_z) {
    SimulatedMachine machine;
    XRail x(50.0f); YRail y(50.0f);
    SimulatedBedSurface bed(x, y, SPM, 0.2f, 0.0f, 0.0f, start_z);
    standing_at(50.0f, 50.0f, start_z);

    // Deployment is global state that survives this fixture, so the first timed probe would
    // otherwise pay for a deploy the second one skips — and that difference lands in exactly
    // the quantity being measured.
    probe.deploy();

    const millis_t began = millis();
    const float measured = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_NONE, 0, false);
    const millis_t took = millis() - began;

    TEST_ASSERT_FALSE_MESSAGE(isnan(measured), "the timed probe never reached the bed");
    return float(took) / 1000.0f;
  }
}

/**
 * A probe starting well above the bed drops quickly before it starts probing.
 *
 * Probing has to be slow to be accurate, and a slow descent from travel height is most of
 * what a levelling run costs. So above a threshold the firmware covers the distance at the
 * fast feedrate first and only probes slowly from there — which changes nothing about the
 * measurement, and is the entire reason the branch exists. Timing is therefore the only
 * thing that can say whether it works.
 *
 * Rather than predict a whole probe, this measures the *kink*: descent time as a function of
 * starting height has one slope above the threshold and a steeper one below it. Two starts
 * either side of it — one millimetre each way — differ by one millimetre at each feedrate,
 * and nothing else. Everything the two runs share (the approach, the trigger, the raise
 * afterwards) cancels in the difference, so nothing here depends on modelling a probe.
 *
 * That also makes the assertion locate the threshold rather than merely notice one: had it
 * been a millimetre out in either direction, both starts would fall on the same side of it
 * and the difference would be two millimetres at a single feedrate — 0.5 s or 1.0 s, either
 * side of the 0.75 s this asserts.
 */
MARLIN_TEST(probe, a_probe_from_high_up_covers_the_first_part_quickly) {
  // The threshold as the configuration states it. `zoffs` is the probe's Z offset negated,
  // which is zero here — asserted rather than assumed, because a non-zero offset would move
  // the threshold and leave this test measuring the wrong millimetre.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, probe.offset.z,
    "this test places its starts around a threshold that a probe Z offset would move");
  constexpr float threshold = (Z_CLEARANCE_DEPLOY_PROBE) + 5.0f;

  const float above = seconds_to_probe_from(threshold + 1.0f),
              below = seconds_to_probe_from(threshold - 1.0f);

  const float fast = motion.z_probe_fast_mm_s, slow = motion.z_probe_slow_mm_s;
  TEST_ASSERT_TRUE_MESSAGE(fast > slow, "the fast probe feedrate must be the faster one");

  // Two millimetres separate the starts, so the difference is bounded by the cost of two
  // millimetres at each feedrate. Landing strictly between them is the whole claim: one of
  // the two was covered quickly and the other was not, which can only happen if the
  // threshold lies between the two starting heights.
  //
  // Bounds rather than the exact 1/fast + 1/slow, because the faster run plans one move more
  // than the slower one and a move costs a fixed amount of waiting on top of its travel. That
  // overhead is a property of the machine, not of the threshold, and it has no business in an
  // assertion about where the threshold is.
  const float two_mm_fast = 2.0f / fast, two_mm_slow = 2.0f / slow;
  const float margin = (two_mm_slow - two_mm_fast) * 0.1f;
  const float measured = above - below;

  TEST_ASSERT_TRUE_MESSAGE(measured > two_mm_fast + margin,
    "the millimetre below the threshold should be probed slowly, not covered at speed");
  TEST_ASSERT_TRUE_MESSAGE(measured < two_mm_slow - margin,
    "the millimetre above the threshold should be covered at speed, not probed slowly");
}

/**
 * A probe mounted below the nozzle reports a bed that much lower.
 *
 * `NOZZLE_TO_PROBE_OFFSET` is how far the trigger point sits from the nozzle tip, and every
 * measurement is corrected by it — because what levelling needs to know is where the *nozzle*
 * must be, not where the switch closed. The default configuration here has no Z offset at
 * all, which makes the correction invisible: a probe that ignored the offset entirely would
 * measure exactly the same bed.
 *
 * The assertion is the difference between two runs over the same surface, so it is the
 * correction itself that is checked and not the height it was applied to.
 */
MARLIN_TEST(probe, a_probe_offset_in_z_moves_every_measurement_by_it) {
  constexpr float OFFSET_Z = -1.5f;   // the trigger point sits 1.5 mm below the nozzle tip
  float without = 0.0f, with = 0.0f;

  {
    SimulatedMachine machine;
    XRail x(50.0f); YRail y(50.0f);
    SimulatedBedSurface bed(x, y, SPM, 0.4f, 0.0f, 0.0f, 5.0f);
    standing_at(50.0f, 50.0f, 5.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, probe.offset.z, "expected no Z offset to start from");
    without = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_NONE, 0, false);
    TEST_ASSERT_FALSE_MESSAGE(isnan(without), "the unoffset run did not reach the bed");
  }
  {
    SimulatedMachine machine;
    XRail x(50.0f); YRail y(50.0f);
    SimulatedBedSurface bed(x, y, SPM, 0.4f, 0.0f, 0.0f, 5.0f);
    standing_at(50.0f, 50.0f, 5.0f);

    const float was = probe.offset.z;
    probe.offset.z = OFFSET_Z;
    with = probe.probe_at_point(50.0f, 50.0f, PROBE_PT_NONE, 0, false);
    probe.offset.z = was;

    TEST_ASSERT_FALSE_MESSAGE(isnan(with), "the offset run did not reach the bed");
  }

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, OFFSET_Z, with - without,
    "the same bed, measured through a probe offset, should read lower by exactly the offset");
}

/**
 * Asking to probe *at* a point puts the probe there, not the nozzle.
 *
 * This is what `probe_relative` means, and it is the default — a levelling grid names points
 * on the bed, and the probe is what has to reach them, so the carriage goes to the point
 * minus the probe's XY offset. Every test above turns it off, which is why the correction has
 * never been exercised.
 *
 * A tilted bed is what makes it visible: on a flat one, probing 10 mm from where you meant to
 * reads the same, and the test would pass with the offset applied, ignored, or doubled.
 */
MARLIN_TEST(probe, probing_relative_to_the_probe_offsets_the_carriage) {
  constexpr float TILT_X = 0.006f, TILT_Y = 0.004f;
  float as_asked = 0.0f, at_the_nozzle = 0.0f;

  // Ask for the probe to be at (60, 60). The nozzle should end up an offset short of it.
  {
    SimulatedMachine machine;
    XRail x(60.0f); YRail y(60.0f);
    SimulatedBedSurface bed(x, y, SPM, 0.3f, TILT_X, TILT_Y, 5.0f);
    standing_at(60.0f, 60.0f, 5.0f);
    as_asked = probe.probe_at_point(60.0f, 60.0f, PROBE_PT_NONE, 0, /*probe_relative*/ true);
    TEST_ASSERT_FALSE_MESSAGE(isnan(as_asked), "the relative run did not reach the bed");
  }

  // The same measurement taken by putting the nozzle where that should have sent it.
  const xy_pos_t nozzle_at = { 60.0f - probe.offset_xy.x, 60.0f - probe.offset_xy.y };
  {
    SimulatedMachine machine;
    XRail x(nozzle_at.x); YRail y(nozzle_at.y);
    SimulatedBedSurface bed(x, y, SPM, 0.3f, TILT_X, TILT_Y, 5.0f);
    standing_at(nozzle_at.x, nozzle_at.y, 5.0f);
    at_the_nozzle = probe.probe_at_point(nozzle_at.x, nozzle_at.y, PROBE_PT_NONE, 0, false);
    TEST_ASSERT_FALSE_MESSAGE(isnan(at_the_nozzle), "the absolute run did not reach the bed");
  }

  // The offset has to be worth measuring, or the two runs would agree for the wrong reason.
  const float slope_apart = TILT_X * probe.offset_xy.x + TILT_Y * probe.offset_xy.y;
  TEST_ASSERT_TRUE_MESSAGE(fabsf(slope_apart) > 0.05f,
    "the probe offset must cross enough tilt for the two points to differ");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, at_the_nozzle, as_asked,
    "probing relative to the probe should measure the bed under the offset carriage");
}

/**
 * Clearance is made for whichever of the two hangs lower.
 *
 * A clearance height is asked for so that something clears the bed, and when the probe is
 * deployed the thing that has to clear it is the *probe* — which, mounted below the nozzle,
 * reaches the bed first. So a probe with a negative Z offset raises the nozzle further by
 * exactly that offset, and a probe level with the nozzle changes nothing.
 *
 * Asserted as the difference between two runs on the same machine, so what is checked is the
 * allowance itself rather than the height it was added to. The `with_probe` companion says
 * the allowance is a decision about the probe rather than a constant: the same offset, with
 * the caller stating that the probe is not what needs the room, has no effect.
 */
MARLIN_TEST(probe, a_probe_below_the_nozzle_gets_extra_clearance) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.2f, 0.0f, 0.0f, 0.5f);
  standing_at(50.0f, 50.0f, 0.5f);

  constexpr float ASKED_FOR = 8.0f, OFFSET_Z = -2.0f;
  const float was = probe.offset.z;

  probe.offset.z = 0.0f;
  motion.do_z_clearance(ASKED_FOR);
  const float level_probe = motion.position.z;

  standing_at(50.0f, 50.0f, 0.5f);
  probe.offset.z = OFFSET_Z;
  motion.do_z_clearance(ASKED_FOR);
  const float low_probe = motion.position.z;

  standing_at(50.0f, 50.0f, 0.5f);
  motion.do_z_clearance(ASKED_FOR, /*with_probe*/ false);
  const float not_for_the_probe = motion.position.z;

  probe.offset.z = was;

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, ASKED_FOR, level_probe,
    "a probe level with the nozzle needs no allowance");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, -OFFSET_Z, low_probe - level_probe,
    "a probe two millimetres below the nozzle should be given two more millimetres");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, ASKED_FOR, not_for_the_probe,
    "clearance that is not for the probe should not be padded for it");
}

/**
 * Deploying a probe that is already deployed does nothing at all.
 *
 * Deploy raises Z to make room for the mechanism, and every probe point in a levelling run
 * asks for a deploy — so a deploy that did its work unconditionally would lift the carriage
 * to the deploy clearance before each of them. The state check is what makes the request
 * idempotent, and the raise is what makes the difference visible.
 */
MARLIN_TEST(probe, deploying_an_already_deployed_probe_does_nothing) {
  SimulatedMachine machine;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.2f, 0.0f, 0.0f, 1.0f);
  standing_at(50.0f, 50.0f, 1.0f);

  // Deployment is machine state, not test state: a probe left deployed by an earlier test is
  // still deployed here, and asking for a deploy would then be the *second* one. Start from a
  // known side of the state rather than from whatever the suite happened to leave.
  probe.stow();
  TEST_ASSERT_FALSE_MESSAGE(endstops.z_probe_enabled, "the probe should be stowed to begin with");
  motion.do_z_clearance(1.0f, true, true);

  TEST_ASSERT_FALSE_MESSAGE(probe.deploy(), "deploying should succeed");
  TEST_ASSERT_TRUE_MESSAGE(endstops.z_probe_enabled, "and should leave the probe deployed");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, Z_CLEARANCE_DEPLOY_PROBE, motion.position.z,
    "the first deploy should raise to the deploy clearance");

  // Back down, and ask again. Nothing to do, so nothing moves. (A clearance move only ever
  // goes up unless lowering is explicitly allowed — this is the test putting the carriage
  // back, not the firmware descending on its own.)
  motion.do_z_clearance(1.0f, true, true);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, motion.position.z);

  TEST_ASSERT_FALSE_MESSAGE(probe.deploy(), "deploying an already-deployed probe should succeed");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 1.0f, motion.position.z,
    "a second deploy should not raise the carriage again");

  probe.stow();
}

#endif // HAS_BED_PROBE
