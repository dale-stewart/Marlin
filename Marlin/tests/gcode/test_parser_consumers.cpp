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
 * The last reachable consumers of the parser's global state (`test/009-parser_consumers.ini`).
 *
 * Each of these files is small and was compiled by no configuration until this one, so
 * there is nothing here to preserve — these are the first tests, not a rescue of
 * existing ones. All five go in through the same front door as the rest of the gcode
 * suite: a line is parsed and dispatched exactly as a host would send it.
 */

#include "src/inc/MarlinConfig.h"

#include "../test/unit_tests.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/core/serial.h"
#include "src/MarlinCore.h"
#include "src/module/motion.h"
#include "serial_capture.h"

#include <string.h>
#include <string>

// The whole file is compiled out unless at least one of its five consumers is built —
// otherwise the shared `host_sends`/`reply_to` helpers below are defined and unused,
// which this build treats as an error (-Werror=unused-function).
#if ENABLED(CONFIGURABLE_MACHINE_NAME) || ENABLED(CNC_COORDINATE_SYSTEMS) \
  || ENABLED(GCODE_MACROS) || (HAS_MEDIA && ENABLED(LONG_FILENAME_HOST_SUPPORT))

namespace {

  // See CLAUDE.md: the port spins forever on a full transmit buffer when it believes a
  // host is listening, so anything that reports must run with the port quiet unless the
  // test wants to capture the reply.
  struct NoHostAttached {
    bool was;
    NoHostAttached() { was = MYSERIAL1.host_connected; MYSERIAL1.host_connected = false; }
    ~NoHostAttached() { MYSERIAL1.host_connected = was; }
  };

  void host_sends(const char * const line) {
    NoHostAttached quiet;
    static char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  std::string reply_to(const char * const line) {
    SerialCapture capture;
    static char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
    return capture.finish();
  }

}

#if ENABLED(CONFIGURABLE_MACHINE_NAME)

/**
 * M550: Set machine name.
 *
 * The name is the printer's own identity, reported back to the host. Setting it two
 * different ways (a quoted "P" value and a bare quoted string) covers the two live
 * branches into `did_set`; `GCODE_QUOTED_STRINGS` is on in this build, so the
 * unquoted-'P' middle branch (`TERN(GCODE_QUOTED_STRINGS, false, ...)`) can never be
 * taken here and is not exercised — see the defect register.
 */
MARLIN_TEST(parser_consumers, M550_sets_the_machine_name_from_a_P_value) {
  host_sends("M550 P\"Workshop\"");
  TEST_ASSERT_EQUAL_STRING("Workshop", (char*)marlin.machine_name);
}

MARLIN_TEST(parser_consumers, M550_sets_the_machine_name_from_a_bare_quoted_string) {
  host_sends("M550 \"Garage\"");
  TEST_ASSERT_EQUAL_STRING("Garage", (char*)marlin.machine_name);
}

/**
 * A name is trimmed of surrounding whitespace before it is kept, so what the host later
 * reads back does not carry the padding it was sent with.
 */
MARLIN_TEST(parser_consumers, M550_trims_whitespace_from_the_new_name) {
  host_sends("M550 P\"  Attic  \"");
  TEST_ASSERT_EQUAL_STRING("Attic", (char*)marlin.machine_name);
}

/**
 * With no name given, M550 reports the current one instead of changing it.
 */
MARLIN_TEST(parser_consumers, M550_with_no_argument_reports_the_current_name_unchanged) {
  host_sends("M550 P\"Reference\"");
  const std::string reply = reply_to("M550");
  TEST_ASSERT_EQUAL_STRING("Reference", (char*)marlin.machine_name);
  // The exact label matters, not merely that the name appears somewhere in the reply —
  // a mutant that dropped or garbled "RepRap name: " but still emitted the name would
  // otherwise pass.
  TEST_ASSERT_TRUE_MESSAGE(reply.find("RepRap name: Reference") != std::string::npos,
    "M550 with no argument should echo the current name under its label");
}

#endif // CONFIGURABLE_MACHINE_NAME

#if ENABLED(CNC_COORDINATE_SYSTEMS)

/**
 * G54-G59.3: select a CNC workspace.
 *
 * Selecting a system loads *its own* stored offset into `motion.workspace_offset` — not
 * some shared or default one — which is the property that would fail if the index math
 * in `G54_59()` (`parser.codenum - 54 + subcode`) were wrong in either the code number or
 * the subcode.
 */
MARLIN_TEST(parser_consumers, selecting_a_workspace_loads_its_own_stored_offset) {
  gcode.coordinate_system[0].set(1, 2, 3);
  gcode.coordinate_system[2].set(7, 8, 9);

  const std::string reply = reply_to("G54");
  TEST_ASSERT_EQUAL(0, gcode.active_coordinate_system);
  TEST_ASSERT_EQUAL_FLOAT(1, motion.workspace_offset.x);
  TEST_ASSERT_EQUAL_FLOAT(2, motion.workspace_offset.y);
  TEST_ASSERT_EQUAL_FLOAT(3, motion.workspace_offset.z);
  // The report has two parts, from two calls: the label naming which workspace was
  // selected, and the position report that follows it. Losing either would still leave
  // the offset itself correct, so the offset alone does not prove both calls ran.
  TEST_ASSERT_TRUE_MESSAGE(reply.find("Select workspace 0") != std::string::npos,
    "selecting a workspace should report which one was selected");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("X:") != std::string::npos,
    "selecting a workspace should report the resulting position");

