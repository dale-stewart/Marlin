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
 * The first tests of an LCD driver in this fork.
 *
 * A DWIN panel is not an interface the firmware calls; it is a screen the firmware writes
 * bytes at. So there is no equivalent of `stub_extui`'s recorded callbacks — the byte
 * stream on `LCD_SERIAL` is the only thing an outside observer can see, and a test that
 * cannot read it can only assert that the firmware did not crash while drawing.
 *
 * `SerialCapture` takes the port to watch, so the same drainer the host-facing tests use
 * works here. It has to drain from another thread: the write busy-waits for room in a
 * 128-byte buffer, and a screen refresh is longer than that, so draining afterwards would
 * be draining a buffer whose producer is already wedged.
 */

#include "../test/unit_tests.h"
#include "src/inc/MarlinConfig.h"

#if ENABLED(DWIN_CREALITY_LCD)

#include "src/lcd/dwin/creality/dwin.h"
#include "src/lcd/dwin/common/dwin_api.h"
#include "src/module/planner.h"
#include "src/lcd/marlinui.h"
#include "src/MarlinCore.h"
#include "../gcode/serial_capture.h"
#include "../support/simulated_encoder.h"
#include "../support/simulated_machine.h"
#include "../gcode/simulated_sensors.h"
#include "src/module/temperature.h"
#include "src/sd/cardreader.h"
#include <string.h>
#include <vector>

MARLIN_TEST(dwin_display, a_status_message_reaches_the_panel) {
  SerialCapture panel(LCD_SERIAL);
  dwinStatusChanged("RESCUED");
  const std::string sent = panel.finish();

  TEST_ASSERT_TRUE_MESSAGE(sent.size() > 0,
    "changing the status should put bytes on the display's serial port");
}

#if ENABLED(EDITABLE_STEPS_PER_UNIT)

/**
 * Register #33, confirmed by behaviour rather than by reading the source.
 *
 * `planner.settings.axis_steps_per_mm` has a derived cache, `planner.mm_per_step`, which is
 * its reciprocal — and keeping the two in step is the caller's job, done by calling
 * `refresh_positioning()`. This driver writes the array and does not. So the machine goes on
 * moving at a scale that no longer matches the one it reports, and nothing complains.
 *
 * The test says exactly that and no more: after a person changes the resolution at the panel,
 * the stored value changed and the reciprocal did not follow it. It does not assert the
 * arithmetic of the new value — the driver scales by a factor private to its own translation
 * unit, and predicting it here would be re-implementing the code under test.
 *
 * Nothing is set behind the driver's back. The starting value is deliberately below the
 * driver's own lower limit, so the first turn of the knob is clamped up to it — which makes
 * the committed value the driver's choice rather than the test's.
 */
MARLIN_TEST(dwin_display, changing_the_resolution_at_the_panel_leaves_the_reciprocal_stale) {
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;              // a click on a dark panel only wakes the screen
  marlin.wait_for_user = false;     // ... and a click that resumes a wait is not an ENTER

  const float steps_before = planner.settings.axis_steps_per_mm[X_AXIS],
              per_step_before = planner.mm_per_step[X_AXIS];

  // The panel, showing the X steps-per-millimetre editor.
  checkkey = ID_StepValue;
  hmiFlag.step_axis = X_AXIS;
  hmiValues.maxStepScaled = 0;

  const auto pump = [] { dwinHandleScreen(); };
  knob.turn_clockwise(pump);
  knob.click(pump);

  panel.finish();

  const float steps_after = planner.settings.axis_steps_per_mm[X_AXIS];

  TEST_ASSERT_NOT_EQUAL_MESSAGE(steps_before, steps_after,
    "turning the knob and clicking should store a new resolution for X");

  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(per_step_before, planner.mm_per_step[X_AXIS],
    "the reciprocal should have gone stale - it is not refreshed after the write (register #33)");

  TEST_ASSERT_FALSE_MESSAGE(NEAR(planner.mm_per_step[X_AXIS], 1.0f / steps_after),
    "and being stale means it no longer is the reciprocal of the stored resolution");

  // Put the machine back the way it was found, both halves together this time.
  planner.set_steps_per_mm(X_AXIS, steps_before);
}

