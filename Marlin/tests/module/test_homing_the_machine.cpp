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
 * Homing a whole machine.
 *
 * `test_homing.cpp` homes one axis against one switch, which is where the mechanism is best
 * observed. It is not where `G28` lives: the command is a *sequence* — decide which axes to
 * home, get the nozzle clear of the bed first, home the lateral axes, cross to a safe point,
 * probe for Z, and put back the levelling it had to switch off to do any of it. Homing a
 * single axis takes almost none of that path.
 *
 * So this file is about the sequence and the arguments that vary it, and it lives in the
 * configuration with a probe because Z homing here *is* a probe: the same surface fixture the
 * levelling tests measure stands in for the bed.
 */

#include "src/inc/MarlinConfig.h"

#if defined(__PLAT_TEST__) && HAS_BED_PROBE && ENABLED(Z_SAFE_HOMING)

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "../support/simulated_bed_surface.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/probe.h"
#include "src/module/endstops.h"
#include "src/feature/bedlevel/bedlevel.h"
#include "src/libs/vector_3.h"

#include <string.h>

namespace {

  constexpr float SPM = SimulatedMachine::STEPS_PER_MM;

  struct XRail : SimulatedAxisWithLimit {
    XRail(const float switch_at_mm, const float carriage_at_mm)
      : SimulatedAxisWithLimit(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                               X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE,
                               int32_t(switch_at_mm * SPM), int32_t(carriage_at_mm * SPM)) {}
    float mm() const { return float(position()) / SPM; }
  };

  struct YRail : SimulatedAxisWithLimit {
    YRail(const float switch_at_mm, const float carriage_at_mm)
      : SimulatedAxisWithLimit(Y_STEP_PIN, Y_DIR_PIN, ENABLED(INVERT_Y_DIR),
                               Y_MIN_PIN, Y_MIN_ENDSTOP_HIT_STATE,
                               int32_t(switch_at_mm * SPM), int32_t(carriage_at_mm * SPM)) {}
    float mm() const { return float(position()) / SPM; }
  };

