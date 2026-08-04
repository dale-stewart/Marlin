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

#include "src/feature/bedlevel/bedlevel.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/probe.h"

#include <math.h>
#include <string.h>

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

#endif // __PLAT_TEST__ && HAS_LEVELING && ABL_PLANAR
