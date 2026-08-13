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
 * How the firmware writes to the host.
 *
 * These are the primitives every report in the machine is assembled from — the `echo:` and
 * `Error:` prefixes a host filters its terminal on, the signed offsets and padded columns
 * that keep a position report readable, the axis-labelled list that `M114` and `M503` and
 * the levelling code all print through.
 *
 * They are worth testing for a reason beyond their own correctness: **this is the instrument
 * every other test in the suite asserts through.** Dozens of tests elsewhere search a
 * captured serial stream for a word. If the word is right but the prefix is wrong, or the
 * axis labels are transposed, those tests still pass and the machine still lies to its host.
 * A fault here weakens assertions everywhere at once while looking like a formatting detail.
 *
 * Almost all of it is pure — no fixtures, no hardware, no clock — so the cost is low and the
 * assertions can be exact.
 */

#include "../test/unit_tests.h"
#include "src/core/serial.h"
#include "../gcode/serial_capture.h"
#include <string>
#include <stdio.h>

namespace {

  // Run one write and return everything it put on the wire.
  template <typename Writer>
  std::string written_by(Writer &&write) {
    SerialCapture host;
    write();
    return host.finish();
  }

}

/**
 * The three prefixes are three different words.
 *
 * A host filters its terminal on them: `echo:` is chatter, `Error:` is a failure, `Warning:`
 * is neither. Asserted as distinct strings rather than one at a time, because the failure
 * that matters is not an absent prefix but the *wrong* one — an error reported as chatter is
 * an error nobody sees, and `probe.cpp` has already cost this fork a test that passed
 * against a deleted failure path because it searched for the words and not the channel.
 */
MARLIN_TEST(serial_formatting, the_prefixes_tell_a_host_what_kind_of_line_it_is) {
  const std::string chatter = written_by([]{ SERIAL_ECHO_START(); }),
                    failure = written_by([]{ SERIAL_ERROR_START(); }),
                    caution = written_by([]{ SERIAL_WARN_START(); });

  TEST_ASSERT_EQUAL_STRING_MESSAGE("echo:", chatter.c_str(),
    "ordinary chatter should be prefixed so a host can filter it out");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Error:", failure.c_str(),
    "a failure should be prefixed so a host can surface it");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Warning:", caution.c_str(),
    "and a warning should be neither of the other two");
}

// Padding is how a report keeps its columns. Bracketed at zero, because a version that
// always wrote one space would satisfy any test that only asked for several.
MARLIN_TEST(serial_formatting, padding_writes_the_number_of_spaces_asked_for) {
  const std::string none = written_by([]{ SERIAL_ECHO_SP(0); }),
                    three = written_by([]{ SERIAL_ECHO_SP(3); });

  TEST_ASSERT_EQUAL_MESSAGE(0, none.size(), "no padding should write nothing at all");
  TEST_ASSERT_EQUAL_MESSAGE(3 * (PROPORTIONAL_FONT_RATIO), three.size(),
    "and three columns of padding should be three spaces wide");
  TEST_ASSERT_EQUAL_MESSAGE(std::string::npos, three.find_first_not_of(' '),
    "and be spaces, not some other filler");
}

/**
 * A signed offset says which way, and says it consistently.
 *
 * `serial_offset(v, sp)` exists so a column of offsets lines up: a positive value gets an
 * explicit `+` so it occupies the same width as a negative one's `-`. Zero is the awkward
 * case and the caller chooses — nothing, a space, or a plus — which is what `sp` is for.
 *
 * All five combinations, because the branch is a three-way decision on two inputs and each
 * arm is reachable from a real caller. A version that dropped the `+` would print a column
 * that still parses and no longer aligns; one that dropped the space for zero would misalign
 * only the zero rows, which is the version nobody notices.
 */
MARLIN_TEST(serial_formatting, an_offset_is_signed_so_a_column_of_them_lines_up) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("+1.50",
    written_by([]{ serial_offset(1.5f); }).c_str(),
    "a positive offset should carry an explicit plus, to match a negative's minus");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("-1.50",
    written_by([]{ serial_offset(-1.5f); }).c_str(),
    "and a negative its own sign, once");
}

/**
 * The zero rule applies to zero only.
 *
 * `sp` says how *zero* should be written, and the non-zero cases must ignore it — a value
 * that already has a sign does not also want a space in front of it, and one that is
 * positive does not want two plusses. The two conditions are `v == 0 && sp == 1` and
 * `v > 0 || (v == 0 && sp == 2)`, and each has mutants that only disagree when the sign and
 * `sp` are varied *together*: relaxing either equality to `<=` or `>=` is invisible until a
 * non-zero value is passed with a non-zero `sp`.
 */