  void host_sends(const char * const line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  // Everything the machine believes about itself, put back on the way in and out. A failing
  // test leaves through a longjmp that runs no destructor, so the state is reset on entry as
  // well — otherwise one failure quietly changes what the next test is measuring.
  struct HomingSlate {
    HomingSlate() { tidy(); }
    ~HomingSlate() { tidy(); }
    static void tidy() {
      TERN_(HAS_LEVELING, set_bed_leveling_enabled(false));
      TERN_(ABL_PLANAR, planner.bed_level_matrix.set_to_identity());
      motion.set_all_unhomed();
    }
  };

  // A machine standing over its bed, nothing homed, ready for a G28.
  struct UnhomedMachine {
    SimulatedMachine machine;
    HomingSlate slate;
    XRail x{0.0f, 40.0f};
    YRail y{0.0f, 40.0f};
    SimulatedBedSurface bed{x, y, SPM, 0.0f, 0.0f, 0.0f, 20.0f};

    UnhomedMachine() {
      xyze_pos_t here = { 0 }; here.x = 40.0f; here.y = 40.0f; here.z = 20.0f;
      motion.position = here;
      planner.set_position_mm(here);
      motion.set_all_unhomed();
    }
  };

}

/**
 * `G28` with no arguments homes every axis.
 *
 * The command's whole job in one assertion. Each axis is checked separately because homing
 * two of three and reporting success is exactly the failure that matters: the machine would
 * then move confidently along an axis whose origin it never found.
 */
MARLIN_TEST(homing_machine, G28_homes_every_axis) {
  UnhomedMachine m;

  host_sends("G28");

  LOOP_NUM_AXES(i) {
    char why[64];
    snprintf(why, sizeof(why), "axis %d should have been homed by a bare G28", i);
    TEST_ASSERT_FALSE_MESSAGE(motion.axis_should_home(AxisEnum(i)), why);
    TEST_ASSERT_TRUE_MESSAGE(motion.axis_is_trusted(AxisEnum(i)), why);
  }
}


/**
 * `G28 X` homes X, and does not quietly home anything else.
 *
 * A machine that homed everything whenever it was asked for one axis would move axes the
 * operator did not ask to be moved — which, mid-job or with a part on the bed, is the reason
 * a single-axis form exists at all.
 */
MARLIN_TEST(homing_machine, G28_Y_homes_only_Y) {
  UnhomedMachine m;

  host_sends("G28 Y");

  TEST_ASSERT_FALSE_MESSAGE(motion.axis_should_home(Y_AXIS), "G28 Y should home Y");
  TEST_ASSERT_TRUE_MESSAGE(motion.axis_should_home(X_AXIS), "G28 Y should leave X unhomed");
  TEST_ASSERT_TRUE_MESSAGE(motion.axis_should_home(Z_AXIS), "G28 Y should leave Z unhomed");
}

/**
 * `G28 O` homes only what is not already homed.
 *
 * A host that homes defensively before each job would otherwise repeat the whole sequence
 * every time. Counting switch closures is what says nothing happened: the axis position after
 * a re-home is the same either way, so position alone cannot tell a skipped sequence from a
 * repeated one.
 */
MARLIN_TEST(homing_machine, G28_O_skips_homing_that_has_already_been_done) {
  UnhomedMachine m;

  host_sends("G28");
  const uint16_t after_homing = m.x.closures();
  TEST_ASSERT_TRUE_MESSAGE(after_homing > 0, "the first G28 should have touched the switch");

  host_sends("G28 O");
  TEST_ASSERT_EQUAL_MESSAGE(after_homing, m.x.closures(),
    "G28 O on a homed machine should not touch the switch again");

  // ...and the same command does home when there is something to home, which is what says
  // the skip was a decision rather than the command doing nothing.
  motion.set_axis_never_homed(X_AXIS);
  host_sends("G28 O");
  TEST_ASSERT_TRUE_MESSAGE(m.x.closures() > after_homing,
    "G28 O should home an axis that has lost its reference");
}

/**
 * Homing lifts the nozzle off the bed before it moves across it.
 *
 * The nozzle may be anywhere, including touching the print, and homing X and Y drags it the
 * width of the machine. So Z is raised first — and `R` is how much, with `R0` meaning the
 * caller has already made room and would rather not lift.
 *
 * `G28 X` rather than a bare `G28`, so the only Z movement in the sequence is the raise being
 * measured: homing Z would move it much further and swamp the difference.
 */
MARLIN_TEST(homing_machine, the_nozzle_is_raised_before_homing_by_the_amount_asked_for) {
  float from_r0 = 0.0f, from_r5 = 0.0f, started_at = 0.0f;
  constexpr float RAISE = 5.0f;

  {
    UnhomedMachine m;
    started_at = m.bed.nozzle_mm();
    host_sends("G28 X R0");
    from_r0 = m.bed.highest_mm();
  }
  {
    UnhomedMachine m;
    host_sends("G28 X R" STRINGIFY(5));
    from_r5 = m.bed.highest_mm();
  }

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, started_at, from_r0,
    "R0 asks for no raise, so the nozzle should not go up at all");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, started_at + RAISE, from_r5,
    "R5 should raise the nozzle five millimetres above where it stood");
}

/**
 * Without `R`, the clearance is a coordinate — even when Z is unknown. **Defect #28.**
 *
 * `Z_CLEARANCE_FOR_HOMING` exists so the nozzle is off the bed before homing drags it across
 * one. The source says how the number should be read: "The 'height' for Z is a coordinate.
 * But if Z is not trusted/homed make it relative." An unknown Z has no coordinate system for a
 * coordinate to mean anything in, so the raise ought to be a lift.
 *
 * The condition is inverted with respect to that comment — it asks for a *lift* only when Z is
 * homed and its minimum is untrusted, and for a *coordinate* when Z is unknown. So a machine
 * that believes it is high up, and is not, makes no room at all and then homes X across the
 * bed. Recorded rather than corrected, and pinned here so a fix is visible as this test
 * failing. See docs/defect-register.md #28.
 */