/**
 * The panel's other two value editors get it right, and that is why #33 is a defect.
 *
 * `hmiMaxFeedspeedXYZE()` and `hmiMaxAccelerationXYZE()` commit through
 * `planner.set_max_feedrate()` and `planner.set_max_acceleration()` — the setters that keep
 * whatever is derived from the value in step with it. `hmiStepXYZE()`, three functions away in
 * the same file, assigns `planner.settings.axis_steps_per_mm` directly and refreshes nothing.
 *
 * Pinning the two correct ones is not padding. It is what turns #33 from "this driver does not
 * bother with refreshes" into "this driver refreshes everywhere except one place", which is the
 * difference between a design decision and a bug — and it is what would notice if a fix for #33
 * broke its neighbours on the way past.
 *
 * The derived quantity for acceleration is `max_acceleration_steps_per_s2`, which the stepper
 * compares against on every block. Asserting it rather than the stored millimetre value is the
 * whole point: the stored value is right in both the working and the broken version.
 */
MARLIN_TEST(dwin_display, changing_the_acceleration_at_the_panel_refreshes_the_step_limit) {
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  const float accel_before = planner.settings.max_acceleration_mm_per_s2[X_AXIS];

  // The panel, showing the X maximum-acceleration editor, with a value below the driver's own
  // lower limit so the first turn is clamped up to it — the committed value is the driver's
  // choice rather than the test's, the same arrangement the steps editor test uses.
  checkkey = ID_MaxAccelerationValue;
  hmiFlag.acc_axis = X_AXIS;
  hmiValues.maxAcceleration = 0;

  const auto pump = [] { dwinHandleScreen(); };
  knob.turn_clockwise(pump);
  knob.click(pump);

  panel.finish();

  const float accel_after = planner.settings.max_acceleration_mm_per_s2[X_AXIS];

  TEST_ASSERT_NOT_EQUAL_MESSAGE(accel_before, accel_after,
    "turning the knob and clicking should store a new maximum acceleration for X");

  // The limit the stepper actually enforces is in steps, and it is derived from the value the
  // panel just wrote. `set_max_acceleration` refreshes it; a raw assignment would not.
  const uint32_t expected_steps = uint32_t(accel_after * planner.settings.axis_steps_per_mm[X_AXIS]);
  TEST_ASSERT_UINT32_WITHIN_MESSAGE(2, expected_steps, planner.max_acceleration_steps_per_s2[X_AXIS],
    "the step-rate limit should have been refreshed from the new acceleration");

  planner.set_max_acceleration(X_AXIS, accel_before);
}

/**
 * ...and the feedrate editor stores what the knob committed.
 *
 * Nothing is derived from a maximum feedrate in this build, so there is no stale-cache twin to
 * assert — which is worth saying rather than leaving as a gap in the pattern. What this pins is
 * the commit itself and the axis it lands on: `hmiFlag.feedspeed_axis` selects which of the
 * four values the click writes, and a panel that edited X and stored Y would be indetectable
 * from the screen.
 */
MARLIN_TEST(dwin_display, changing_the_feedrate_at_the_panel_stores_it_against_the_right_axis) {
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  const feedRate_t x_before = planner.settings.max_feedrate_mm_s[X_AXIS];
  #if HAS_Y_AXIS
    const feedRate_t y_before = planner.settings.max_feedrate_mm_s[Y_AXIS];
  #endif

  checkkey = ID_MaxSpeedValue;
  hmiFlag.feedspeed_axis = X_AXIS;
  hmiValues.maxFeedSpeed = 0;

  const auto pump = [] { dwinHandleScreen(); };
  knob.turn_clockwise(pump);
  knob.click(pump);

  panel.finish();

  TEST_ASSERT_NOT_EQUAL_MESSAGE(x_before, planner.settings.max_feedrate_mm_s[X_AXIS],
    "the click should have stored a new maximum feedrate for X");
  #if HAS_Y_AXIS
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(y_before, planner.settings.max_feedrate_mm_s[Y_AXIS],
      "and should not have touched another axis");
  #endif

  planner.set_max_feedrate(X_AXIS, x_before);
}

#endif // EDITABLE_STEPS_PER_UNIT

// ---------------------------------------------------------------------------
// Getting around the menus
// ---------------------------------------------------------------------------