MARLIN_TEST(serial_formatting, the_zero_rule_does_not_leak_into_signed_values) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("+1.50",
    written_by([]{ serial_offset(1.5f, 1); }).c_str(),
    "asking for a space in place of zero's sign must not take a positive value's plus away");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("-1.50",
    written_by([]{ serial_offset(-1.5f, 1); }).c_str(),
    "nor put a space in front of a negative one, which already has its sign");

  TEST_ASSERT_EQUAL_STRING_MESSAGE("+1.50",
    written_by([]{ serial_offset(1.5f, 2); }).c_str(),
    "asking for a plus in place of zero's sign leaves a positive value with exactly one");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("-1.50",
    written_by([]{ serial_offset(-1.5f, 2); }).c_str(),
    "and must not put one in front of a minus");
}

// A value between zero and one is still positive. Guards against a threshold that drifted
// off zero — which would leave sub-millimetre offsets unsigned and the column ragged.
MARLIN_TEST(serial_formatting, a_small_positive_offset_is_still_signed) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("+0.50",
    written_by([]{ serial_offset(0.5f); }).c_str(),
    "anything above zero gets the plus, not just anything above one");
}

// `sp` is a choice between three named behaviours, not a magnitude.
MARLIN_TEST(serial_formatting, an_unrecognised_zero_style_is_treated_as_the_default) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("0.00",
    written_by([]{ serial_offset(0.0f, 3); }).c_str(),
    "a style beyond the two defined ones should fall back to plain, not to the last one");
}

MARLIN_TEST(serial_formatting, the_caller_chooses_how_zero_is_written) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("0.00",
    written_by([]{ serial_offset(0.0f, 0); }).c_str(),
    "by default zero is unsigned and unpadded");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(" 0.00",
    written_by([]{ serial_offset(0.0f, 1); }).c_str(),
    "a caller aligning a column asks for a space where the sign would be");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("+0.00",
    written_by([]{ serial_offset(0.0f, 2); }).c_str(),
    "and one that wants every row signed asks for a plus");
}

/**
 * The either-or helper picks one word and only one.
 *
 * `serial_ternary(pre, onoff, on, off, post)` is how settings are reported — "Filament
 * runout: ON". Each of the four parts is separately optional, and each has its own guard, so
 * the interesting failures are a version that prints both words, or neither, or drops the
 * label and leaves the value hanging.
 */
MARLIN_TEST(serial_formatting, the_either_or_helper_prints_one_word_not_both) {
  const std::string when_on = written_by([]{
                      serial_ternary(F("State: "), true, F("ON"), F("OFF"), F("!"));
                    }),
                    when_off = written_by([]{
                      serial_ternary(F("State: "), false, F("ON"), F("OFF"), F("!"));
                    });

  TEST_ASSERT_EQUAL_STRING_MESSAGE("State: ON!", when_on.c_str(),
    "the true case should print the label, the on word, and the tail - and not the off word");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("State: OFF!", when_off.c_str(),
    "and the false case the other word, in the same frame");
}

// Every part is optional on its own, which is four guards nothing else distinguishes.
MARLIN_TEST(serial_formatting, each_part_of_the_either_or_helper_can_be_left_out) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ON",
    written_by([]{ serial_ternary(nullptr, true, F("ON"), F("OFF")); }).c_str(),
    "with no label and no tail, only the chosen word is written");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("State: ",
    written_by([]{ serial_ternary(F("State: "), true, nullptr, F("OFF")); }).c_str(),
    "a true case with no word to print prints the label and stops");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("State: ",
    written_by([]{ serial_ternary(F("State: "), false, F("ON"), nullptr); }).c_str(),
    "and so does a false one");
}

/**
 * A binary dump is sixteen digits, grouped so a person can read them.
 *
 * `print_bin()` is a debugging aid for bit masks — which axes are enabled, which endstops
 * are wired. The grouping is the whole point: sixteen ungrouped digits are unreadable, and
 * a group boundary in the wrong place makes a mask look like a different mask.
 *
 * Asserted on a value with every nibble different, so a fault in the bit order, the digit
 * choice or the spacing shows up as a different string rather than a coincidence.
 */
MARLIN_TEST(serial_formatting, a_binary_dump_is_grouped_into_readable_nibbles) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("0001 0010 0100 1000",
    written_by([]{ print_bin(0x1248); }).c_str(),
    "sixteen bits, most significant first, in groups of four with no trailing space");

  TEST_ASSERT_EQUAL_STRING_MESSAGE("0000 0000 0000 0000",
    written_by([]{ print_bin(0); }).c_str(),
    "and a zero mask is still sixteen digits, not an empty line");
}

