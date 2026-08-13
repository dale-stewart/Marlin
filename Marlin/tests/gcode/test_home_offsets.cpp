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
 * The home offset: M206 sets it directly, M428 measures it.
 *
 * The home offset is a permanent shift of the whole coordinate system, applied at the
 * moment homing re-references the machine to its switches. Everything printed afterwards
 * sits where it puts the origin, so getting it wrong displaces every print — and it does
 * so silently, because the machine reports the coordinates it believes rather than the
 * ones it is at.
 *
 * That is also why the interesting assertion is physical rather than arithmetic. The
 * offset only takes effect at the next home (`Motion::homeaxis` sets
 * `position = base_home_pos + home_offset`), so checking the stored number says nothing
 * about whether the machine goes anywhere different. `M428_makes_the_spot_it_is_at_read_as_the_origin`
 * therefore drives the carriage over a simulated rail, calls M428, homes again, and asks
 * for the origin back — the assertion is that the tool physically returns to the spot the
 * operator called home. The rail counts pulses on the step pin and is not re-referenced by
 * homing, so it can see a movement that the firmware's own counters cannot.
 *
 * M428 refuses more than 20 mm from a reference point, which is a guard against an
 * operator who has jogged somewhere unrelated and would otherwise shift the origin across
 * the bed. A magnitude needs bracketing from both sides, so it is probed either side of
 * the limit rather than shown to refuse once.
 */

#include "../test/unit_tests.h"
#include "serial_capture.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include <string.h>

#if HAS_HOME_OFFSET

namespace {

  constexpr float SPM = SimulatedMachine::STEPS_PER_MM;

  // A carriage on the X rail with its limit switch at the minimum end.
  struct XRail : SimulatedAxisWithLimit {
    XRail(const float switch_at_mm, const float carriage_at_mm)
      : SimulatedAxisWithLimit(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                               X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE,
                               int32_t(switch_at_mm * SPM), int32_t(carriage_at_mm * SPM)) {}
    float mm() const { return float(position()) / SPM; }
  };

  // The home offset is machine-wide state; put it back however the test ends.
  struct SavedHomeOffset {
    xyz_pos_t was;
    SavedHomeOffset() { was = motion.home_offset; }
    ~SavedHomeOffset() { motion.home_offset = was; }
  };

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    const bool was = MYSERIAL1.host_connected;
    MYSERIAL1.host_connected = false;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    MYSERIAL1.host_connected = was;
  }

  std::string reply_to(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    SerialCapture capture;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    return capture.finish();
  }

  bool contains(const std::string &haystack, const char * const needle) {
    return haystack.find(needle) != std::string::npos;
  }

  // Put the machine, the planner and the steppers all at the same place.
  void machine_is_at(const float x, const float y, const float z) {
    xyze_pos_t here = { 0 };
    NUM_AXIS_CODE(here.x = x, here.y = y, here.z = z, , , , , , );
    motion.position = here;
    planner.set_position_mm(here);
  }

  // A value no arithmetic in M428 could arrive at, so "unchanged" means untouched.
  constexpr float MARKER = 1.25f;

  /**
   * Why the last nine mutants of `M206_M428.cpp:87` cannot be killed here, stated as a
   * check rather than as a comment.
   *
   * M428's correction replaces `base_home_pos(i) - position[i]` with `-position[i]`, so on
   * any axis whose home position is zero the two are the same expression and the branch is
   * a no-op however the guard is mutated. `012-max_endstops` moves Z off zero, which is
   * what makes the correction observable at all; X and Y stay at zero, so every mutant that
   * merely widens the guard to include them still changes nothing.
   *
   * If this ever fails, those survivors have become killable and the classification in
   * CLAUDE.md is stale.
   */
  static_assert(X_HOME_POS == 0, "the equivalence argument for M206_M428.cpp:87 assumes X homes to zero");
  #if HAS_Y_AXIS
    static_assert(Y_HOME_POS == 0, "the equivalence argument for M206_M428.cpp:87 assumes Y homes to zero");
  #endif

  void offset_is_marked() {
    LOOP_NUM_AXES(i) motion.set_home_offset((AxisEnum)i, MARKER);
  }

  void offset_is_still_marked(const char * const why) {
    LOOP_NUM_AXES(i) TEST_ASSERT_EQUAL_FLOAT_MESSAGE(MARKER, motion.home_offset[i], why);
  }

}

