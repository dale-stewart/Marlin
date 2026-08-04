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
 * Levelling, end to end: measure a tilted bed and then print flat on it.
 *
 * `G29` probes a grid, fits a plane through the readings, and hands the planner a rotation
 * that every later move is put through. The pieces underneath are covered separately — the
 * matrix algebra in `test_vector_3.cpp`, the probe in `test_probe.cpp` — and this is the one
 * test file where they have to agree with each other and with a physical surface.
 *
 * The property is stated once and everything here is a form of it: **the correction the
 * machine applies must equal the error the bed has.** The fixture knows the bed's plane as a
 * formula, so the correction can be predicted at any point without running anything, and the
 * assertion compares two independent things rather than the firmware against itself.
 *
 * That is worth insisting on. A test that probed the bed and then asserted the levelled Z
 * matched what levelling produced would pass with any plane at all, including no plane.
 */

#include "src/inc/MarlinConfig.h"

#if defined(__PLAT_TEST__) && HAS_LEVELING && ABL_PLANAR

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "../support/simulated_bed_surface.h"
#include "../gcode/serial_capture.h"

#include "src/feature/bedlevel/bedlevel.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/probe.h"

#include <math.h>
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

  /**
   * Levelling state outlives the test that measured it, and every move afterwards goes
   * through it — so a plane left behind by one test silently displaces the next one's moves.
   * Reset on the way in as well as out, because a failing test leaves through a `longjmp`
   * that runs no destructor.
   */
  struct LevellingSlate {
    bool was_connected;
    LevellingSlate() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      tidy();
    }
    ~LevellingSlate() { tidy(); MYSERIAL1.host_connected = was_connected; }
    static void tidy() {
      set_bed_leveling_enabled(false);
      planner.bed_level_matrix.set_to_identity();
    }
  };

  void send(const char * const line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  void standing_at(const float x, const float y, const float z) {
    xyze_pos_t here = { 0 }; here.x = x; here.y = y; here.z = z;
    motion.position = here;
    planner.set_position_mm(here);
    motion.set_all_homed();
  }

  std::string reply_to(const char * const line) {
    SerialCapture capture;
    send(line);
    return capture.finish();
  }

  // Pull one of the fitted plane's coefficients out of the "Eqn coefficients" report.
  float coefficient(const std::string &reply, const char * const name) {
    const size_t at = reply.find(name);
    if (at == std::string::npos) return NAN;
    return strtof(reply.c_str() + at + strlen(name), nullptr);
  }

  // What the machine will actually do to a requested Z at this point on the bed.
  float correction_at(const float x, const float y) {
    xyz_pos_t p = { x, y, 0.0f };
    planner.apply_leveling(p);
    return p.z;
  }

}

/**
 * Probing a tilted bed produces a correction that matches the tilt.
 *
 * The correction is compared against the surface the fixture defines, at points the probe
 * never visited, so this says the machine recovered the *plane* rather than the readings. A
 * fit that merely interpolated its own samples would satisfy an assertion made at the probe
 * points and fail here.
 */
MARLIN_TEST(bed_leveling, G29_recovers_the_plane_of_a_tilted_bed) {
  SimulatedMachine machine;
  LevellingSlate slate;

  constexpr float AT_ORIGIN = 0.0f, TILT_X = 0.004f, TILT_Y = -0.002f;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, AT_ORIGIN, TILT_X, TILT_Y, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  send("G29");

  TEST_ASSERT_TRUE_MESSAGE(planner.leveling_active, "G29 should leave levelling switched on");

  // Two points away from the middle, and neither is a grid point. The difference between
  // them is the part that matters: it is the slope, with any constant offset removed.
  const float low = correction_at(40.0f, 60.0f), high = correction_at(80.0f, 60.0f);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, (bed.height_at(80.0f, 60.0f) - bed.height_at(40.0f, 60.0f)),
    high - low, "the correction across X should match the bed's slope across X");

  const float near_y = correction_at(60.0f, 40.0f), far_y = correction_at(60.0f, 80.0f);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, (bed.height_at(60.0f, 80.0f) - bed.height_at(60.0f, 40.0f)),
    far_y - near_y, "the correction across Y should match the bed's slope across Y");
}

/**
 * A flat bed needs no correction, and the machine says so by not moving anything.
 *
 * The base case, and the one that makes the test above mean something: without it, a fit
 * that always reported some slope would still satisfy a difference measured on a tilted bed
 * if the slope happened to be close.
 */
