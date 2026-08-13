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
 * Arcs given as a radius.
 *
 * `G2`/`G3` accept the arc centre two ways: as `I`/`J`, an offset from where the tool is, or as
 * `R`, a radius — leaving the firmware to work out where the centre must be. The second form is
 * the one with geometry in it, and it had never been executed.
 *
 * Two points and a radius do not determine one arc; they determine four. The centre can sit on
 * either side of the chord, and having chosen a side, the tool can take the short way round or
 * the long way. `G2` versus `G3` picks the direction, and the *sign* of `R` picks short or long.
 *
 * What makes this testable without reaching inside is that the answer is pinned by a property
 * rather than by a number: whatever centre the firmware computes, it must lie exactly `R` from
 * both ends of the chord. That fixes how far the path bulges away from the chord —
 * `R - sqrt(R^2 - half_chord^2)` for the short way — and the bulge is something the machine
 * physically does, so a simulated carriage can measure it.
 *
 * Test-HAL only: these run the arc and wait for it.
 */

#include "src/inc/MarlinConfig.h"

#if ENABLED(ARC_SUPPORT) && HAS_Y_AXIS

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "../gcode/serial_capture.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"

#include <math.h>
#include <string.h>
#include <string>

namespace {

  constexpr float SPM = SimulatedMachine::STEPS_PER_MM;

  // The chord: straight up the middle of the bed, so the arc has room to bulge either way and
  // nothing here is really about the software limits.
  constexpr float CHORD_X = 60.0f, CHORD_Y0 = 40.0f, CHORD_Y1 = 60.0f;
  constexpr float HALF_CHORD = (CHORD_Y1 - CHORD_Y0) * 0.5f;

  std::string host_sends(const char * const line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    SerialCapture capture;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    return capture.finish();
  }

  // How far the centre sits from the chord's midpoint, for a centre that is `r` from both ends.
  // Pythagoras on the half-chord and the radius — the same relation the firmware is solving,
  // written out here so the test states the geometry rather than the firmware's arithmetic.
  float centre_offset_from_midpoint(const float r) {
    const float h2 = r * r - HALF_CHORD * HALF_CHORD;
    return h2 > 0.0f ? sqrtf(h2) : 0.0f;
  }

  struct ArcMachine {
    SimulatedMachine machine;
    SimulatedAxisWithLimit x;

    ArcMachine()
      : x(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR), X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE,
          int32_t(-1000.0f * SPM), int32_t(CHORD_X * SPM)) {
      xyze_pos_t at = motion.position;
      at.x = CHORD_X; at.y = CHORD_Y0; at.z = 10.0f;
      motion.position = at;
      planner.set_position_mm(at);
      motion.destination = at;
      motion.set_all_homed();
      x.place_at(int32_t(CHORD_X * SPM));
    }
    ~ArcMachine() { planner.clear_block_buffer(); }

    // Run an arc to the far end of the chord and report how far the carriage swung to either
    // side of it, in millimetres. Named by axis direction rather than by "left" and "right",
    // which depend on which way you imagine facing.
    struct Bulge { float toward_plus_x, toward_minus_x; };

    Bulge run(const char * const cmd) {
      x.forget_extremes();
      host_sends(cmd);
      TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the arc never finished");
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, CHORD_Y1, motion.position.y,
        "the arc should have ended at the far end of the chord");
      return { float(x.highest_reached()) / SPM - CHORD_X,
               CHORD_X - float(x.lowest_reached()) / SPM };   // both are >= 0
    }
  };

}

/**
 * A radius exactly half the chord is a semicircle, and it bulges by the radius.
 *
 * The degenerate case and the easiest to reason about: the centre can only be the midpoint of
 * the chord, so the path is half a circle and its furthest point is a full radius away. It is
 * also the boundary of what is geometrically possible — a smaller radius could not reach both
 * ends — which is why it is worth having as well as the general case below.
 */
MARLIN_TEST(arcs, an_arc_of_half_the_chord_is_a_semicircle) {
  ArcMachine arc;

  const ArcMachine::Bulge b = arc.run("G2 X60 Y60 R10 F3000");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.3f, HALF_CHORD, b.toward_minus_x,
    "a semicircle should swing out by its full radius");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.3f, 0.0f, b.toward_plus_x,
    "and should not cross to the other side of the chord");
}