MARLIN_TEST(homing_machine, an_unknown_Z_reads_the_clearance_as_a_coordinate_and_may_not_lift) {
  UnhomedMachine m;   // believes it is at Z20, and is
  const float started_at = m.bed.nozzle_mm();

  TEST_ASSERT_TRUE_MESSAGE(motion.axis_should_home(Z_AXIS), "this test starts with Z unknown");
  TEST_ASSERT_TRUE_MESSAGE(started_at > (Z_CLEARANCE_FOR_HOMING),
    "the believed position must be above the clearance for the two readings to differ");

  host_sends("G28 X");

  // What a lift would have given: started_at + Z_CLEARANCE_FOR_HOMING. What a coordinate
  // gives: nothing, because the believed position is already above it.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, started_at, m.bed.highest_mm(),
    "recorded behaviour: an unknown Z above the clearance coordinate is not raised at all");
}

/**
 * From below the clearance the nozzle is raised to it.
 *
 * The other half of the same reading, and the case that stops the test above from being
 * satisfied by a `G28` that never raises anything.
 */
MARLIN_TEST(homing_machine, homing_lifts_a_nozzle_that_is_below_the_clearance) {
  UnhomedMachine m;

  // Down near the bed, which is where the clearance matters, and where the two readings of
  // the number agree.
  m.bed.place_nozzle_at(1.0f);
  xyze_pos_t here = motion.position; here.z = 1.0f;
  motion.position = here;
  planner.set_position_mm(here);

  host_sends("G28 X");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, Z_CLEARANCE_FOR_HOMING, m.bed.highest_mm(),
    "a nozzle below the clearance should be lifted to it before homing moves across the bed");
}

/**
 * A machine that knows where Z is treats the clearance as a height, not a lift.
 *
 * The distinction only exists because of what the number means when Z is unknown: it cannot
 * be a coordinate, because there is no coordinate system yet, so it is read as "go up by
 * this much". Once Z is homed the same number is a place — and a nozzle already well above
 * it does not need to move at all.
 *
 * Read as a lift instead, an ordinary `G28 X` partway through a tall print would climb the
 * clearance again on every call.
 */
MARLIN_TEST(homing_machine, a_homed_machine_reads_the_clearance_as_a_height) {
  UnhomedMachine m;

  host_sends("G28");
  TEST_ASSERT_FALSE_MESSAGE(motion.axis_should_home(Z_AXIS), "Z should be homed before this test starts");

  // Well above the clearance, so a machine reading it as a height has nothing to do.
  const float standing_at = (Z_CLEARANCE_FOR_HOMING) + 5.0f;
  char up[24]; snprintf(up, sizeof(up), "G1 Z%.1f F600", double(standing_at));
  host_sends(up);
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the machine did not reach the starting height");
  m.bed.forget_highest();
  const float before_homing = m.bed.nozzle_mm();

  host_sends("G28 X");

  // Against where it started, not against where it ended: a raise that happened and was
  // never undone would leave those two equal and the assertion satisfied.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, before_homing, m.bed.highest_mm(),
    "a nozzle already above the clearance height should not be raised again");
}

/**
 * `R` is the raise, whatever it is asked for.
 *
 * Two amounts and zero are what say the parameter is read rather than merely noticed — a
 * machine that treated any `R` as "the usual clearance", or as "no raise", would satisfy a
 * test that only ever asked for one value.
 */