// ---------------------------------------------------------------------------
// M428 — measure the offset from where the machine is
// ---------------------------------------------------------------------------

/**
 * M428 on an unhomed machine is refused, and says which axes to home.
 *
 * Without this the offset would be measured from a position the machine only guessed at,
 * which is the one input for which the answer is guaranteed wrong. The refusal has to be
 * audible as well as effective: a host that got silence would have no way to tell the
 * difference between "done" and "declined".
 */
MARLIN_TEST(home_offsets, M428_before_homing_asks_to_be_homed_and_changes_nothing) {
  SimulatedMachine machine;
  SavedHomeOffset restore;

  machine_is_at(5.0f, 0.0f, 0.0f);
  offset_is_marked();
  motion.set_all_unhomed();

  const std::string reply = reply_to("M428");

  TEST_ASSERT_TRUE_MESSAGE(contains(reply, "Home"), "M428 did not ask for homing");
  offset_is_still_marked("an unhomed M428 set an offset anyway");
}

/**
 * The point of M428, stated physically.
 *
 * The tool is driven 5 mm off the switch and M428 is told that this spot is home. Homing
 * again and asking for the origin must bring the carriage back to that same spot rather
 * than to the switch — that is what "set the origin here" means to the person who used it.
 *
 * The assertion is on the rail, which counts pulses on the step pin: homing re-references
 * the firmware's own counters to the switch without the carriage moving, so nothing the
 * firmware reports can measure the displacement that homing itself introduces.
 *
 * The control is implicit and worth stating: with no M428 in the middle, `G0 X0` after
 * homing puts the carriage at the switch, 5 mm away from where this test asserts it is.
 */
MARLIN_TEST(home_offsets, M428_makes_the_spot_it_is_at_read_as_the_origin) {
  SimulatedMachine machine;
  SavedHomeOffset restore;

  XRail x(0.0f, 30.0f);
  machine_is_at(30.0f, 0.0f, 0.0f);
  LOOP_NUM_AXES(i) motion.set_home_offset((AxisEnum)i, 0.0f);

  motion.set_axis_never_homed(X_AXIS);
  host_sends("G28 X");                                  // origin is the switch

  host_sends("G0 X5 F6000");
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
  const float the_spot = x.mm();
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, 5.0f, the_spot, "the carriage did not reach X5");

  host_sends("M428");                                   // ...and now it is here

  motion.set_axis_never_homed(X_AXIS);
  host_sends("G28 X");
  host_sends("G0 X0 F6000");
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.3f, the_spot, x.mm(),
    "asking for the origin did not return the tool to the spot M428 was called at");
}

/**
 * Every axis is measured, and each from its own position.
 *
 * Three different distances, so an offset written to the wrong axis, or one axis's
 * distance used for another, cannot pass. The expected values are stated from the
 * configured home position and the distance the test itself commanded, rather than from
 * the accessor M428 computes with.
 *
 * Every axis here is close to its home, which is the case with no correction in it: the
 * reference point is the endstop position on any machine. `M428_near_the_bed_measures_
 * from_zero_when_the_switch_is_at_the_top` is the other one.
 */
MARLIN_TEST(home_offsets, M428_records_each_axis_distance_from_its_own_home) {
  SimulatedMachine machine;
  SavedHomeOffset restore;

  offset_is_marked();
  machine_is_at(X_HOME_POS + 5.0f, Y_HOME_POS + 3.0f, Z_HOME_POS + 2.0f);
  motion.set_all_homed();

  host_sends("M428");

  TEST_ASSERT_EQUAL_FLOAT(-5.0f, motion.home_offset.x);
  #if HAS_Y_AXIS
    TEST_ASSERT_EQUAL_FLOAT(-3.0f, motion.home_offset.y);
  #endif
  #if HAS_Z_AXIS
    TEST_ASSERT_EQUAL_FLOAT(-2.0f, motion.home_offset.z);
  #endif
}

/**
 * A successful M428 reports where the machine now is, and says the offsets were applied.
 *
 * Two separate channels with two separate failures behind them. The position report is
 * how a host learns that its idea of the coordinates is stale; the status message is how
 * a person at the machine learns the command did anything at all.
 */