/**
 * A position report labels each axis with its own value.
 *
 * This is what `M114` and every levelling report print through, and the failure it must not
 * have is a transposition: X's value under Y's label reads as a perfectly plausible machine
 * that is not where it says it is. Each axis therefore gets a distinct value, so a swap
 * changes the output rather than merely reordering identical numbers.
 */
MARLIN_TEST(serial_formatting, a_position_report_puts_each_value_under_its_own_label) {
  const std::string sent = written_by([]{
    print_xyz(NUM_AXIS_LIST_(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f) nullptr, F(""));
  });

  TEST_ASSERT_TRUE_MESSAGE(sent.find("X1.00") != std::string::npos,
    "X should carry X's value");
  TEST_ASSERT_TRUE_MESSAGE(sent.find("Y2.00") != std::string::npos,
    "and Y its own, not X's");
  TEST_ASSERT_TRUE_MESSAGE(sent.find("Z3.00") != std::string::npos,
    "and Z its own");

  TEST_ASSERT_TRUE_MESSAGE(sent.find("X1") < sent.find("Y2"),
    "and they should be listed in axis order");
  TEST_ASSERT_TRUE_MESSAGE(sent.find("Y2") < sent.find("Z3"), "X, then Y, then Z");
}

/**
 * The prefix and the suffix are both optional, and their absence means different things.
 *
 * No prefix means the report starts with the first axis; no suffix means it ends the line.
 * That second one is load-bearing — a caller who passes a suffix is continuing the line, and
 * a stray newline in the middle splits one report into two that a host reads as two.
 */
MARLIN_TEST(serial_formatting, a_position_report_ends_the_line_unless_given_a_suffix) {
  const std::string bare = written_by([]{
    print_xyz(NUM_AXIS_LIST_(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f) nullptr);
  });
  const std::string continued = written_by([]{
    print_xyz(NUM_AXIS_LIST_(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f) nullptr, F(" more"));
  });

  TEST_ASSERT_TRUE_MESSAGE(!bare.empty() && bare.back() == '\n',
    "with nothing to follow it, a report should end its line");
  TEST_ASSERT_TRUE_MESSAGE(continued.find(" more") != std::string::npos,
    "with a suffix, the suffix should be written");
  TEST_ASSERT_TRUE_MESSAGE(continued.find('\n') == std::string::npos,
    "and the line left open, because the caller has more to say");
}

MARLIN_TEST(serial_formatting, a_position_report_writes_its_prefix_first) {
  const std::string sent = written_by([]{
    print_xyz(NUM_AXIS_LIST_(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f) F("Position"), F(""));
  });

  TEST_ASSERT_TRUE_MESSAGE(sent.rfind("Position", 0) == 0,
    "a prefix should open the report, not appear somewhere inside it");
}

#if HAS_EXTRUDERS

// The extruder is reported after the motion axes, by a separate function with its own copy
// of the prefix and suffix handling.
MARLIN_TEST(serial_formatting, a_position_report_with_the_extruder_names_it_last) {
  const std::string sent = written_by([]{
    print_xyze(LOGICAL_AXIS_LIST_(9.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f) nullptr, F(""));
  });

  TEST_ASSERT_TRUE_MESSAGE(sent.find("E9.50") != std::string::npos,
    "the extruder position should be reported with its own value");
  TEST_ASSERT_TRUE_MESSAGE(sent.find("X1") < sent.find("E9"),
    "and after the motion axes, which is where a host expects it");

  // The motion axes are printed by a second call site with its own argument list, so the
  // transposition this guards against has to be guarded against twice.
  TEST_ASSERT_TRUE_MESSAGE(sent.find("X1.00") != std::string::npos, "X should carry X's value here too");
  TEST_ASSERT_TRUE_MESSAGE(sent.find("Y2.00") != std::string::npos, "and Y its own");
  TEST_ASSERT_TRUE_MESSAGE(sent.find("Z3.00") != std::string::npos, "and Z its own");
}

// ...and its own copy of the line-ending decision.
MARLIN_TEST(serial_formatting, a_report_with_the_extruder_ends_the_line_unless_given_a_suffix) {
  const std::string bare = written_by([]{
    print_xyze(LOGICAL_AXIS_LIST_(9.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f) nullptr);
  });
  const std::string continued = written_by([]{
    print_xyze(LOGICAL_AXIS_LIST_(9.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f) nullptr, F(" more"));
  });

  TEST_ASSERT_TRUE_MESSAGE(!bare.empty() && bare.back() == '\n',
    "with nothing to follow it, the report should end its line");
  TEST_ASSERT_TRUE_MESSAGE(continued.find(" more") != std::string::npos,
    "with a suffix, the suffix should be written");
  TEST_ASSERT_TRUE_MESSAGE(continued.find('\n') == std::string::npos,
    "and the line left open");
}

#endif // HAS_EXTRUDERS