MARLIN_TEST(homing_machine, the_R_raise_is_the_distance_asked_for) {
  float started_at = 0.0f, from_r1 = 0.0f;

  {
    UnhomedMachine m;
    started_at = m.bed.nozzle_mm();
    host_sends("G28 X R" STRINGIFY(1));
    from_r1 = m.bed.highest_mm();
  }

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, started_at + 1.0f, from_r1,
    "R1 should raise the nozzle one millimetre, not the configured clearance and not nothing");
}

/**
 * Z is homed over the middle of the bed, not wherever the nozzle happened to be.
 *
 * `Z_SAFE_HOMING` exists because Z homing here drives the nozzle down until the probe
 * triggers, and the probe only has a bed underneath it in the middle. Homing Z at a corner
 * would drive the nozzle past the bed edge and into the frame.
 *
 * It is the *probe* that has to be over the bed, so the carriage stops an offset short of the
 * middle — asserting the nozzle position would be asserting the offset by accident.
 */
MARLIN_TEST(homing_machine, Z_is_homed_with_the_probe_over_the_middle_of_the_bed) {
  UnhomedMachine m;

  host_sends("G28");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, X_CENTER, motion.position.x + probe.offset_xy.x,
    "the probe should have been over the middle of X when Z was homed");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, Y_CENTER, motion.position.y + probe.offset_xy.y,
    "the probe should have been over the middle of Y when Z was homed");
}

/**
 * `G28 Z H` homes Z where the nozzle already is.
 *
 * The safe point is the right default and the wrong answer for anyone measuring a particular
 * spot — checking a suspect corner, or re-probing where a print failed. `H` says the caller
 * has chosen the place.
 *
 * Asserting the carriage did *not* move is the whole of it: Z is homed either way, so the
 * flag is invisible in everything except where it happened.
 */
MARLIN_TEST(homing_machine, G28_Z_H_homes_Z_without_crossing_to_the_safe_point) {
  UnhomedMachine m;

  host_sends("G28 X Y");
  host_sends("G1 X30 Y70 F3000");
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the machine did not reach the chosen spot");

  const float chose_x = motion.position.x, chose_y = motion.position.y;
  TEST_ASSERT_TRUE_MESSAGE(fabsf(chose_x + probe.offset_xy.x - (X_CENTER)) > 1.0f,
    "the chosen spot must not be the safe point, or this asserts nothing");

  host_sends("G28 Z H");

  TEST_ASSERT_FALSE_MESSAGE(motion.axis_should_home(Z_AXIS), "G28 Z H should still home Z");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, chose_x, motion.position.x,
    "G28 Z H should home Z where the nozzle stood, not at the safe point");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, chose_y, motion.position.y,
    "and should not have crossed in Y either");
}

/**
 * `G28 Z` homes X and Y first, because it cannot home Z without them.
 *
 * Z is homed over one particular point of the bed — the only place the probe has a bed
 * underneath it — and reaching that point means moving in X and Y. A machine with no lateral
 * origin cannot go there, so asking for Z alone is taken as asking for whatever Z needs.
 *
 * The alternative reading, that `G28 Z` homes only Z, would drive the nozzle down at whatever
 * arbitrary point it happened to be over.
 */
MARLIN_TEST(homing_machine, G28_Z_homes_what_Z_depends_on) {
  UnhomedMachine m;

  host_sends("G28 Z");

  TEST_ASSERT_FALSE_MESSAGE(motion.axis_should_home(Z_AXIS), "G28 Z should home Z");
  TEST_ASSERT_FALSE_MESSAGE(motion.axis_should_home(X_AXIS),
    "G28 Z should home X first, since Z is homed at a point it has to travel to");
  TEST_ASSERT_FALSE_MESSAGE(motion.axis_should_home(Y_AXIS),
    "G28 Z should home Y first, for the same reason");

  // ...and only because they were unknown. An axis already homed is not homed again.
  const uint16_t closures = m.x.closures();
  host_sends("G28 Z");
  TEST_ASSERT_EQUAL_MESSAGE(closures, m.x.closures(),
    "G28 Z should not re-home an X that already knows where it is");
}