MARLIN_TEST(home_offsets, M428_reports_the_new_position_and_says_the_offsets_are_applied) {
  SimulatedMachine machine;
  SavedHomeOffset restore;

  machine_is_at(5.0f, 3.0f, 0.0f);
  motion.set_all_homed();

  const std::string reply = reply_to("M428");

  TEST_ASSERT_TRUE_MESSAGE(contains(reply, "X:5.00"), "M428 did not report the position");
  TEST_ASSERT_TRUE_MESSAGE(contains(reply, "Offsets Applied"),
    "M428 did not say the offsets had been applied");
}

/**
 * Too far from any reference point is refused, on both channels, and nothing is changed.
 *
 * The two messages are deliberately different words — `Too far from MIN/MAX` on the error
 * channel and `MIN/MAX Too Far` on the status line — so each is asserted for what it is
 * rather than through the other. A test matching one loose substring would pass with
 * either of them deleted.
 */
MARLIN_TEST(home_offsets, M428_further_than_20mm_from_home_is_refused_and_reported) {
  SimulatedMachine machine;
  SavedHomeOffset restore;

  offset_is_marked();
  machine_is_at(25.0f, 0.0f, 0.0f);
  motion.set_all_homed();

  const std::string reply = reply_to("M428");

  TEST_ASSERT_TRUE_MESSAGE(contains(reply, "Error:" STR_ERR_M428_TOO_FAR),
    "the host was not told the position was out of range");
  TEST_ASSERT_TRUE_MESSAGE(contains(reply, "MIN/MAX Too Far"),
    "the status line did not carry the alert");
  offset_is_still_marked("a refused M428 changed the offset anyway");
}

/**
 * The 20 mm limit, bracketed from both sides and in both directions.
 *
 * "It refuses when far away" is satisfied by a limit of any size at all, and by a limit
 * with the wrong sign on one end. Four positions astride the two bounds pin the number and
 * the symmetry: just inside is accepted, just outside is refused, either way from home.
 */
MARLIN_TEST(home_offsets, M428_takes_20mm_either_side_of_home_and_no_more) {
  struct Case { float at; bool accepted; const char *what; };
  const Case cases[] = {
    { 19.9f, true,  "19.9 mm short of home should be accepted" },
    { 20.1f, false, "20.1 mm short of home should be refused" },
    { -19.9f, true,  "19.9 mm past home should be accepted" },
    { -20.1f, false, "20.1 mm past home should be refused" }
  };

  for (const Case &c : cases) {
    SimulatedMachine machine;
    SavedHomeOffset restore;

    offset_is_marked();
    machine_is_at(c.at, 0.0f, 0.0f);
    motion.set_all_homed();

    host_sends("M428");

    if (c.accepted)
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(X_HOME_POS - c.at, motion.home_offset.x, c.what);
    else
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(MARKER, motion.home_offset.x, c.what);
  }
}

/**
 * The axis that is out of range need not be the first one checked.
 *
 * X is at home and Y is 25 mm away, so a check that stopped after the first axis — or
 * that only ever looked at axis 0 — would accept this and shift the origin in Y by
 * exactly the distance the guard exists to refuse.
 */
#if HAS_Y_AXIS
  MARLIN_TEST(home_offsets, M428_refuses_when_any_axis_is_too_far_not_just_the_first) {
    SimulatedMachine machine;
    SavedHomeOffset restore;

    offset_is_marked();
    machine_is_at(0.0f, 25.0f, 0.0f);
    motion.set_all_homed();

    const std::string reply = reply_to("M428");

    TEST_ASSERT_TRUE_MESSAGE(contains(reply, "Error:" STR_ERR_M428_TOO_FAR),
      "a Y axis out of range was not refused");
    offset_is_still_marked("a refused M428 changed the offset anyway");
  }
#endif

// ---------------------------------------------------------------------------
// A machine that homes Z upward — see test/012-max_endstops.ini
// ---------------------------------------------------------------------------