  host_sends("G56");
  TEST_ASSERT_EQUAL(2, gcode.active_coordinate_system);
  TEST_ASSERT_EQUAL_FLOAT(7, motion.workspace_offset.x);
  TEST_ASSERT_EQUAL_FLOAT(8, motion.workspace_offset.y);
  TEST_ASSERT_EQUAL_FLOAT(9, motion.workspace_offset.z);

  gcode.coordinate_system[4].set(70, 80, 90);
  host_sends("G58");
  TEST_ASSERT_EQUAL(4, gcode.active_coordinate_system);
  TEST_ASSERT_EQUAL_FLOAT(70, motion.workspace_offset.x);
  TEST_ASSERT_EQUAL_FLOAT(80, motion.workspace_offset.y);
  TEST_ASSERT_EQUAL_FLOAT(90, motion.workspace_offset.z);
}

/**
 * G59.1-G59.3 reach the systems beyond G59 itself through the subcode, so the same
 * index arithmetic has to hold with a non-zero `parser.subcode` added in. The subcode
 * used here is derived from `MAX_COORDINATE_SYSTEMS` so the test reaches the highest
 * selectable workspace on any build, not just this one: G59 alone selects index 5
 * (`59 - 54`), so `.N` must add up to `MAX_COORDINATE_SYSTEMS - 1 - 5`.
 */
MARLIN_TEST(parser_consumers, G59_subcodes_select_the_extended_workspaces) {
  const uint8_t top = MAX_COORDINATE_SYSTEMS - 1;      // the highest selectable index
  const uint8_t g59_subcode = top - (59 - 54);         // how far past bare G59 that is
  gcode.coordinate_system[top].set(40, 50, 60);

  char cmd[16];
  snprintf(cmd, sizeof(cmd), "G59.%u", g59_subcode);
  host_sends(cmd);
  TEST_ASSERT_EQUAL(top, gcode.active_coordinate_system);
  TEST_ASSERT_EQUAL_FLOAT(40, motion.workspace_offset.x);
  TEST_ASSERT_EQUAL_FLOAT(50, motion.workspace_offset.y);
  TEST_ASSERT_EQUAL_FLOAT(60, motion.workspace_offset.z);
}

/**
 * The subcode is not itself range-checked by the parser, so a workspace index past
 * `MAX_COORDINATE_SYSTEMS` is reachable ("G59.99") even though no menu offers it. The
 * firmware still has to leave the offset at native (0) rather than reading past the end
 * of `coordinate_system[]` — this is the input class that pins the *upper* bound of the
 * range check in `select_coordinate_system()`, independent of whatever
 * `MAX_COORDINATE_SYSTEMS` happens to be.
 */
