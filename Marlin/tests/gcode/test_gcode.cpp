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

#include "../test/unit_tests.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"

MARLIN_TEST(gcode, process_parsed_command) {
  GcodeSuite suite;
  parser.command_letter = 'G';
  parser.codenum = 0;
  suite.process_parsed_command(false);
}

MARLIN_TEST(gcode, parse_g1_xz) {
  char current_command[] = "G0 X10 Z30";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('G', parser.command_letter);
  TEST_ASSERT_EQUAL(0, parser.codenum);
  TEST_ASSERT_TRUE(parser.seen('X'));
  TEST_ASSERT_FALSE(parser.seen('Y'));
  TEST_ASSERT_TRUE(parser.seen('Z'));
  TEST_ASSERT_FALSE(parser.seen('E'));
}

MARLIN_TEST(gcode, parse_g1_nxz) {
  char current_command[] = "N123 G0 X10 Z30";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('G', parser.command_letter);
  TEST_ASSERT_EQUAL(0, parser.codenum);
  TEST_ASSERT_TRUE(parser.seen('X'));
  TEST_ASSERT_FALSE(parser.seen('Y'));
  TEST_ASSERT_TRUE(parser.seen('Z'));
  TEST_ASSERT_FALSE(parser.seen('E'));
}

// A multi-digit code number exercises the digit accumulator more than once.
// With a single-digit code every scaling of the accumulator gives the same result.
MARLIN_TEST(gcode, parse_multi_digit_codenum) {
  char current_command[] = "M104 S200";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('M', parser.command_letter);
  TEST_ASSERT_EQUAL(104, parser.codenum);
  TEST_ASSERT_TRUE(parser.seenval('S'));
  TEST_ASSERT_EQUAL(200, parser.value_int());
}

MARLIN_TEST(gcode, parse_two_digit_codenum) {
  char current_command[] = "G28 X";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('G', parser.command_letter);
  TEST_ASSERT_EQUAL(28, parser.codenum);
  TEST_ASSERT_TRUE(parser.seen('X'));
}

// Spaces between the code number and the first parameter, and between a parameter
// and its value, must be skipped rather than ending the parse.
MARLIN_TEST(gcode, parse_extra_spaces) {
  char current_command[] = "G0   X10   Z30";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('G', parser.command_letter);
  TEST_ASSERT_EQUAL(0, parser.codenum);
  TEST_ASSERT_TRUE(parser.seenval('X'));
  TEST_ASSERT_EQUAL(10, parser.value_int());
  TEST_ASSERT_TRUE(parser.seenval('Z'));
}

MARLIN_TEST(gcode, parse_space_before_value) {
  char current_command[] = "G0 X 10 Z 30";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_TRUE(parser.seenval('X'));
  TEST_ASSERT_EQUAL(10, parser.value_int());
  TEST_ASSERT_TRUE(parser.seenval('Z'));
  TEST_ASSERT_EQUAL(30, parser.value_int());
}

// A parameter with no value becomes the string argument.
MARLIN_TEST(gcode, parse_valueless_param_is_string_arg) {
  char current_command[] = "G0 X";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_TRUE(parser.seen('X'));
  TEST_ASSERT_FALSE(parser.seenval('X'));
  TEST_ASSERT_TRUE(parser.has_string());
  TEST_ASSERT_EQUAL_STRING("X", parser.string_arg);
}

// M32 takes a path after '!' as its last parameter, terminated by '#'.
MARLIN_TEST(gcode, parse_m32_bang_path) {
  char current_command[] = "M32 !/path/to/file.g#";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('M', parser.command_letter);
  TEST_ASSERT_EQUAL(32, parser.codenum);
  TEST_ASSERT_TRUE(parser.has_string());
  TEST_ASSERT_EQUAL_STRING("/path/to/file.g", parser.string_arg);
}

// The '!' path is only special for M32 — any other command treats it as a parameter.
MARLIN_TEST(gcode, parse_bang_not_special_for_other_commands) {
  char current_command[] = "M33 !/path/to/file.g#";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL(33, parser.codenum);
  TEST_ASSERT_EQUAL_STRING("!/path/to/file.g#", parser.string_arg);
}

// Leading spaces before the command letter are skipped.
MARLIN_TEST(gcode, parse_leading_spaces) {
  char current_command[] = "   G0 X10";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('G', parser.command_letter);
  TEST_ASSERT_EQUAL(0, parser.codenum);
  TEST_ASSERT_TRUE(parser.seenval('X'));
}

// A multi-digit line number, and spaces after it, are skipped before the command.
MARLIN_TEST(gcode, parse_line_number_then_spaces) {
  char current_command[] = "N1234   M104 S200";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('M', parser.command_letter);
  TEST_ASSERT_EQUAL(104, parser.codenum);
  TEST_ASSERT_TRUE(parser.seenval('S'));
  TEST_ASSERT_EQUAL(200, parser.value_int());
}

// 'N' is only a line number when followed by a digit.
MARLIN_TEST(gcode, parse_n_without_number_is_not_a_line_number) {
  char current_command[] = "NG0 X10";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('?', parser.command_letter);
}

