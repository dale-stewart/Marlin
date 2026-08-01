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
 * Characterization tests for numtostr.
 *
 * These record what the conversions currently produce, including results that look
 * wrong — see the LEGACY-BEHAVIOR notes. Nothing here asserts what the functions
 * *should* do; that is a separate conversation, and changing the output would change
 * what several displays render.
 *
 * NOTE: every function in numtostr writes into one shared global buffer and returns a
 * pointer into it, so a result must be consumed before the next call. Each assertion
 * below therefore covers a single call.
 */

#include "../test/unit_tests.h"
#include "src/libs/numtostr.h"

MARLIN_TEST(numtostr, ui8tostr2) {
  TEST_ASSERT_EQUAL_STRING("00", ui8tostr2(0));
  TEST_ASSERT_EQUAL_STRING("01", ui8tostr2(1));
  TEST_ASSERT_EQUAL_STRING("09", ui8tostr2(9));
  TEST_ASSERT_EQUAL_STRING("10", ui8tostr2(10));
  TEST_ASSERT_EQUAL_STRING("99", ui8tostr2(99));
  // LEGACY-BEHAVIOR: a two-digit field keeps the low digits and drops the rest.
  TEST_ASSERT_EQUAL_STRING("00", ui8tostr2(100));
  TEST_ASSERT_EQUAL_STRING("55", ui8tostr2(255));
}

MARLIN_TEST(numtostr, ui8tostr3rj) {
  TEST_ASSERT_EQUAL_STRING("  0", ui8tostr3rj(0));
  TEST_ASSERT_EQUAL_STRING("  9", ui8tostr3rj(9));
  TEST_ASSERT_EQUAL_STRING(" 10", ui8tostr3rj(10));
  TEST_ASSERT_EQUAL_STRING(" 99", ui8tostr3rj(99));
  TEST_ASSERT_EQUAL_STRING("100", ui8tostr3rj(100));
  TEST_ASSERT_EQUAL_STRING("255", ui8tostr3rj(255));
}

MARLIN_TEST(numtostr, pcttostrpctrj) {
  TEST_ASSERT_EQUAL_STRING("  0%", pcttostrpctrj(0));
  TEST_ASSERT_EQUAL_STRING("  9%", pcttostrpctrj(9));
  TEST_ASSERT_EQUAL_STRING(" 10%", pcttostrpctrj(10));
  TEST_ASSERT_EQUAL_STRING("100%", pcttostrpctrj(100));
  // LEGACY-BEHAVIOR: the value is not clamped, so above 100 it still prints.
  TEST_ASSERT_EQUAL_STRING("123%", pcttostrpctrj(123));
}

// 0-255 is rescaled to 0-100%.
MARLIN_TEST(numtostr, ui8tostr4pctrj) {
  TEST_ASSERT_EQUAL_STRING("  0%", ui8tostr4pctrj(0));
  TEST_ASSERT_EQUAL_STRING("  0%", ui8tostr4pctrj(1));
  TEST_ASSERT_EQUAL_STRING("  4%", ui8tostr4pctrj(10));
  TEST_ASSERT_EQUAL_STRING(" 39%", ui8tostr4pctrj(100));
  TEST_ASSERT_EQUAL_STRING("100%", ui8tostr4pctrj(255));
}

MARLIN_TEST(numtostr, i8tostr3rj) {
  TEST_ASSERT_EQUAL_STRING("  0", i8tostr3rj(0));
  TEST_ASSERT_EQUAL_STRING("  7", i8tostr3rj(7));
  TEST_ASSERT_EQUAL_STRING(" 99", i8tostr3rj(99));
  TEST_ASSERT_EQUAL_STRING("127", i8tostr3rj(127));
  // A negative sign takes the hundreds column, so only two digits remain.
  TEST_ASSERT_EQUAL_STRING("- 1", i8tostr3rj(-1));
  TEST_ASSERT_EQUAL_STRING("- 7", i8tostr3rj(-7));
  TEST_ASSERT_EQUAL_STRING("-99", i8tostr3rj(-99));
  // LEGACY-BEHAVIOR: -128 needs a sign and three digits but the field holds three
  // characters, so the hundreds digit is lost and this reads as -28.
  TEST_ASSERT_EQUAL_STRING("-28", i8tostr3rj(-128));
}