MARLIN_TEST(bed_leveling, G29_on_a_flat_bed_leaves_moves_alone) {
  SimulatedMachine machine;
  LevellingSlate slate;

  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, /*at origin*/ 0.0f, /*flat*/ 0.0f, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  send("G29");
  TEST_ASSERT_TRUE(planner.leveling_active);

  const float a = correction_at(40.0f, 60.0f), b = correction_at(80.0f, 60.0f);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 0.0f, b - a,
    "a flat bed should be corrected by the same amount everywhere, which is no tilt at all");
}

/**
 * The correction is applied to moves, and taken off again exactly.
 *
 * `apply_leveling` converts a requested position into machine coordinates and
 * `unapply_leveling` converts back; the planner uses both, on every move and every position
 * report. If they are not exact inverses the machine's idea of where it is drifts a little
 * each time — invisible in one move, and cumulative over a print.
 *
 * This is the same property `test_vector_3.cpp` asserts of the matrix, checked here against
 * a plane the machine measured for itself rather than one handed to it.
 */
MARLIN_TEST(bed_leveling, applying_and_unapplying_levelling_returns_the_original_point) {
  SimulatedMachine machine;
  LevellingSlate slate;

  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.0f, 0.004f, -0.002f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  send("G29");
  TEST_ASSERT_TRUE(planner.leveling_active);

  const xyz_pos_t start = { 37.5f, 62.5f, 1.75f };
  xyz_pos_t there = start;

  planner.apply_leveling(there);
  TEST_ASSERT_TRUE_MESSAGE(fabsf(there.z - start.z) > 1e-4f,
    "the fixture measured a bed flat enough that the round trip proves nothing");

  planner.unapply_leveling(there);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, start.x, there.x, "X should survive the round trip");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, start.y, there.y, "Y should survive the round trip");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, start.z, there.z, "Z should survive the round trip");
}

/**
 * `M420 S0` puts the correction away without forgetting it, and `M420 S1` brings it back.
 *
 * A host turns levelling off to probe or to move somewhere awkward and expects the measured
 * plane to still be there afterwards. Asserting that the correction is *the same* on either
 * side of the round trip says the plane was preserved, which a flag test alone would not.
 */
MARLIN_TEST(bed_leveling, M420_switches_the_correction_off_and_back_on) {
  SimulatedMachine machine;
  LevellingSlate slate;

  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.0f, 0.004f, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  send("G29");
  const float with_levelling = correction_at(80.0f, 60.0f) - correction_at(40.0f, 60.0f);
  TEST_ASSERT_TRUE_MESSAGE(fabsf(with_levelling) > 1e-3f, "the fixture measured no tilt to switch off");

  send("M420 S0");
  TEST_ASSERT_FALSE_MESSAGE(planner.leveling_active, "M420 S0 should switch levelling off");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.0f,
    correction_at(80.0f, 60.0f) - correction_at(40.0f, 60.0f),
    "with levelling off nothing should be corrected");

  send("M420 S1");
  TEST_ASSERT_TRUE_MESSAGE(planner.leveling_active, "M420 S1 should switch levelling back on");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, with_levelling,
    correction_at(80.0f, 60.0f) - correction_at(40.0f, 60.0f),
    "switching levelling off and on should not change the plane it measured");
}

/**
 * The plane the machine reports is the plane the bed has.
 *
 * `G29 V1` and above print the fit as an equation, and its coefficients are the slopes in X
 * and Y — the same two numbers the fixture was built from. So this compares the firmware's
 * own statement of what it measured against the surface that produced it, which is a
 * stronger claim than the corrections agreeing: a fit could be self-consistently wrong and
 * still correct moves consistently with itself.
 *
 * The tolerance is derived rather than chosen. Every probe reading is quantised to a whole
 * step, so a slope recovered from readings across a span can be wrong by about one step over
 * that span; anything tighter would be asserting a precision the machine does not have.
 */
MARLIN_TEST(bed_leveling, the_reported_plane_equation_matches_the_bed) {
  SimulatedMachine machine;
  LevellingSlate slate;

  constexpr float TILT_X = 0.004f, TILT_Y = -0.002f;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.0f, TILT_X, TILT_Y, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const std::string reply = reply_to("G29 V1");

  TEST_ASSERT_TRUE_MESSAGE(reply.find("Eqn coefficients") != std::string::npos,
    "G29 V1 should report the plane it fitted");

  // One step across the probed span, which is the grid's width.
  const float span = float(X_BED_SIZE) - 2.0f * (PROBING_MARGIN);
  const float slope_tolerance = (1.0f / SPM) / span * 4.0f;

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(slope_tolerance, TILT_X, coefficient(reply, "a: "),
    "the fitted X slope should be the bed's X slope");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(slope_tolerance, TILT_Y, coefficient(reply, "b: "),
    "the fitted Y slope should be the bed's Y slope");
}