MARLIN_TEST(parser_consumers, a_subcode_past_the_last_workspace_leaves_the_offset_native) {
  const uint8_t out_of_range = MAX_COORDINATE_SYSTEMS; // one past the highest valid index
  const uint8_t g59_subcode = out_of_range - (59 - 54);
  char cmd[16];
  snprintf(cmd, sizeof(cmd), "G59.%u", g59_subcode);
  host_sends(cmd);
  TEST_ASSERT_EQUAL(out_of_range, gcode.active_coordinate_system);
  TEST_ASSERT_EQUAL_FLOAT(0, motion.workspace_offset.x);
  TEST_ASSERT_EQUAL_FLOAT(0, motion.workspace_offset.y);
  TEST_ASSERT_EQUAL_FLOAT(0, motion.workspace_offset.z);
}

/**
 * Selecting the workspace that is already active is a no-op: no report, no change.
 * `select_coordinate_system()` returns false in that case, and `G54_59()` only reports
 * when it returns true.
 */
MARLIN_TEST(parser_consumers, selecting_the_same_workspace_again_reports_nothing) {
  gcode.coordinate_system[1].set(11, 12, 13);
  host_sends("G55");

  const std::string reply = reply_to("G55");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("Select workspace") == std::string::npos,
    "reselecting the active workspace should not report a change");
  TEST_ASSERT_EQUAL_FLOAT(11, motion.workspace_offset.x);
}

/**
 * G53 alone (no chained command) drops to native space and stays there — it is the
 * "back to machine space" command, not a one-shot modifier, when nothing follows it.
 */
MARLIN_TEST(parser_consumers, G53_alone_switches_to_native_space_and_stays_there) {
  gcode.coordinate_system[3].set(21, 22, 23);
  host_sends("G57"); // select system 3
  TEST_ASSERT_EQUAL_FLOAT(21, motion.workspace_offset.x);

  host_sends("G53");
  TEST_ASSERT_EQUAL(-1, gcode.active_coordinate_system);
  TEST_ASSERT_EQUAL_FLOAT(0, motion.workspace_offset.x);
  TEST_ASSERT_EQUAL_FLOAT(0, motion.workspace_offset.y);
  TEST_ASSERT_EQUAL_FLOAT(0, motion.workspace_offset.z);
}

/**
 * G53 with a chained command runs that command natively, then restores the workspace
 * that was active before — the "modifier for one line" form. `M111` is the chained
 * command here because its effect (the debug flags) is trivial to observe and has
 * nothing to do with coordinates, which is what proves `chain()` really ran it rather
 * than the assertion coincidentally matching some side effect of G53 itself.
 */
MARLIN_TEST(parser_consumers, G53_with_a_chained_command_restores_the_workspace_after) {
  const uint8_t was_debug = marlin_debug_flags;
  gcode.coordinate_system[3].set(31, 32, 33);
  host_sends("G57"); // select system 3, offset (31,32,33)
  TEST_ASSERT_EQUAL(3, gcode.active_coordinate_system);

  host_sends("G53 M111 S5");
  TEST_ASSERT_EQUAL(5, marlin_debug_flags);
  TEST_ASSERT_EQUAL(3, gcode.active_coordinate_system);
  TEST_ASSERT_EQUAL_FLOAT(31, motion.workspace_offset.x);

  marlin_debug_flags = was_debug;
}

#endif // CNC_COORDINATE_SYSTEMS

#if ENABLED(EXPECTED_PRINTER_CHECK) && ENABLED(CONFIGURABLE_MACHINE_NAME)

/**
 * M16: Expected Printer Check.
 *
 * A host sends the name it expects and the firmware refuses the job if it does not
 * match its own. Only the matching case can be exercised here: a mismatch calls
 * `kill()`, which spins forever waiting for the KILL button or a reset and never
 * returns — the same class of untestable command as M112, documented in
 * `test_queue.cpp` and the defect register.
 */