MARLIN_TEST(numtostr, ui16tostr_right_justified) {
  TEST_ASSERT_EQUAL_STRING("  0", ui16tostr3rj(0));
  TEST_ASSERT_EQUAL_STRING(" 42", ui16tostr3rj(42));
  TEST_ASSERT_EQUAL_STRING("999", ui16tostr3rj(999));
  TEST_ASSERT_EQUAL_STRING("   0", ui16tostr4rj(0));
  TEST_ASSERT_EQUAL_STRING("1000", ui16tostr4rj(1000));
  TEST_ASSERT_EQUAL_STRING("    0", ui16tostr5rj(0));
  TEST_ASSERT_EQUAL_STRING("12345", ui16tostr5rj(12345));
  TEST_ASSERT_EQUAL_STRING("65535", ui16tostr5rj(65535));
}

// LEGACY-BEHAVIOR: a value too wide for its field is truncated to the low digits with
// no indication — 1000 in a three-wide field reads as 000, not 999 or an overflow mark.
MARLIN_TEST(numtostr, ui16tostr_overflows_silently) {
  TEST_ASSERT_EQUAL_STRING("000", ui16tostr3rj(1000));
  TEST_ASSERT_EQUAL_STRING("345", ui16tostr3rj(12345));
  TEST_ASSERT_EQUAL_STRING("2345", ui16tostr4rj(12345));
  TEST_ASSERT_EQUAL_STRING("5535", ui16tostr4rj(65535));
}

MARLIN_TEST(numtostr, i16tostr3rj) {
  TEST_ASSERT_EQUAL_STRING("  0", i16tostr3rj(0));
  TEST_ASSERT_EQUAL_STRING(" 42", i16tostr3rj(42));
  TEST_ASSERT_EQUAL_STRING("999", i16tostr3rj(999));
  TEST_ASSERT_EQUAL_STRING("- 5", i16tostr3rj(-5));
  TEST_ASSERT_EQUAL_STRING("-42", i16tostr3rj(-42));
}

MARLIN_TEST(numtostr, i16tostr4signrj) {
  TEST_ASSERT_EQUAL_STRING("   0", i16tostr4signrj(0));
  TEST_ASSERT_EQUAL_STRING("  42", i16tostr4signrj(42));
  TEST_ASSERT_EQUAL_STRING("  -5", i16tostr4signrj(-5));
  TEST_ASSERT_EQUAL_STRING(" -42", i16tostr4signrj(-42));
  TEST_ASSERT_EQUAL_STRING("1234", i16tostr4signrj(1234));
}

// Left-justified, no padding: the width grows with the value.
MARLIN_TEST(numtostr, i16tostr3left) {
  TEST_ASSERT_EQUAL_STRING("0", i16tostr3left(0));
  TEST_ASSERT_EQUAL_STRING("42", i16tostr3left(42));
  TEST_ASSERT_EQUAL_STRING("999", i16tostr3left(999));
}

// LEGACY-BEHAVIOR: i16tostr3left takes an int16_t but cannot represent a negative.
// Each digit is produced as '0' + (n % 10), so a negative yields a character *below*
// '0' — -5 renders as "+" (ASCII 43), not "-5". Callers currently pass values that are
// non-negative in practice (print counts, plot coordinates, an already-negated
// temperature), so this is latent rather than live. Recorded, not fixed: several
// displays render whatever this returns.
MARLIN_TEST(numtostr, i16tostr3left_is_wrong_for_negatives) {
  TEST_ASSERT_EQUAL_STRING("+", i16tostr3left(-5));
  TEST_ASSERT_EQUAL_STRING("/", i16tostr3left(-1));
}

MARLIN_TEST(numtostr, ftostr_no_sign) {
  TEST_ASSERT_EQUAL_STRING("0.0", ftostr11ns(0.0f));
  TEST_ASSERT_EQUAL_STRING("1.5", ftostr11ns(1.5f));
  TEST_ASSERT_EQUAL_STRING("0.00", ftostr12ns(0.0f));
  TEST_ASSERT_EQUAL_STRING("1.50", ftostr12ns(1.5f));
  TEST_ASSERT_EQUAL_STRING("00.0", ftostr31ns(0.0f));
  TEST_ASSERT_EQUAL_STRING("12.3", ftostr31ns(12.345f));
  TEST_ASSERT_EQUAL_STRING("000.0", ftostr41ns(0.0f));
  TEST_ASSERT_EQUAL_STRING("012.3", ftostr41ns(12.345f));
}