#if Z_HOME_TO_MAX

  /**
   * Near the bed, the reference point is zero rather than the endstop.
   *
   * On a machine whose Z switch is at the top, the useful place to stand when setting a Z
   * offset is at the bed — and the bed is the whole length of the axis away from the
   * switch, which the 20 mm guard would otherwise refuse. M428 has a correction for
   * exactly this: an axis on the far side of centre from its endstop is measured from 0
   * instead. The command's own comment says so, and until `012-max_endstops` there was no
   * configuration here in which the line could run.
   *
   * The number matters as much as the fact. Measured from the endstop the answer would be
   * `Z_HOME_POS - 2`; measured from zero it is `-2`, and only one of those puts the origin
   * where the operator is standing.
   */
  MARLIN_TEST(home_offsets, M428_near_the_bed_measures_from_zero_when_the_switch_is_at_the_top) {
    SimulatedMachine machine;
    SavedHomeOffset restore;

    offset_is_marked();
    machine_is_at(0.0f, 0.0f, 2.0f);          // 2 mm off the bed, a long way below the switch
    motion.set_all_homed();

    host_sends("M428");

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-2.0f, motion.home_offset.z,
      "close to the bed M428 should measure from zero, not from the endstop at the top");
  }

  /**
   * ...and near the switch it is still the switch, not zero.
   *
   * The correction is conditional, and a test of only the corrected case is passed by
   * firmware that corrects always. Two millimetres below the endstop is the same distance
   * from home as the case above is from the bed, and the two answers differ by the whole
   * length of the axis.
   */
  MARLIN_TEST(home_offsets, M428_near_the_top_still_measures_from_the_endstop) {
    SimulatedMachine machine;
    SavedHomeOffset restore;

    offset_is_marked();
    machine_is_at(0.0f, 0.0f, Z_HOME_POS - 2.0f);
    motion.set_all_homed();

    host_sends("M428");

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f, motion.home_offset.z,
      "close to the endstop M428 should measure from the endstop");
  }

  /**
   * The correction does not lift the 20 mm limit, it moves what the limit is measured from.
   *
   * Somewhere between the bed and the switch is a band that is too far from both, and a
   * machine that accepted it would shift the origin by however far the operator happened to
   * have jogged. Bracketed either side of the bed's own limit, since that is the reference
   * the correction introduces.
   */
  MARLIN_TEST(home_offsets, M428_between_the_bed_and_the_switch_is_still_too_far) {
    SimulatedMachine machine;
    SavedHomeOffset restore;

    offset_is_marked();
    machine_is_at(0.0f, 0.0f, 20.1f);         // past the bed's 20 mm, nowhere near the switch
    motion.set_all_homed();

    const std::string reply = reply_to("M428");

    TEST_ASSERT_TRUE_MESSAGE(contains(reply, "Error:" STR_ERR_M428_TOO_FAR),
      "a Z between the two reference points should be refused");
    offset_is_still_marked("a refused M428 changed the offset anyway");
  }

#endif // Z_HOME_TO_MAX

// ---------------------------------------------------------------------------
// M206 — set the offset directly
// ---------------------------------------------------------------------------

/**
 * M206 reports where the machine is once the offset has changed.
 *
 * The offset moves the coordinate system under the tool, so the coordinates the host is
 * holding become stale the moment the command is accepted. Reporting the position is how
 * it finds out; without it the host carries on with numbers that no longer mean anything.
 */
MARLIN_TEST(home_offsets, M206_reports_the_position_after_changing_the_offset) {
  SimulatedMachine machine;
  SavedHomeOffset restore;

  machine_is_at(5.0f, 3.0f, 0.0f);

  const std::string reply = reply_to("M206 X1");

  TEST_ASSERT_TRUE_MESSAGE(contains(reply, "X:5.00"),
    "M206 did not report the position after setting an offset");
}

/**
 * The M503 listing labels the home offset, rather than printing a bare M206 line.
 *
 * A settings dump is read by people and by hosts restoring a machine; an unlabelled
 * section is one a reader cannot attribute. The heading is emitted for the listing and
 * suppressed on a plain `M206` query, so both are asserted — the second is what fails if
 * the heading is emitted unconditionally.
 */
MARLIN_TEST(home_offsets, the_home_offset_is_headed_in_the_settings_listing_only) {
  SimulatedMachine machine;
  SavedHomeOffset restore;

  TEST_ASSERT_TRUE_MESSAGE(contains(reply_to("M503"), STR_HOME_OFFSET),
    "the settings listing did not name the home offset");
  TEST_ASSERT_FALSE_MESSAGE(contains(reply_to("M206"), STR_HOME_OFFSET),
    "a plain M206 query printed the listing heading");
}


#endif // HAS_HOME_OFFSET