MARLIN_TEST(parser_consumers, M16_with_the_machine_own_name_does_not_refuse) {
  host_sends("M550 P\"KnownGood\"");
  host_sends("M16 KnownGood");
  // Reaching this line at all is the assertion: a mismatch would have called kill(),
  // which never returns.
  TEST_ASSERT_EQUAL_STRING("KnownGood", (char*)marlin.machine_name);
}

#endif // EXPECTED_PRINTER_CHECK && CONFIGURABLE_MACHINE_NAME

#if ENABLED(GCODE_MACROS)

/**
 * M810-M819: define or run a stored G-code macro.
 *
 * With no argument the command executes the stored macro; with an argument it stores
 * one, translating '|' into a newline so a single command line can define several
 * commands. `M111` is used as the payload for the same reason as in the G53 test above:
 * its effect is a plain, unrelated global that proves the macro's *text* actually ran.
 */
MARLIN_TEST(parser_consumers, M810_stores_and_M810_without_args_runs_it) {
  const uint8_t was_debug = marlin_debug_flags;
  gcode.reset_macros();

  host_sends("M810 M111 S6");
  TEST_ASSERT_EQUAL_STRING("M111 S6", gcode.macros[0]);

  host_sends("M810");
  TEST_ASSERT_EQUAL(6, marlin_debug_flags);

  marlin_debug_flags = was_debug;
  gcode.reset_macros();
}

/**
 * '|' separates commands within one macro definition, becoming a newline so
 * `process_subcommands_now()` sees two lines rather than one malformed one.
 */
MARLIN_TEST(parser_consumers, M811_pipe_separates_two_commands_in_one_macro) {
  const uint8_t was_debug = marlin_debug_flags;
  gcode.reset_macros();

  host_sends("M811 M111 S1|M111 S4");
  TEST_ASSERT_EQUAL_STRING("M111 S1\nM111 S4", gcode.macros[1]);

  host_sends("M811");
  TEST_ASSERT_EQUAL(4, marlin_debug_flags); // the second command's value is what's left

  marlin_debug_flags = was_debug;
  gcode.reset_macros();
}

/**
 * A macro number at or beyond the number of configured slots is a no-op, neither
 * storing nor running anything. `GCODE_MACROS_SLOTS` is a build constant, so the
 * boundary is computed rather than a literal `M819` that would only be meaningful for
 * one particular build.
 */
MARLIN_TEST(parser_consumers, a_macro_number_past_the_configured_slots_does_nothing) {
  gcode.reset_macros();
  char cmd[16];
  snprintf(cmd, sizeof(cmd), "M%u M111 S2", 810 + GCODE_MACROS_SLOTS);
  host_sends(cmd);
  for (uint8_t i = 0; i < GCODE_MACROS_SLOTS; ++i)
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", gcode.macros[i], "no in-range slot should have been touched");
}

/**
 * A macro longer than its slot is rejected outright rather than being silently
 * truncated and stored partially. `GCODE_MACROS_SLOT_SIZE` is a build constant, so the
 * text is built long enough to exceed it whatever that size is.
 */
MARLIN_TEST(parser_consumers, a_macro_longer_than_its_slot_is_rejected_with_an_error) {
  gcode.reset_macros();

  char cmd[16 + GCODE_MACROS_SLOT_SIZE + 8];
  strcpy(cmd, "M812 ");
  size_t i = strlen(cmd);
  for (; i < strlen("M812 ") + GCODE_MACROS_SLOT_SIZE + 2; ++i) cmd[i] = 'A';
  cmd[i] = '\0';

  const std::string reply = reply_to(cmd);
  TEST_ASSERT_TRUE_MESSAGE(reply.find("too long") != std::string::npos,
    "an over-length macro should be reported as too long");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", gcode.macros[2], "an over-length macro should not be stored");
}

/**
 * The rejection is a strict "longer than", not "at least as long as" — a macro that
 * exactly fills its slot is accepted. This is the other side of the boundary from the
 * test above, both derived from `GCODE_MACROS_SLOT_SIZE` rather than a literal.
 */
