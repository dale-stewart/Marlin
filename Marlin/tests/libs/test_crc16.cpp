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
 * Tests for crc16.
 *
 * Unlike most of this rescue, these are not characterization tests: crc16 implements a
 * standard algorithm, so the expected values come from its published check vectors
 * rather than from the current output. If a change breaks one of these, the
 * implementation has stopped being CRC-16 — it is not a matter of taste.
 *
 * The check value for "123456789" is 0x31C3 seeded with 0x0000 (CRC-16/XMODEM) and
 * 0x29B1 seeded with 0xFFFF (CRC-16/CCITT-FALSE).
 */

#include "../test/unit_tests.h"
#include "src/libs/crc16.h"
#include <string.h>

static uint16_t crc_of(const char * const s, const uint16_t seed = 0) {
  uint16_t crc = seed;
  crc16(&crc, s, strlen(s));
  return crc;
}

MARLIN_TEST(crc16, standard_check_vectors) {
  TEST_ASSERT_EQUAL_HEX16(0x31C3, crc_of("123456789"));
  TEST_ASSERT_EQUAL_HEX16(0x29B1, crc_of("123456789", 0xFFFF));
}

MARLIN_TEST(crc16, empty_input_leaves_the_seed_untouched) {
  TEST_ASSERT_EQUAL_HEX16(0x0000, crc_of(""));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, crc_of("", 0xFFFF));
  TEST_ASSERT_EQUAL_HEX16(0x1234, crc_of("", 0x1234));
}

// The accumulator is carried in and out, so a message can be fed in pieces.
MARLIN_TEST(crc16, is_incremental) {
  uint16_t whole = 0;
  crc16(&whole, "123456789", 9);

  uint16_t pieces = 0;
  crc16(&pieces, "1234", 4);
  crc16(&pieces, "56789", 5);

  TEST_ASSERT_EQUAL_HEX16(whole, pieces);
  TEST_ASSERT_EQUAL_HEX16(0x31C3, pieces);
}

MARLIN_TEST(crc16, detects_the_errors_it_exists_to_detect) {
  // Any single-character difference changes the result.
  TEST_ASSERT_NOT_EQUAL(crc_of("Marlin"), crc_of("marlin"));
  TEST_ASSERT_NOT_EQUAL(crc_of("Marlin"), crc_of("Marlim"));
  // Transposition, which a simple sum would miss.
  TEST_ASSERT_NOT_EQUAL(crc_of("Marlin"), crc_of("Mralin"));
  // Length matters: trailing NULs are not invisible.
  TEST_ASSERT_NOT_EQUAL(crc_of("A"), crc_of("AA"));
}

MARLIN_TEST(crc16, single_bytes) {
  TEST_ASSERT_EQUAL_HEX16(0x58E5, crc_of("A"));
  TEST_ASSERT_EQUAL_HEX16(0xEA13, crc_of("Marlin"));
}