/**
 * Every main-menu page leads somewhere, and to somewhere different — in adjacency order.
 *
 * This is the first thing anybody does with the panel: turn the knob until the icon they want
 * is highlighted, then press. The dispatch is a `switch` with one arm per page, so a machine
 * that sent every press to the same screen, or that had two arms transposed, would look
 * completely normal until you tried to use it. Pressing on one page only would leave three arms
 * unasserted — the same shape as `each_switch_is_reported_from_its_own_pin`: the deciding
 * element has to be moved through the collection rather than exercised once.
 *
 * **The test deliberately assumes neither the knob's direction nor its gearing.** A first
 * version hard-coded both and failed twice in different ways — clockwise turned out to
 * *decrease* the selection, and a `turn_clockwise()` call did not reliably move it one page.
 * Neither is a property of the firmware worth pinning here; both are properties of how this
 * fixture and this panel happen to agree, and encoding them would have made the test a
 * statement about the harness.
 *
 * **The menus also rate-limit the knob, where the value editors do not.** `hmiMainMenu()` reads
 * through `get_encoder_state()`, which ignores everything for `ENCODER_WAIT_MS` (20 ms) after
 * each accepted event; the value editors call `encoderReceiveAnalyze()` directly and have no
 * such gate. Under a HAL where time only moves when a test says so, that means consecutive
 * turns are simply swallowed — the first version of this test walked the whole menu and never
 * left the first page. Every step here lets the gate expire, which is also what a person's hand
 * does without thinking about it.
 *
 * So it winds hard to one end, then walks the other way one detent at a time, clicking at each
 * step and recording where it lands. What is asserted is the *sequence of distinct
 * destinations*, which is exactly the claim worth making: the four screens are reachable, they
 * are adjacent in the order the icons are drawn, and no two pages lead to the same place.
 *
 * `checkkey` is the observable rather than the internal selection, and it is the better one: it
 * is *where the press took you*, which is what the user experiences, and it is already part of
 * the driver's declared surface.
 */
MARLIN_TEST(dwin_display, the_main_menu_pages_lead_to_four_different_screens_in_order) {
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  // Every pump lets the encoder's 20 ms gate expire, or the turns after the first are ignored.
  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  // Wind hard to one end. `select_t::dec()` clamps at zero and `inc(4)` clamps at three, so
  // enough turns either way reaches a known end whichever way round the knob is wired.
  checkkey = ID_MainMenu;
  for (uint8_t i = 0; i < 20; i++) knob.turn_counterclockwise(pump);

  // Now walk back, clicking at every step, and collect the destinations in the order they
  // appear. Twenty steps is far more than four pages, so the walk is bounded by the clamp
  // rather than by a count that has to be right.
  std::vector<uint8_t> seen;
  for (uint8_t i = 0; i < 20; i++) {
    checkkey = ID_MainMenu;
    knob.click(pump);
    if (seen.empty() || seen.back() != checkkey) seen.push_back(checkkey);
    checkkey = ID_MainMenu;
    knob.turn_clockwise(pump);
  }

  // One last press at the far end, in case the final turn moved onto a page not yet pressed.
  checkkey = ID_MainMenu;
  knob.click(pump);
  if (seen.empty() || seen.back() != checkkey) seen.push_back(checkkey);

  panel.finish();
  checkkey = ID_MainMenu;

  const uint8_t last_page = TERN(HAS_ONESTEP_LEVELING, ID_Leveling, ID_Info);
  const uint8_t forwards[] = { ID_SelectFile, ID_Prepare, ID_Control, last_page };

  char msg[200];
  snprintf(msg, sizeof(msg), "walking the knob across the menu should visit four screens, saw %u",
           unsigned(seen.size()));
  TEST_ASSERT_EQUAL_MESSAGE(4, seen.size(), msg);

  // Either direction of travel is fine — which way the knob turns is the panel's business —
  // but the *order* must be the order the icons are drawn in.
  const bool ascending = seen.front() == forwards[0];
  for (size_t k = 0; k < 4; k++) {
    const uint8_t expect = ascending ? forwards[k] : forwards[3 - k];
    snprintf(msg, sizeof(msg), "screen %u along should be %u, was %u",
             unsigned(k), unsigned(expect), unsigned(seen[k]));
    TEST_ASSERT_EQUAL_MESSAGE(expect, seen[k], msg);
  }
}

