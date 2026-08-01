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
 * Tests for utf8 — counting and indexing characters in strings that reach the display,
 * and the binary search used to look up glyph ranges.
 *
 * These mostly assert against the UTF-8 standard rather than against current output:
 * the encoding of "é" or "€" is not a matter of local convention. Where behaviour is
 * Marlin's own choice (what a malformed sequence does, what an out-of-range index
 * returns) it is characterized and marked.
 */

#include "../test/unit_tests.h"
#include "src/lcd/utf8.h"
#include <string.h>

// Encodings used below, spelled out so the tests do not depend on the encoding of
// this source file:
//   "é"  U+00E9   C3 A9              (2 bytes)
//   "€"  U+20AC   E2 82 AC           (3 bytes)
//   "😀" U+1F600  F0 9F 98 80        (4 bytes)
static const char ASCII[]    = "abc";
static const char TWO_BYTE[] = "a\xC3\xA9" "b";                  // a é b
static const char THREE[]    = "a\xE2\x82\xAC" "b";              // a € b
static const char FOUR[]     = "a\xF0\x9F\x98\x80" "b";          // a 😀 b
static const char MIXED[]    = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // é € 😀

MARLIN_TEST(utf8, strlen_counts_characters_not_bytes) {
  TEST_ASSERT_EQUAL(3, utf8_strlen(ASCII));
  TEST_ASSERT_EQUAL(3, strlen(ASCII));

  TEST_ASSERT_EQUAL(3, utf8_strlen(TWO_BYTE));
  TEST_ASSERT_EQUAL(4, strlen(TWO_BYTE));

  TEST_ASSERT_EQUAL(3, utf8_strlen(THREE));
  TEST_ASSERT_EQUAL(5, strlen(THREE));

  TEST_ASSERT_EQUAL(3, utf8_strlen(FOUR));
  TEST_ASSERT_EQUAL(6, strlen(FOUR));

  TEST_ASSERT_EQUAL(3, utf8_strlen(MIXED));
  TEST_ASSERT_EQUAL(9, strlen(MIXED));
}

MARLIN_TEST(utf8, strlen_edge_cases) {
  TEST_ASSERT_EQUAL(0, utf8_strlen(""));
  TEST_ASSERT_EQUAL(0, utf8_strlen((const char *)nullptr));   // a null string counts as empty
  TEST_ASSERT_EQUAL(1, utf8_strlen("\xC3\xA9"));
}

// The byte offset where the Nth character starts.
MARLIN_TEST(utf8, byte_pos_by_char_num_ascii) {
  TEST_ASSERT_EQUAL(0, utf8_byte_pos_by_char_num(ASCII, 0));
  TEST_ASSERT_EQUAL(1, utf8_byte_pos_by_char_num(ASCII, 1));
  TEST_ASSERT_EQUAL(2, utf8_byte_pos_by_char_num(ASCII, 2));
}

MARLIN_TEST(utf8, byte_pos_by_char_num_skips_continuation_bytes) {
  // "a é b": character 1 starts at byte 1, character 2 at byte 3.
  TEST_ASSERT_EQUAL(0, utf8_byte_pos_by_char_num(TWO_BYTE, 0));
  TEST_ASSERT_EQUAL(1, utf8_byte_pos_by_char_num(TWO_BYTE, 1));
  TEST_ASSERT_EQUAL(3, utf8_byte_pos_by_char_num(TWO_BYTE, 2));

  // "a € b": the three-byte character pushes 'b' to byte 4.
  TEST_ASSERT_EQUAL(1, utf8_byte_pos_by_char_num(THREE, 1));
  TEST_ASSERT_EQUAL(4, utf8_byte_pos_by_char_num(THREE, 2));

  // "a 😀 b": the four-byte character pushes 'b' to byte 5.
  TEST_ASSERT_EQUAL(1, utf8_byte_pos_by_char_num(FOUR, 1));
  TEST_ASSERT_EQUAL(5, utf8_byte_pos_by_char_num(FOUR, 2));
}

// Asking past the end returns the offset of the terminator, so a caller slicing the
// string gets the whole of it rather than reading beyond it.
MARLIN_TEST(utf8, byte_pos_past_the_end_returns_the_length) {
  TEST_ASSERT_EQUAL(3, utf8_byte_pos_by_char_num(ASCII, 3));
  TEST_ASSERT_EQUAL(3, utf8_byte_pos_by_char_num(ASCII, 99));
  TEST_ASSERT_EQUAL(0, utf8_byte_pos_by_char_num("", 0));
  TEST_ASSERT_EQUAL(0, utf8_byte_pos_by_char_num("", 5));
}

