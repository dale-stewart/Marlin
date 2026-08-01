/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2024 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
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
 * Acceptance tests for parser.feature.
 *
 * These describe what the printer understands when a host sends it a line, in
 * the host's terms rather than the parser's. Each test is one scenario from the
 * feature file and is built only from the steps below, so the scenarios stay
 * readable and the parser's internals stay in one place.
 */

#include "../test/unit_tests.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"

namespace {

  // The parser writes into the line it is given, so each scenario gets its own copy.
  char command_line[128];

  void the_printer_is_waiting_for_a_command() {
    parser.command_letter = -128;
    parser.codenum = -1;
    parser.string_arg = nullptr;
  }

  void the_host_sends(const char * const line) {
    strncpy(command_line, line, sizeof(command_line) - 1);
    command_line[sizeof(command_line) - 1] = '\0';
    parser.parse(command_line);
  }

  // "G0", "M104", "T1" — the command as a host would write it.
  void the_printer_understands_the_command(const char * const expected) {
    TEST_ASSERT_EQUAL(expected[0], parser.command_letter);
    TEST_ASSERT_EQUAL(atoi(expected + 1), parser.codenum);
  }

  // Refusing a line means nothing of it — or of the line before it — is remembered.
  void the_printer_refuses_the_command() {
    TEST_ASSERT_EQUAL('?', parser.command_letter);
    TEST_ASSERT_EQUAL(0, parser.codenum);
  }

  void no_parameters_are_remembered() {
    for (char c = 'A'; c <= 'Z'; c++) TEST_ASSERT_FALSE(parser.seen(c));
  }

  void the_value_of_is(const char param, const float expected) {
    TEST_ASSERT_TRUE(parser.seenval(param));
    TEST_ASSERT_EQUAL_FLOAT(expected, parser.value_float());
  }

  void a_value_was_given_for(const char param) {
    TEST_ASSERT_TRUE(parser.seenval(param));
  }

  void no_value_was_given_for(const char param) {
    TEST_ASSERT_FALSE(parser.seenval(param));
  }

  void the_message_is(const char * const expected) {
    TEST_ASSERT_TRUE(parser.has_string());
    TEST_ASSERT_EQUAL_STRING(expected, parser.string_arg);
  }

  void there_is_no_message() {
    TEST_ASSERT_FALSE(parser.has_string());
  }

} // namespace

MARLIN_TEST(gcode_acceptance, a_movement_command_with_coordinates) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("G0 X10 Z30");
  the_printer_understands_the_command("G0");
  the_value_of_is('X', 10);
  no_value_was_given_for('Y');
}

MARLIN_TEST(gcode_acceptance, a_command_whose_number_has_more_than_one_digit) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("M104 S200");
  the_printer_understands_the_command("M104");
  the_value_of_is('S', 200);
}

MARLIN_TEST(gcode_acceptance, a_tool_change_command) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("T1 S1");
  the_printer_understands_the_command("T1");
  a_value_was_given_for('S');
}

MARLIN_TEST(gcode_acceptance, a_line_numbered_by_the_host) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("N1234   M104 S200");
  the_printer_understands_the_command("M104");
  the_value_of_is('S', 200);
}

MARLIN_TEST(gcode_acceptance, a_line_protected_by_a_checksum) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("N1 G0 X10*85");
  the_printer_understands_the_command("G0");
  the_value_of_is('X', 10);
}

MARLIN_TEST(gcode_acceptance, spacing_does_not_change_the_meaning_of_a_line) {
  const char * const lines[] = { "G0 X10", "G0   X10", "G0 X 10", "G0X10", "   G0 X10", "G0 X10   Y20" };
  for (const char * const line : lines) {
    the_printer_is_waiting_for_a_command();
    the_host_sends(line);
    the_printer_understands_the_command("G0");
    the_value_of_is('X', 10);
  }

  // The same tolerance applies between a command letter and its number.
  the_printer_is_waiting_for_a_command();
  the_host_sends("M  104 S200 X10");
  the_printer_understands_the_command("M104");
  the_value_of_is('X', 10);
}

MARLIN_TEST(gcode_acceptance, negative_and_fractional_coordinates) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("G0 X-10.5 Y20.25");
  the_value_of_is('X', -10.5);
  the_value_of_is('Y', 20.25);
}

MARLIN_TEST(gcode_acceptance, a_message_command_takes_the_rest_of_the_line) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("M118 Hello World");
  the_printer_understands_the_command("M118");
  the_message_is("Hello World");
}

MARLIN_TEST(gcode_acceptance, a_file_selection_command_takes_a_path) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("M32 !/path/to/file.g#");
  the_printer_understands_the_command("M32");
  the_message_is("/path/to/file.g");
}

MARLIN_TEST(gcode_acceptance, a_line_that_is_not_a_command_is_refused) {
  const char * const lines[] = { "GX10", "Q1 X10", "NG0 X10", "" };
  for (const char * const line : lines) {
    the_printer_is_waiting_for_a_command();
    the_host_sends(line);
    the_printer_refuses_the_command();
  }
}

MARLIN_TEST(gcode_acceptance, a_refused_line_does_not_leave_the_previous_command_in_place) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("M118 Hello");
  the_message_is("Hello");

  the_host_sends("");
  the_printer_refuses_the_command();
  there_is_no_message();
}

MARLIN_TEST(gcode_acceptance, a_parameter_given_without_a_value) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("G0 X");
  the_printer_understands_the_command("G0");
  no_value_was_given_for('X');
  the_message_is("X");
}

// LEGACY-BEHAVIOR: the message starts at the space before the following token
// rather than at the valueless parameter itself, so this is " Y" and not "X Y".
MARLIN_TEST(gcode_acceptance, only_the_first_valueless_parameter_is_taken_as_the_message) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("G0 X Y");
  the_message_is(" Y");
}

MARLIN_TEST(gcode_acceptance, a_parameter_that_is_not_a_letter_is_taken_as_the_message) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("M33 !/path/to/file.g#");
  the_printer_understands_the_command("M33");
  the_message_is("!/path/to/file.g#");
}

MARLIN_TEST(gcode_acceptance, the_message_form_belongs_to_the_command_not_its_number) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("G118 X10");
  the_printer_understands_the_command("G118");
  the_value_of_is('X', 10);
  there_is_no_message();
}

MARLIN_TEST(gcode_acceptance, a_refused_line_does_not_leave_the_previous_values_in_place) {
  the_printer_is_waiting_for_a_command();
  the_host_sends("M104 S200");
  the_value_of_is('S', 200);

  the_host_sends("");
  the_printer_refuses_the_command();
  no_value_was_given_for('S');
  no_parameters_are_remembered();
}