#if HAS_LEVELING

/**
 * Homing switches levelling off to do its work, and puts it back afterwards.
 *
 * Every homing move has to be in machine coordinates — correcting them for a bed whose
 * position is exactly what is being re-established would be circular. But an operator who
 * homes mid-job expects the plane they measured to still be in force afterwards, so it is
 * restored rather than discarded.
 *
 * Asserting the correction rather than the flag is what says the plane survived: switching
 * levelling back on with the matrix thrown away would satisfy a check on `leveling_active`.
 */
MARLIN_TEST(homing_machine, G28_restores_the_levelling_it_switched_off) {
  UnhomedMachine m;

  host_sends("G28");

  // Stand in for a measured bed: a matrix that visibly corrects, without running a probe grid.
  const vector_3 tilted = vector_3(-0.02f, 0.01f, 1.0f).get_normal();
  planner.bed_level_matrix = matrix_3x3::create_look_at(tilted);
  set_bed_leveling_enabled(true);

  xyz_pos_t sample = { 30.0f, 30.0f, 0.0f };
  planner.apply_leveling(sample);
  const float correction_before = sample.z;
  TEST_ASSERT_TRUE_MESSAGE(fabsf(correction_before) > 1e-3f,
    "the stand-in plane must correct something for this to test anything");

  host_sends("G28");

  TEST_ASSERT_TRUE_MESSAGE(planner.leveling_active,
    "levelling should be switched back on after homing");

  xyz_pos_t after = { 30.0f, 30.0f, 0.0f };
  planner.apply_leveling(after);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, correction_before, after.z,
    "the plane measured before homing should still be the plane afterwards");
}

/**
 * `G28 L0` says leave it off.
 *
 * The restore is a convenience, and an operator who has just homed because something is
 * wrong needs a way to get the machine into plain coordinates and keep it there.
 */
MARLIN_TEST(homing_machine, G28_L0_leaves_levelling_switched_off) {
  UnhomedMachine m;

  host_sends("G28");
  planner.bed_level_matrix = matrix_3x3::create_look_at(vector_3(-0.02f, 0.01f, 1.0f).get_normal());
  set_bed_leveling_enabled(true);

  host_sends("G28 L0");

  TEST_ASSERT_FALSE_MESSAGE(planner.leveling_active,
    "G28 L0 should leave levelling switched off");
}

/**
 * Homing does not switch levelling *on*.
 *
 * The restore puts back what was there, and what was there may have been nothing. A machine
 * that came out of homing correcting for a plane the operator had switched off — or never
 * measured — would silently print skewed, and the operator's own action would be the thing
 * that had been undone.
 */
MARLIN_TEST(homing_machine, G28_leaves_levelling_off_if_it_was_off) {
  UnhomedMachine m;

  planner.bed_level_matrix = matrix_3x3::create_look_at(vector_3(-0.02f, 0.01f, 1.0f).get_normal());
  set_bed_leveling_enabled(false);
  TEST_ASSERT_FALSE_MESSAGE(planner.leveling_active, "this test starts with levelling off");

  host_sends("G28");

  TEST_ASSERT_FALSE_MESSAGE(planner.leveling_active,
    "homing should restore the levelling state it found, and it found none");
}

#endif // HAS_LEVELING


/**
 * Homing Z with a probe reports where the *nozzle* is, not where the probe triggered.
 *
 * The switch closes when the probe touches the bed, and the probe hangs below the nozzle — so
 * the nozzle is still that far up. Every move afterwards is commanded in nozzle coordinates,
 * so the offset has to be taken out at the moment the origin is set, or the first layer is
 * printed the probe's offset too high for the rest of the machine's life.
 *
 * The assertion is the difference between two runs over the same bed, so it is the correction
 * that is checked rather than the height it was applied to.
 */