MARLIN_TEST(utf8, decodes_to_code_points) {
  lchar_t ch = 0;
  const uint8_t *next = get_utf8_value_cb((const uint8_t *)ASCII, read_byte_ram, ch);
  TEST_ASSERT_EQUAL(0x61, ch);                                  // 'a'
  TEST_ASSERT_EQUAL(1, next - (const uint8_t *)ASCII);

  next = get_utf8_value_cb((const uint8_t *)"\xC3\xA9", read_byte_ram, ch);
  TEST_ASSERT_EQUAL(0x00E9, ch);                                // é
  TEST_ASSERT_EQUAL(2, next - (const uint8_t *)"\xC3\xA9");

  next = get_utf8_value_cb((const uint8_t *)"\xE2\x82\xAC", read_byte_ram, ch);
  TEST_ASSERT_EQUAL(0x20AC, ch);                                // €
  TEST_ASSERT_EQUAL(3, next - (const uint8_t *)"\xE2\x82\xAC");

  next = get_utf8_value_cb((const uint8_t *)"\xF0\x9F\x98\x80", read_byte_ram, ch);
  TEST_ASSERT_EQUAL(0x1F600, ch);                               // 😀
  TEST_ASSERT_EQUAL(4, next - (const uint8_t *)"\xF0\x9F\x98\x80");
}

MARLIN_TEST(utf8, decoding_advances_through_a_string) {
  const uint8_t *p = (const uint8_t *)MIXED;
  lchar_t ch = 0;
  p = get_utf8_value_cb(p, read_byte_ram, ch);
  TEST_ASSERT_EQUAL(0x00E9, ch);
  p = get_utf8_value_cb(p, read_byte_ram, ch);
  TEST_ASSERT_EQUAL(0x20AC, ch);
  p = get_utf8_value_cb(p, read_byte_ram, ch);
  TEST_ASSERT_EQUAL(0x1F600, ch);
  TEST_ASSERT_EQUAL(0, read_byte_ram(p));                       // at the terminator
}

// LEGACY-BEHAVIOR: a byte sequence that starts mid-character is skipped rather than
// reported. The decoder advances past the continuation bytes and leaves the value at 0,
// so a corrupted string silently loses a character instead of showing a replacement.
MARLIN_TEST(utf8, a_stray_continuation_byte_is_skipped_silently) {
  const uint8_t *p = (const uint8_t *)"\xA9" "b";
  lchar_t ch = 0xFFFF;
  p = get_utf8_value_cb(p, read_byte_ram, ch);
  TEST_ASSERT_EQUAL(0, ch);
  TEST_ASSERT_EQUAL('b', read_byte_ram(p));
}

/**
 * pf_bsearch_r is a generic binary search over caller-owned data. The callback
 * returns data[i] - pin, and the search reports the index of a match, or -1 with
 * ret_idx set to where the value would be inserted.
 */
static int compare_uint8(void *userdata, size_t idx, void *pin) {
  const uint8_t * const arr = (const uint8_t *)userdata;
  return int(arr[idx]) - int(*(const uint8_t *)pin);
}

MARLIN_TEST(utf8, bsearch_finds_a_present_value) {
  uint8_t data[] = { 10, 20, 30, 40, 50 };
  size_t idx = 99;
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t pin = data[i];
    TEST_ASSERT_EQUAL(0, pf_bsearch_r(data, 5, compare_uint8, &pin, &idx));
    TEST_ASSERT_EQUAL(i, idx);
  }
}

MARLIN_TEST(utf8, bsearch_reports_where_a_missing_value_belongs) {
  uint8_t data[] = { 10, 20, 30, 40, 50 };
  size_t idx = 99;

  uint8_t below = 5;
  TEST_ASSERT_EQUAL(-1, pf_bsearch_r(data, 5, compare_uint8, &below, &idx));
  TEST_ASSERT_EQUAL(0, idx);

  uint8_t between = 35;
  TEST_ASSERT_EQUAL(-1, pf_bsearch_r(data, 5, compare_uint8, &between, &idx));
  TEST_ASSERT_EQUAL(3, idx);

  uint8_t above = 99;
  TEST_ASSERT_EQUAL(-1, pf_bsearch_r(data, 5, compare_uint8, &above, &idx));
  TEST_ASSERT_EQUAL(5, idx);
}

MARLIN_TEST(utf8, bsearch_on_an_empty_set) {
  uint8_t data[] = { 0 };
  size_t idx = 99;
  uint8_t pin = 42;
  TEST_ASSERT_EQUAL(-1, pf_bsearch_r(data, 0, compare_uint8, &pin, &idx));
  TEST_ASSERT_EQUAL(0, idx);
}

MARLIN_TEST(utf8, bsearch_on_a_single_element) {
  uint8_t data[] = { 42 };
  size_t idx = 99;

  uint8_t hit = 42;
  TEST_ASSERT_EQUAL(0, pf_bsearch_r(data, 1, compare_uint8, &hit, &idx));
  TEST_ASSERT_EQUAL(0, idx);

  uint8_t miss = 43;
  TEST_ASSERT_EQUAL(-1, pf_bsearch_r(data, 1, compare_uint8, &miss, &idx));
  TEST_ASSERT_EQUAL(1, idx);
}