/**
 * A larger radius is a flatter arc, by the amount the geometry says.
 *
 * The general case. With the centre `R` from both ends it sits `sqrt(R^2 - half_chord^2)` from
 * the midpoint, so the path bulges by the remainder of the radius. A radius half again as long
 * as the semicircle's flattens the bulge to about a third — a relation no constant or
 * proportional mistake reproduces.
 */
MARLIN_TEST(arcs, a_larger_radius_gives_a_flatter_arc) {
  ArcMachine arc;

  constexpr float R = 15.0f;
  const ArcMachine::Bulge b = arc.run("G2 X60 Y60 R15 F3000");

  const float expected = R - centre_offset_from_midpoint(R);
  TEST_ASSERT_TRUE_MESSAGE(expected < HALF_CHORD * 0.5f,
    "this test needs a radius that flattens the arc well below the semicircle");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.3f, expected, b.toward_minus_x,
    "the bulge should be the radius less the centre's distance from the chord midpoint");
}

/**
 * Reversing the direction of travel puts the arc on the other side.
 *
 * `G2` and `G3` are the same arc taken the other way round, and with the endpoints fixed that
 * means the tool passes on the opposite side of the chord. Without this, every assertion above
 * is equally satisfied by a firmware that ignores the direction — and an arc bulging the wrong
 * way is a gouge through whatever the chord was routed around.
 */
MARLIN_TEST(arcs, the_direction_of_travel_decides_which_side_the_arc_passes) {
  ArcMachine arc;

  const ArcMachine::Bulge clockwise = arc.run("G2 X60 Y60 R15 F3000");

  // Back to the start, and the same arc the other way.
  host_sends("G0 X60 Y40 F6000");
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  const ArcMachine::Bulge counter = arc.run("G3 X60 Y60 R15 F3000");

  TEST_ASSERT_TRUE_MESSAGE(clockwise.toward_minus_x > 1.0f && counter.toward_plus_x > 1.0f,
    "the two directions should bulge to opposite sides of the chord");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.3f, clockwise.toward_minus_x, counter.toward_plus_x,
    "and by the same amount, since it is the same arc taken the other way");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.3f, 0.0f, clockwise.toward_plus_x,
    "neither should stray across the chord");
}

/**
 * A negative radius asks for the long way round.
 *
 * The sign of `R` is not the sign of a distance — it selects which of the two arcs through the
 * same two points is meant. Positive takes the minor arc; negative puts the centre on the other
 * side of the chord and takes the major one, so the tool sweeps almost all the way round and
 * bulges by the radius *plus* the centre's offset instead of minus it.
 *
 * Note it bulges the same way as the minor arc, not the opposite way — the centre moves across
 * the chord and the long way round then comes back over the same side. That was measured rather
 * than assumed; the first version of this test asserted the opposite side and was wrong.
 *
 * Sharing endpoints and radius with the test above, so the only thing that differs is the sign.
 */
MARLIN_TEST(arcs, a_negative_radius_takes_the_long_way_round) {
  ArcMachine arc;

  constexpr float R = 15.0f;
  const ArcMachine::Bulge b = arc.run("G2 X60 Y60 R-15 F3000");

  const float expected = R + centre_offset_from_midpoint(R);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.4f, expected, b.toward_minus_x,
    "the major arc should bulge by the radius plus the centre's offset");
  TEST_ASSERT_TRUE_MESSAGE(expected > 2.0f * (R - centre_offset_from_midpoint(R)),
    "and should be much further out than the minor arc through the same two points");
}

/**
 * An arc with no centre at all is refused.
 *
 * Neither `R` nor `I`/`J` leaves nothing to describe an arc with, and silently doing nothing
 * would leave the tool where it was while the file carried on believing it had moved. The
 * error names the command rather than the machine going quiet.
 */
MARLIN_TEST(arcs, an_arc_with_no_centre_is_refused) {
  ArcMachine arc;

  const std::string said = host_sends("G2 X70 Y50 F3000");

  TEST_ASSERT_TRUE_MESSAGE(said.find(STR_ERR_ARC_ARGS) != std::string::npos,
    "an arc with neither a radius nor an offset should be reported as bad arguments");
  TEST_ASSERT_FALSE_MESSAGE(planner.has_blocks_queued(),
    "and nothing should have been queued for it");
}

#endif // ENABLED(ARC_SUPPORT) && HAS_Y_AXIS