// LEGACY-BEHAVIOR: the "ns" (no sign) conversions drop the sign entirely rather than
// clamping or marking it, so a negative renders identically to its absolute value.
MARLIN_TEST(numtostr, ftostr_no_sign_loses_negatives) {
  TEST_ASSERT_EQUAL_STRING("1.5", ftostr11ns(-1.5f));
  TEST_ASSERT_EQUAL_STRING("1.50", ftostr12ns(-1.5f));
  TEST_ASSERT_EQUAL_STRING("12.3", ftostr31ns(-12.345f));
}

MARLIN_TEST(numtostr, ftostr_signed) {
  TEST_ASSERT_EQUAL_STRING("+00.0", ftostr31sign(0.0f));
  TEST_ASSERT_EQUAL_STRING("+12.3", ftostr31sign(12.345f));
  TEST_ASSERT_EQUAL_STRING("-12.3", ftostr31sign(-12.345f));
  TEST_ASSERT_EQUAL_STRING("+0000.0", ftostr51sign(0.0f));
  TEST_ASSERT_EQUAL_STRING("-0012.3", ftostr51sign(-12.345f));
  TEST_ASSERT_EQUAL_STRING("+000.00", ftostr52sign(0.0f));
  TEST_ASSERT_EQUAL_STRING("-012.35", ftostr52sign(-12.345f));
}

MARLIN_TEST(numtostr, ftostr_space_padded) {
  TEST_ASSERT_EQUAL_STRING(" 0.00", ftostr42_52(0.0f));
  TEST_ASSERT_EQUAL_STRING("-1.50", ftostr42_52(-1.5f));
  // Widens to six characters once the value no longer fits.
  TEST_ASSERT_EQUAL_STRING("123.46", ftostr42_52(123.456f));
  TEST_ASSERT_EQUAL_STRING("000.00", ftostr52(0.0f));
  TEST_ASSERT_EQUAL_STRING("-01.50", ftostr52(-1.5f));
  TEST_ASSERT_EQUAL_STRING(" 0.000", ftostr43sign(0.0f));
  TEST_ASSERT_EQUAL_STRING("-1.500", ftostr43sign(-1.5f));
}

// Trailing zeros are blanked, so the result is a fixed width with spaces inside it.
MARLIN_TEST(numtostr, ftostr52sp_blanks_trailing_zeros) {
  TEST_ASSERT_EQUAL_STRING("   0   ", ftostr52sp(0.0f));
  TEST_ASSERT_EQUAL_STRING("   1.5 ", ftostr52sp(1.5f));
  TEST_ASSERT_EQUAL_STRING("  12.35", ftostr52sp(12.345f));
  TEST_ASSERT_EQUAL_STRING("-  1.5 ", ftostr52sp(-1.5f));
}

MARLIN_TEST(numtostr, ftostr_right_justified) {
  TEST_ASSERT_EQUAL_STRING("   0.0", ftostr51rj(0.0f));
  TEST_ASSERT_EQUAL_STRING("  12.3", ftostr51rj(12.345f));
  TEST_ASSERT_EQUAL_STRING(" 123.5", ftostr51rj(123.456f));
  TEST_ASSERT_EQUAL_STRING("0.00", ftostr32rj(0.0f));
  TEST_ASSERT_EQUAL_STRING("3.46", ftostr32rj(123.456f));
}

// Rounding is half-away-from-zero at the last kept digit, and carries into the
// integer part.
MARLIN_TEST(numtostr, rounding) {
  TEST_ASSERT_EQUAL_STRING("99.99", ftostr42_52(99.99f));
  TEST_ASSERT_EQUAL_STRING("100.0", ftostr41ns(99.99f));
  TEST_ASSERT_EQUAL_STRING(" 100.0", ftostr51rj(99.99f));
  TEST_ASSERT_EQUAL_STRING("123.46", ftostr52(123.456f));
  TEST_ASSERT_EQUAL_STRING("3.46", ftostr32rj(123.456f));
}