// ---------------------------------------------------------------------------
// Keeping the panel's numbers agreeing with the machine
// ---------------------------------------------------------------------------

/**
 * The panel is redrawn only where something changed.
 *
 * `updateVariable()` runs on every UI pass and holds a `static` cache of each displayed value —
 * the two temperatures, their targets, the fan, the flow, the feedrate, the babystep offset.
 * Every one is compared and only redrawn on a difference. That is not an optimisation to be
 * traded away: the link to the panel is a 128-byte buffer that the firmware busy-waits on, so a
 * screen redrawn wholesale on every pass would spend the print blocking on it. It is the same
 * property `SerialCapture` needs a second thread for.
 *
 * The assertion is on *how much* came out rather than on what it said. Decoding the DWIN wire
 * format would pin the protocol — pixel positions, font ids, the lot — and none of that is the
 * behaviour under test. Traffic volume is: a pass with nothing to say must cost less than a
 * pass with something to say.
 *
 * The caches are process-wide statics, so the first call after anything else has touched the
 * machine reports everything. Priming is therefore part of the arrangement rather than
 * defensive padding.
 */
MARLIN_TEST(dwin_display, the_panel_is_redrawn_only_where_something_changed) {
  SimulatedMachine machine;
  SimulatedSensors sensors;

  checkkey = ID_MainMenu;

  // Prime: whatever the caches were holding, get them agreeing with the machine now.
  { SerialCapture panel(LCD_SERIAL); updateVariable(); panel.finish(); }

  size_t quiet = 0, quiet_again = 0, after_change = 0;

  { SerialCapture panel(LCD_SERIAL); updateVariable(); quiet = panel.finish().size(); }
  { SerialCapture panel(LCD_SERIAL); updateVariable(); quiet_again = panel.finish().size(); }

  // Not merely "the same" — *nothing*. Measured rather than assumed: a settled machine costs
  // 0 bytes and a changed hotend target costs 42, so this is the strong form of the claim
  // rather than a bound that a wasteful implementation could also satisfy.
  TEST_ASSERT_EQUAL_MESSAGE(0, quiet,
    "a pass over a machine that has not changed should send the panel nothing at all");
  TEST_ASSERT_EQUAL_MESSAGE(0, quiet_again,
    "and should keep sending nothing: the cache should settle, not oscillate");

  const celsius_t was = thermalManager.degTargetHotend(0);
  thermalManager.setTargetHotend(was + 40, 0);

  { SerialCapture panel(LCD_SERIAL); updateVariable(); after_change = panel.finish().size(); }

  char msg[160];
  snprintf(msg, sizeof(msg),
           "a changed target should reach the panel: quiet pass was %u bytes, changed pass %u",
           unsigned(quiet), unsigned(after_change));
  TEST_ASSERT_TRUE_MESSAGE(after_change > quiet, msg);

  thermalManager.setTargetHotend(was, 0);
  { SerialCapture panel(LCD_SERIAL); updateVariable(); panel.finish(); }
}

// ---------------------------------------------------------------------------
// What the file menu calls a file
// ---------------------------------------------------------------------------

#if HAS_MEDIA

namespace {

  /**
   * Run a filename through the menu's namer and return what the row would say.
   *
   * The length is passed rather than left to `MENU_CHAR_LIMIT`, because the truncation rule is
   * the same on every panel while the room to print it is not — the same reasoning as
   * `MAX_MESSAGE_SIZE` in the message-template tests.
   *
   * The card's own `longFilename` has to be set as well as the argument, because the function
   * takes its starting index from `card.longest_filename()` rather than from `src`. That is
   * defect #54 and it is why this helper writes the same string into both.
   */
  std::string menu_name_for(const char * const filename, const bool is_dir, const size_t maxlen) {
    strlcpy(card.longFilename, filename, sizeof(card.longFilename));
    card.flag.filenameIsDir = is_dir;

    char src[LONG_FILENAME_LENGTH];
    strlcpy(src, filename, sizeof(src));

    char dst[LONG_FILENAME_LENGTH] = { 0 };
    make_name_without_ext(dst, src, maxlen);
    return std::string(dst);
  }

}