// The _P variants read through the ROM accessor. On this platform ROM and RAM reads are
// the same, but the pairing must still hold — a display that reads a progmem string
// should count it the same way.
MARLIN_TEST(utf8, rom_variants_match_the_ram_variants) {
  TEST_ASSERT_EQUAL(utf8_strlen(ASCII), utf8_strlen_P(ASCII));
  TEST_ASSERT_EQUAL(utf8_strlen(MIXED), utf8_strlen_P(MIXED));
  TEST_ASSERT_EQUAL(3, utf8_strlen_P(MIXED));
  TEST_ASSERT_EQUAL(0, utf8_strlen_P(""));

  TEST_ASSERT_EQUAL(1, utf8_byte_pos_by_char_num_P(TWO_BYTE, 1));
  TEST_ASSERT_EQUAL(3, utf8_byte_pos_by_char_num_P(TWO_BYTE, 2));
  TEST_ASSERT_EQUAL(4, utf8_byte_pos_by_char_num_P(THREE, 2));
  TEST_ASSERT_EQUAL(3, utf8_byte_pos_by_char_num_P(ASCII, 99));
}

MARLIN_TEST(utf8, read_byte_rom_reads_the_same_bytes_as_ram) {
  const uint8_t * const p = (const uint8_t *)MIXED;
  for (uint8_t i = 0; i < 9; i++)
    TEST_ASSERT_EQUAL(read_byte_ram(p + i), read_byte_rom(p + i));
}

// LEGACY-BEHAVIOR: a lead byte claiming a sequence longer than four bytes (0xFE, 0xFF —
// never valid UTF-8) is skipped by consuming bytes until one is no longer such a lead.
// The character is dropped and the value left at 0, with no indication to the caller.
MARLIN_TEST(utf8, an_over_long_lead_byte_is_skipped) {
  const uint8_t *p = (const uint8_t *)"\xFE" "b";
  lchar_t ch = 0xFFFF;
  p = get_utf8_value_cb(p, read_byte_ram, ch);
  TEST_ASSERT_EQUAL(0, ch);
  TEST_ASSERT_EQUAL('b', read_byte_ram(p));
}

// The insertion index reported for a miss must be usable: inserting there keeps the
// array sorted. These pin each branch of that calculation.
MARLIN_TEST(utf8, bsearch_insertion_index_is_usable) {
  uint8_t data[] = { 10, 20, 30, 40 };
  size_t idx = 99;

  // Below every element, between each adjacent pair, and above every element.
  const uint8_t pins[]     = { 1, 15, 25, 35, 45 };
  const size_t  expected[] = { 0,  1,  2,  3,  4 };
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t pin = pins[i];
    TEST_ASSERT_EQUAL(-1, pf_bsearch_r(data, 4, compare_uint8, &pin, &idx));
    TEST_ASSERT_EQUAL(expected[i], idx);
  }
}

MARLIN_TEST(utf8, bsearch_on_two_elements) {
  uint8_t data[] = { 10, 20 };
  size_t idx = 99;

  uint8_t first = 10;
  TEST_ASSERT_EQUAL(0, pf_bsearch_r(data, 2, compare_uint8, &first, &idx));
  TEST_ASSERT_EQUAL(0, idx);

  uint8_t second = 20;
  TEST_ASSERT_EQUAL(0, pf_bsearch_r(data, 2, compare_uint8, &second, &idx));
  TEST_ASSERT_EQUAL(1, idx);

  uint8_t middle = 15;
  TEST_ASSERT_EQUAL(-1, pf_bsearch_r(data, 2, compare_uint8, &middle, &idx));
  TEST_ASSERT_EQUAL(1, idx);
}

/**
 * Exhaustive check of the search against an independent reference.
 *
 * The index arithmetic is easy to get subtly wrong in a way that the handful of
 * hand-written cases above happen not to distinguish, so this sweeps every array size
 * up to eight and every value that could be searched for — present or absent, below,
 * between and above — and compares against a straightforward linear implementation.
 */
static int reference_search(const uint8_t *data, size_t n, uint8_t pin, size_t *idx) {
  for (size_t i = 0; i < n; i++) {
    if (data[i] == pin) { *idx = i; return 0; }
    if (data[i] > pin) { *idx = i; return -1; }
  }
  *idx = n;
  return -1;
}

MARLIN_TEST(utf8, bsearch_matches_a_linear_search_for_every_small_case) {
  for (size_t n = 1; n <= 8; n++) {
    uint8_t data[8];
    for (size_t i = 0; i < n; i++) data[i] = uint8_t((i + 1) * 2);   // 2,4,6,...

    for (uint8_t pin = 1; pin <= (n + 1) * 2; pin++) {
      size_t got = 999, want = 999;
      uint8_t p = pin;
      const int got_rc = pf_bsearch_r(data, n, compare_uint8, &p, &got);
      const int want_rc = reference_search(data, n, pin, &want);
      TEST_ASSERT_EQUAL(want_rc, got_rc);
      TEST_ASSERT_EQUAL(want, got);
    }
  }
}
