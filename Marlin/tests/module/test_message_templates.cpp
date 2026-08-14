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
 * How a message template becomes a message.
 *
 * Every localised string that has to name something — a tool, an axis, a value — is written
 * with a placeholder, and `expand_u8str_P()` is what fills it in. The contract is stated at the
 * top of `language_en.h`, which is the nearest thing this has to a specification:
 *
 *     $ displays an inserted string
 *     { displays  '0'....'10' for indexes 0 - 10
 *     ~ displays  '1'....'11' for indexes 0 - 10
 *     * displays 'E1'...'E11' for indexes 0 - 10 (By default. Uses LCD_FIRST_TOOL)
 *     @ displays an axis name such as XYZUVW, or E for an extruder
 *
 * Four different numberings of the same tool, which is exactly the kind of thing that is easy
 * to get wrong and hard to notice: `{` is zero-based because it indexes an array, `~` and `*`
 * are one-based because that is what the machine calls the tool on its front panel. A fault
 * here does not crash anything — it tells somebody to check the wrong nozzle, or moves the
 * wrong axis in a prompt they are about to agree to.
 *
 * It is a pure function over a template and an index, so every case below is an exact string
 * rather than a substring. That is worth having: `contains("E1")` is satisfied by `"E11"`, and
 * the off-by-one between the three tool forms is precisely what a loose assertion would miss.
 *
 * Templates are written here as literals rather than taken from `language_en.h`, because
 * asserting `MSG_MOVE_N` against `MSG_MOVE_N` would be comparing the code to itself. What is
 * pinned is the *substitution rule*, which is the part every one of those messages depends on.
 */

#include "../test/unit_tests.h"
#include "src/lcd/marlinui.h"
#include <string.h>

namespace {

  /**
   * Expand a template and return exactly what came out.
   *
   * The length is stated here rather than left to default to `MAX_MESSAGE_SIZE`, and that is
   * not a detail: **`MAX_MESSAGE_SIZE` is 1 on a machine with no display** — the fallback arm
   * of `Conditionals-2-LCD.h` — so a test that inherited it would assert that "Tool 0" comes
   * out as "T" and would be measuring the configuration rather than the substitution rule.
   * The rule is the same on every machine; the room to print it is not.
   */
  constexpr uint8_t ROOM_ENOUGH = 63;

  std::string expanded(const char * const tpl, const int8_t ind,
                       const char *cstr = nullptr, FSTR_P const fstr = nullptr,
                       const uint8_t maxlen = ROOM_ENOUGH) {
    char out[ROOM_ENOUGH * 2] = { 0 };
    expand_u8str_P(out, tpl, ind, cstr, fstr, maxlen);
    return std::string(out);
  }

}

// ---------------------------------------------------------------------------
// Naming a tool: three placeholders, three numberings
// ---------------------------------------------------------------------------

/**
 * `{` is the index as the firmware holds it — zero-based.
 *
 * This is the form used where the number has to line up with an array subscript or a `T`
 * command, so it must not be shifted for display.
 */
MARLIN_TEST(message_templates, brace_names_the_tool_by_its_zero_based_index) {
  TEST_ASSERT_EQUAL_STRING("Tool 0", expanded("Tool {", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("Tool 1", expanded("Tool {", 1).c_str());
}

/**
 * `~` is the index as a person counts — one-based.
 *
 * The same tool, one higher, because the front panel calls the first extruder "1". Asserting
 * either form alone would pass with both implemented the same way, which is the fault worth
 * catching: a machine that told the user to clear a jam in tool 1 when it meant tool 2.
 */
MARLIN_TEST(message_templates, tilde_names_the_tool_as_a_person_counts_them) {
  TEST_ASSERT_EQUAL_STRING("Tool 1", expanded("Tool ~", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("Tool 2", expanded("Tool ~", 1).c_str());

  // ...and the two forms disagree by exactly one for the same index.
  TEST_ASSERT_EQUAL_STRING("0", expanded("{", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("1", expanded("~", 0).c_str());
}

/**
 * `*` is the one-based number with an `E` in front of it.
 *
 * The form used where a message has to name a *heater* rather than a tool. It is `~` with a
 * prefix, so a test that only checked the digit would pass with the `E` missing — which is the
 * difference between "E1 too hot" and "1 too hot" in an error a person has to act on.
 */
MARLIN_TEST(message_templates, star_names_the_heater_with_its_letter) {
  TEST_ASSERT_EQUAL_STRING("E1", expanded("*", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("E2", expanded("*", 1).c_str());
}

/**
 * Two digits are both emitted, and in order.
 *
 * The expander divides by ten by hand rather than calling a formatter, so the tens digit is a
 * separate statement from the units digit. Ten is the first index that reaches it, and it is
 * also where a machine with many tools starts mattering. Reversing the two would give "01",
 * which is a plausible-looking string that names a tool that does not exist.
 */
MARLIN_TEST(message_templates, a_two_digit_tool_number_comes_out_in_order) {
  TEST_ASSERT_EQUAL_STRING("10", expanded("{", 10).c_str());
  TEST_ASSERT_EQUAL_STRING("11", expanded("~", 10).c_str());
  TEST_ASSERT_EQUAL_STRING("E11", expanded("*", 10).c_str());

  // Nine is the last single-digit case: the boundary either side of the tens branch.
  TEST_ASSERT_EQUAL_STRING("9", expanded("{", 9).c_str());
  TEST_ASSERT_EQUAL_STRING("E10", expanded("*", 9).c_str());
}

/**
 * A negative index names the bed or the chamber instead of a tool.
 *
 * The same templates serve heaters that have no number, and the expander answers with a word
 * rather than a digit. Two different negatives mean two different things, which is the whole
 * reason it is not simply "not a tool" — and a machine that said "Bed" for the chamber would
 * send somebody to the wrong part of the printer.
 */
MARLIN_TEST(message_templates, a_negative_index_names_the_bed_or_the_chamber) {
  TEST_ASSERT_EQUAL_STRING(GET_TEXT(MSG_BED), expanded("{", -1).c_str());
  TEST_ASSERT_EQUAL_STRING(GET_TEXT(MSG_CHAMBER), expanded("{", -2).c_str());
}

// ---------------------------------------------------------------------------
// The other substitutions
// ---------------------------------------------------------------------------

/**
 * `@` is the axis letter.
 *
 * Used by the move menu, where the difference between "Move X" and "Move Y" is the difference
 * between two machines' worth of travel. The index is an axis here rather than a tool, which
 * is the same argument meaning something else depending on the template — worth pinning
 * because nothing in the signature says so.
 */
MARLIN_TEST(message_templates, at_names_the_axis) {
  TEST_ASSERT_EQUAL_STRING("Move X", expanded("Move @", X_AXIS).c_str());
  #if HAS_Y_AXIS
    TEST_ASSERT_EQUAL_STRING("Move Y", expanded("Move @", Y_AXIS).c_str());
  #endif
  #if HAS_Z_AXIS
    TEST_ASSERT_EQUAL_STRING("Move Z", expanded("Move @", Z_AXIS).c_str());
  #endif
}

/**
 * `$` inserts a string the caller supplies, from either kind of pointer.
 *
 * Two separate branches — one for a run-time string, one for a program-memory string — with
 * their own copy of the length bookkeeping each. Covering one leaves the other unasserted, and
 * the caller chooses between them by which argument it passes rather than by any flag.
 */
MARLIN_TEST(message_templates, dollar_inserts_the_string_the_caller_gave) {
  TEST_ASSERT_EQUAL_STRING("Move 10mm", expanded("Move $mm", 0, "10").c_str());
  TEST_ASSERT_EQUAL_STRING("Move 10mm", expanded("Move $mm", 0, nullptr, F("10")).c_str());
}

/**
 * A template with no placeholder is copied through unchanged.
 *
 * Most messages have none, so this is the common path and the one a fault would break most
 * loudly — but it is also the case a test suite forgets, because it looks like nothing is
 * happening. The index is deliberately a real tool number: nothing should appear.
 */
MARLIN_TEST(message_templates, a_template_with_no_placeholder_is_left_alone) {
  TEST_ASSERT_EQUAL_STRING("Nozzle", expanded("Nozzle", 1).c_str());
}

/**
 * The expansion stops at `maxlen`, and the return value says how much was written.
 *
 * The buffer belongs to the caller and is the width of a display, so overrunning it is how a
 * status line corrupts whatever follows it in memory. The count is what the display drivers
 * use to decide how much room is left on the row, so it has to agree with the string.
 */
MARLIN_TEST(message_templates, expansion_stops_at_the_length_it_was_given) {
  char out[64] = { 0 };
  const uint8_t written = expand_u8str_P(out, "0123456789", 0, nullptr, nullptr, 4);

  TEST_ASSERT_EQUAL_STRING_MESSAGE("0123", out,
    "expansion should stop at the length the caller allowed");
  TEST_ASSERT_EQUAL_MESSAGE(4, written,
    "and should report how many characters it wrote");
}