/**
 * A more talkative run measures the same bed.
 *
 * `V4` turns on the topography map and the extra reports on top of it. The output is not
 * asserted line by line — it is a diagnostic aid, not an interface — but the plane it reports
 * has to be the same one a quiet run finds, or the reporting is changing the measurement.
 */
MARLIN_TEST(bed_leveling, a_verbose_run_measures_what_a_quiet_one_does) {
  SimulatedMachine machine;
  LevellingSlate slate;

  constexpr float TILT_X = 0.004f;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.0f, TILT_X, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const std::string quiet = reply_to("G29 V1");
  const float quiet_a = coefficient(quiet, "a: ");

  LevellingSlate::tidy();
  standing_at(50.0f, 50.0f, 5.0f);
  const std::string loud = reply_to("G29 V4");

  TEST_ASSERT_TRUE_MESSAGE(loud.find("Bed Height Topography") != std::string::npos,
    "V4 should include the topography map");
  TEST_ASSERT_TRUE_MESSAGE(loud.length() > quiet.length(),
    "a more verbose run should say more");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, quiet_a, coefficient(loud, "a: "),
    "how much the run says should not change what it measures");
}

/**
 * A dry run measures the bed and changes nothing.
 *
 * `G29 D` exists so an operator can see what the bed looks like without committing to it —
 * so it has to probe, report, and leave the machine exactly as it was. Asserting both halves
 * is the point: a dry run that skipped probing would also leave the machine alone.
 */
MARLIN_TEST(bed_leveling, a_dry_run_reports_the_bed_without_changing_anything) {
  SimulatedMachine machine;
  LevellingSlate slate;

  constexpr float TILT_X = 0.004f;
  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.0f, TILT_X, 0.0f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const std::string reply = reply_to("G29 D V1");

  // It probed: the plane it reports is the bed's.
  const float span = float(X_BED_SIZE) - 2.0f * (PROBING_MARGIN);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE((1.0f / SPM) / span * 4.0f, TILT_X, coefficient(reply, "a: "),
    "a dry run should still measure the bed");

  // ...and it committed nothing.
  TEST_ASSERT_FALSE_MESSAGE(planner.leveling_active, "a dry run should not switch levelling on");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 0.0f, correction_at(80.0f, 60.0f) - correction_at(40.0f, 60.0f),
    "a dry run should leave moves uncorrected");
}

/**
 * Every probed point lies on the plane the machine fitted.
 *
 * The corrected topography table prints, for each probe point, how far it sits from the
 * fitted plane. The bed here *is* a plane, so every one of those residuals has to be zero —
 * not approximately similar to each other, zero — and any of them being large means the fit
 * missed a point that the corrections elsewhere would then be wrong about.
 *
 * It is the strongest statement available about the fit, and it costs nothing extra: the
 * firmware already computes and prints exactly this. Reading a diagnostic table for a
 * property rather than for its layout also keeps the test from pinning the formatting, which
 * is not behaviour anybody depends on.
 */
MARLIN_TEST(bed_leveling, every_probed_point_lies_on_the_fitted_plane) {
  SimulatedMachine machine;
  LevellingSlate slate;

  XRail x(50.0f); YRail y(50.0f);
  SimulatedBedSurface bed(x, y, SPM, 0.0f, 0.004f, -0.002f, 5.0f);
  standing_at(50.0f, 50.0f, 5.0f);

  const std::string reply = reply_to("G29 V4");

  const size_t at = reply.find("Corrected Bed Height vs. Bed Topology:");
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, "V4 should print the corrected topography");

  // Every signed number in the table that follows, until the next titled section.
  const size_t end = reply.find("\n\n", at + 40);
  const std::string table = reply.substr(at, (end == std::string::npos ? reply.size() : end) - at);

  // Two steps: each reading is quantised to one, and the residual is a difference of two.
  const float tolerance = 2.0f / SPM;
  uint8_t residuals = 0;
  for (size_t i = 0; i < table.size(); ++i) {
    if (table[i] != '+' && table[i] != '-') continue;
    const float residual = strtof(table.c_str() + i, nullptr);
    ++residuals;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(tolerance, 0.0f, residual,
      "a probed point sits off the plane the machine fitted to it");
  }

  TEST_ASSERT_EQUAL_MESSAGE(GRID_MAX_POINTS_X * GRID_MAX_POINTS_Y, residuals,
    "the table should hold one residual per probe point");
}

#endif // __PLAT_TEST__ && HAS_LEVELING && ABL_PLANAR