MARLIN_TEST(parser_consumers, a_macro_that_exactly_fills_its_slot_is_accepted) {
  gcode.reset_macros();

  char cmd[16 + GCODE_MACROS_SLOT_SIZE];
  strcpy(cmd, "M813 ");
  size_t i = strlen(cmd);
  for (; i < strlen("M813 ") + GCODE_MACROS_SLOT_SIZE; ++i) cmd[i] = 'A';
  cmd[i] = '\0';

  const std::string reply = reply_to(cmd);
  TEST_ASSERT_TRUE_MESSAGE(reply.find("too long") == std::string::npos,
    "a macro exactly filling its slot should not be rejected");
  TEST_ASSERT_EQUAL(GCODE_MACROS_SLOT_SIZE, (int)strlen(gcode.macros[3]));
  gcode.reset_macros();
}

/**
 * Only the pipe character becomes a newline; nothing else in its neighbourhood does.
 * '}' sits one past '|' in ASCII, so it is what a relational near-miss (`>=` for `==`)
 * would wrongly convert.
 */
MARLIN_TEST(parser_consumers, only_the_pipe_character_becomes_a_newline) {
  gcode.reset_macros();
  host_sends("M814 A}B|C");
  TEST_ASSERT_EQUAL_STRING("A}B\nC", gcode.macros[4]);
  gcode.reset_macros();
}

/**
 * M810_819_report() is the forwarding half of macro reporting — it exists only to be
 * called from `M503`'s settings dump, so that is the front door that reaches it
 * (`M820` itself is a different file and dispatched directly by its own case, which
 * would not exercise this forwarding call at all).
 */
MARLIN_TEST(parser_consumers, M503_reports_a_stored_macro_through_M810_819_report) {
  gcode.reset_macros();
  host_sends("M810 M111 S9");

  const std::string reply = reply_to("M503");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("M810 M111 S9") != std::string::npos,
    "M503 should list a stored macro via M810_819_report");
  gcode.reset_macros();
}

#endif // GCODE_MACROS

#if HAS_MEDIA && ENABLED(LONG_FILENAME_HOST_SUPPORT)

#include "../support/simulated_media.h"
#include "src/sd/cardreader.h"

namespace {

  struct MediaSlate {
    bool was_connected;
    MediaSlate() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      if (card.isFileOpen()) card.closefile();
      if (!card.isMounted()) card.mount();
      card.cdroot();
    }
    ~MediaSlate() {
      if (card.isFileOpen()) card.closefile();
      card.cdroot();
      MYSERIAL1.host_connected = was_connected;
    }
  };

  void put_file(const char * const name, const char * const text) {
    card.openFileWrite(name);
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not create the file");
    card.write((void*)text, strlen(text));
    card.closefile();
  }

}

/**
 * M33: Get Long Path.
 *
 * This build has no `LONG_FILENAME_WRITE_SUPPORT`, so no file on this card ever gets a
 * long-filename directory entry — `printLongPath()`'s "pretty" half is therefore
 * unreachable here, and it falls back to the DOS name it was given
 * (`cardreader.cpp:409`, `longFilename[0] ? longFilename : filename`). What is
 * reachable, and what these test, is the path-walking itself: joining a single name and
 * joining a directory and a name with '/', through the real recursive lookup.
 */
MARLIN_TEST(parser_consumers, M33_reports_the_path_of_a_root_file) {
  MediaSlate slate;
  put_file("ROOTFILE.GCO", "G28\n");

  const std::string reply = reply_to("M33 ROOTFILE.GCO");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("/ROOTFILE.GCO") != std::string::npos,
    "M33 should report the path of a file that exists");
}

MARLIN_TEST(parser_consumers, M33_reports_the_path_of_a_file_in_a_subdirectory) {
  MediaSlate slate;
  MediaFile root = card.getroot(), made;
  made.mkdir(&root, "SUBDIR");
  made.close();
  card.cd("SUBDIR");
  put_file("NESTED.GCO", "G28\n");
  card.cdroot();

  const std::string reply = reply_to("M33 SUBDIR/NESTED.GCO");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("/SUBDIR/NESTED.GCO") != std::string::npos,
    "M33 should join a directory and a file name with '/'");
}

#endif // HAS_MEDIA && LONG_FILENAME_HOST_SUPPORT

#endif // any of the five consumers above
