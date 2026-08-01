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