/**
 * A file loses its extension; a folder keeps its dots.
 *
 * The menu shows `model` rather than `model.gcode` because the row is narrow and every file on
 * the card ends the same way. A *folder* has no extension to remove, so the same trimming
 * applied to it would eat part of its name — `v1.2` would become `v1`, naming a directory that
 * is not there.
 *
 * Both directions, because the guard is a single `if` on `filenameIsDir` and a machine that
 * ignored it would look right on every file and wrong on every dotted folder.
 */
MARLIN_TEST(dwin_display, the_file_menu_drops_an_extension_but_not_a_folders_dots) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("model", menu_name_for("model.gcode", false, 24).c_str(),
    "a file should be shown without its extension");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("part", menu_name_for("part.g", false, 24).c_str(),
    "whichever of the three extensions it has");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("v1.2", menu_name_for("v1.2", true, 24).c_str(),
    "a folder has no extension to drop and should keep its dots");
}

/**
 * Only the last dot counts.
 *
 * Names with a version or a date in them are ordinary — `bracket.v2.gcode` — and the search
 * runs backwards from the end for exactly that reason. A machine that stopped at the first dot
 * would show `bracket`, so two revisions of the same part would be indistinguishable in the
 * list you pick a print from.
 */
MARLIN_TEST(dwin_display, only_the_last_dot_starts_the_extension) {
  TEST_ASSERT_EQUAL_STRING("bracket.v2", menu_name_for("bracket.v2.gcode", false, 24).c_str());
}

/**
 * LEGACY-BEHAVIOR: defect #55 — a file whose name has no dot is shown as an empty row.
 *
 * The backwards search is `while (pos && src[pos] != '.') pos--`. With nothing to find it walks
 * all the way to zero, and zero is then taken as the length: `dst[0] = '\0'`. The row is blank.
 *
 * **Latent, not live**, and the reason is in the card rather than here: `is_visible_entity()`
 * only lists a file when its 8.3 extension begins with `G`, and the 8.3 name is derived from
 * the long one — so a file with no extension never reaches the menu. Directories are handled
 * before the loop by the `filenameIsDir` guard, which is why `v1.2` survives above.
 *
 * Pinned rather than fixed, and pinned to what it *does*: the first version of this test
 * asserted the sensible answer, `README`, and found the empty string. That is the finding. If
 * the card's filter ever widens, or a caller passes a name from somewhere else, this test is
 * what will notice.
 */
MARLIN_TEST(dwin_display, a_name_with_no_extension_comes_out_empty) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", menu_name_for("README", false, 24).c_str(),
    "a dotless name walks the search to zero and is shown as nothing (defect #55)");

  // The same name as a folder is left whole, which is the arm that makes this a defect in the
  // file path rather than a deliberate rule about dotless names.
  TEST_ASSERT_EQUAL_STRING_MESSAGE("README", menu_name_for("README", true, 24).c_str(),
    "a folder skips the extension search entirely and keeps its name");
}

/**
 * A name too long for the row is cut and marked, and the mark replaces rather than extends.
 *
 * The three dots are written *into* the last three characters of the allowance, not appended
 * after it — `dst[--pos] = '.'` three times from `len = maxlen`. So the result is exactly
 * `maxlen` characters however long the name was, which is what stops the row overrunning into
 * whatever the panel draws next. Asserting the ellipsis without asserting the length would pass
 * against an implementation that appended it.
 *
 * Bracketed: a name that fits exactly is untouched, and one character more is cut.
 */
MARLIN_TEST(dwin_display, a_name_too_long_for_the_row_is_cut_and_marked) {
  const std::string fits = menu_name_for("12345678.gcode", false, 8);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("12345678", fits.c_str(),
    "a name that fills the row exactly should be left alone");

  const std::string cut = menu_name_for("123456789.gcode", false, 8);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("12345...", cut.c_str(),
    "one character more should be cut and marked with an ellipsis");
  TEST_ASSERT_EQUAL_MESSAGE(8, cut.size(),
    "and the marked name should still be exactly the width of the row");
}

#endif // HAS_MEDIA

#endif // ENABLED(DWIN_CREALITY_LCD)