MARLIN_TEST(homing_machine, homing_Z_with_a_probe_reports_the_nozzle_height) {
  constexpr float OFFSET_Z = -2.0f;
  float level = 0.0f, hanging_low = 0.0f;

  {
    UnhomedMachine m;
    const float was = probe.offset.z;
    probe.offset.z = 0.0f;
    host_sends("G28");
    level = motion.position.z;
    probe.offset.z = was;
  }
  {
    UnhomedMachine m;
    const float was = probe.offset.z;
    probe.offset.z = OFFSET_Z;
    host_sends("G28");
    hanging_low = motion.position.z;
    probe.offset.z = was;
  }

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, -OFFSET_Z, hanging_low - level,
    "a probe two millimetres below the nozzle should leave the nozzle reported two higher");
}

/**
 * The probe is put away when homing finishes.
 *
 * Leaving it deployed leaves the Z endstop watching the probe, so the next ordinary move
 * downwards would stop on it. Homing is the one sequence that deploys the probe without the
 * caller asking, so it is the one that has to put it back.
 */
MARLIN_TEST(homing_machine, homing_puts_the_probe_away_afterwards) {
  UnhomedMachine m;

  host_sends("G28");

  TEST_ASSERT_FALSE_MESSAGE(endstops.z_probe_enabled,
    "the probe should be stowed once homing is over");
}

/**
 * Between the two touches, Z retreats by the probe's clearance rather than the axis's bump.
 *
 * `HOMING_BUMP_MM` is how far a limit switch needs backing off to be approached again. A probe
 * needs more than that: it has to be clear of the bed, because what happens between the two
 * touches is a move across a surface it is nearly touching. So the retreat is the larger of
 * the two distances, which for this machine is the probe clearance.
 *
 * The nozzle starts just above the bed so that the retreat is the highest point of the whole
 * sequence, and `R0` suppresses the pre-homing raise that would otherwise be higher still.
 */
MARLIN_TEST(homing_machine, Z_retreats_by_the_probe_clearance_between_its_two_touches) {
  UnhomedMachine m;

  host_sends("G28 X Y");

  // Deploy first. Homing Z deploys the probe itself, and deploying raises to the deploy
  // clearance — which is higher than the retreat and would be all this measured. Asking for
  // it in advance makes the deploy inside the sequence a no-op.
  probe.deploy();

  // Down near the bed, machine and fixture agreeing.
  constexpr float START_Z = 1.0f;
  m.bed.place_nozzle_at(START_Z);
  xyze_pos_t here = motion.position; here.z = START_Z;
  motion.position = here;
  planner.set_position_mm(here);
  m.bed.forget_highest();

  host_sends("G28 Z R0");

  constexpr xyz_float_t bump = HOMING_BUMP_MM;
  const float retreated_to = m.bed.highest_mm();

  TEST_ASSERT_TRUE_MESSAGE((Z_CLEARANCE_BETWEEN_PROBES) > bump.z,
    "this test distinguishes two distances, so they must differ");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.3f, m.bed.height_at(0.0f, 0.0f) + (Z_CLEARANCE_BETWEEN_PROBES),
    retreated_to,
    "Z should retreat by the probe clearance, not by the axis homing bump");
}

/**
 * Homing leaves no endstop hit outstanding.
 *
 * An endstop closing is normally an event worth telling the host about — it means a move was
 * stopped by something. During homing it is the entire point, so each homing move clears the
 * record as it goes. Left set, the next thing to look would report a switch that was hit on
 * purpose several moves ago.
 */
MARLIN_TEST(homing_machine, homing_leaves_no_endstop_hit_outstanding) {
  UnhomedMachine m;

  host_sends("G28");

  TEST_ASSERT_EQUAL_MESSAGE(0, endstops.trigger_state(),
    "homing touches every switch on purpose and should leave none of them recorded as a hit");
}

#endif // __PLAT_TEST__ && HAS_BED_PROBE && Z_SAFE_HOMING