// Spaces are allowed between the command letter and its code number.
MARLIN_TEST(gcode, parse_space_between_letter_and_codenum) {
  char current_command[] = "M  104 S200";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('M', parser.command_letter);
  TEST_ASSERT_EQUAL(104, parser.codenum);
}

// A command letter with no code number is rejected.
MARLIN_TEST(gcode, parse_letter_without_codenum_is_rejected) {
  char current_command[] = "GX10";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('?', parser.command_letter);
  TEST_ASSERT_FALSE(parser.seen('X'));
}

// A letter that is not G, M or T is not a command.
MARLIN_TEST(gcode, parse_unknown_command_letter_is_rejected) {
  char current_command[] = "Q1 X10";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('?', parser.command_letter);
}

// M118 takes the rest of the line as its string argument.
MARLIN_TEST(gcode, parse_m118_takes_whole_line_as_string) {
  char current_command[] = "M118 Hello World";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('M', parser.command_letter);
  TEST_ASSERT_EQUAL(118, parser.codenum);
  TEST_ASSERT_EQUAL_STRING("Hello World", parser.string_arg);
}

// The whole-line string argument is specific to the M code, not the number.
MARLIN_TEST(gcode, parse_g118_does_not_take_whole_line) {
  char current_command[] = "G118 X10";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('G', parser.command_letter);
  TEST_ASSERT_EQUAL(118, parser.codenum);
  TEST_ASSERT_TRUE(parser.seenval('X'));
  TEST_ASSERT_EQUAL(10, parser.value_int());
}

// Parameters packed together with no separators are each found with their values.
MARLIN_TEST(gcode, parse_packed_params) {
  char current_command[] = "G0X10Y-20Z30.5";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_TRUE(parser.seenval('X'));
  TEST_ASSERT_EQUAL(10, parser.value_int());
  TEST_ASSERT_TRUE(parser.seenval('Y'));
  TEST_ASSERT_EQUAL(-20, parser.value_int());
  TEST_ASSERT_TRUE(parser.seenval('Z'));
  TEST_ASSERT_EQUAL_FLOAT(30.5f, parser.value_float());
}

// Parsing a line with no command clears the state left by the previous command.
MARLIN_TEST(gcode, parse_empty_line_resets_state) {
  char previous_command[] = "M118 Hello";
  parser.parse(previous_command);
  TEST_ASSERT_EQUAL(118, parser.codenum);
  TEST_ASSERT_TRUE(parser.has_string());

  char current_command[] = "";
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('?', parser.command_letter);
  TEST_ASSERT_EQUAL(0, parser.codenum);
  TEST_ASSERT_FALSE(parser.has_string());
}

// 'T' is a command letter, not a line number, even though it sorts after 'N'.
MARLIN_TEST(gcode, parse_t_command) {
  char current_command[] = "T0";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('T', parser.command_letter);
  TEST_ASSERT_EQUAL(0, parser.codenum);
}

MARLIN_TEST(gcode, parse_t_command_with_param) {
  char current_command[] = "T1 S1";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_EQUAL('T', parser.command_letter);
  TEST_ASSERT_EQUAL(1, parser.codenum);
  TEST_ASSERT_TRUE(parser.seenval('S'));
}

// Only the first valueless parameter sets the string argument; a later one must
// not replace it.
//
// LEGACY-BEHAVIOR: string_arg is set after spaces have been skipped, so for a
// valueless parameter followed by another token it points at the space before the
// *next* token rather than at the parameter itself — " Y" here, not "X Y". The
// single-parameter case ("G0 X") has no following space and does yield "X".
MARLIN_TEST(gcode, parse_first_valueless_param_wins_string_arg) {
  char current_command[] = "G0 X Y";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_TRUE(parser.seen('X'));
  TEST_ASSERT_TRUE(parser.seen('Y'));
  TEST_ASSERT_EQUAL_STRING(" Y", parser.string_arg);
}

// Runs of spaces between a value and the next parameter are all skipped.
MARLIN_TEST(gcode, parse_multiple_spaces_after_value) {
  char current_command[] = "G0 X10   Y20";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_TRUE(parser.seenval('X'));
  TEST_ASSERT_EQUAL(10, parser.value_int());
  TEST_ASSERT_TRUE(parser.seenval('Y'));
  TEST_ASSERT_EQUAL(20, parser.value_int());
}

// Signed and fractional values are skipped in full when scanning to the next parameter.
MARLIN_TEST(gcode, parse_signed_decimal_values) {
  char current_command[] = "G0 X-10.5 Y20.25 Z-3";
  parser.command_letter = -128;
  parser.codenum = -1;
  parser.parse(current_command);
  TEST_ASSERT_TRUE(parser.seenval('X'));
  TEST_ASSERT_EQUAL_FLOAT(-10.5f, parser.value_float());
  TEST_ASSERT_TRUE(parser.seenval('Y'));
  TEST_ASSERT_EQUAL_FLOAT(20.25f, parser.value_float());
  TEST_ASSERT_TRUE(parser.seenval('Z'));
  TEST_ASSERT_EQUAL_FLOAT(-3.0f, parser.value_float());
}
