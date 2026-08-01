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

MARLIN_TEST(numtostr, ftostr_three_decimals) {
  TEST_ASSERT_EQUAL_STRING(" 0.000", ftostr53_63(0.0f));
  TEST_ASSERT_EQUAL_STRING("-1.500", ftostr53_63(-1.5f));
  TEST_ASSERT_EQUAL_STRING("12.345", ftostr53_63(12.345f));
  TEST_ASSERT_EQUAL_STRING("999.990", ftostr53_63(999.99f));
  TEST_ASSERT_EQUAL_STRING("000.000", ftostr63(0.0f));
  TEST_ASSERT_EQUAL_STRING("-01.500", ftostr63(-1.5f));
  TEST_ASSERT_EQUAL_STRING("012.345", ftostr63(12.345f));
  TEST_ASSERT_EQUAL_STRING("-12.345", ftostr63(-12.345f));
}

MARLIN_TEST(numtostr, ftostr_signed_wide) {
  TEST_ASSERT_EQUAL_STRING("+000.0", ftostr41sign(0.0f));
  TEST_ASSERT_EQUAL_STRING("-001.5", ftostr41sign(-1.5f));
  TEST_ASSERT_EQUAL_STRING("+012.3", ftostr41sign(12.345f));
  TEST_ASSERT_EQUAL_STRING(" 00.000", ftostr53sign(0.0f));
  TEST_ASSERT_EQUAL_STRING("-01.500", ftostr53sign(-1.5f));
  TEST_ASSERT_EQUAL_STRING(" 12.345", ftostr53sign(12.345f));
  TEST_ASSERT_EQUAL_STRING(" 0.0000", ftostr54sign(0.0f));
  TEST_ASSERT_EQUAL_STRING("-1.5000", ftostr54sign(-1.5f));
  TEST_ASSERT_EQUAL_STRING(" 2.3450", ftostr54sign(12.345f));
}

// Rounds to a whole number in a five-wide right-justified field.
MARLIN_TEST(numtostr, ftostr5rj) {
  TEST_ASSERT_EQUAL_STRING("    0", ftostr5rj(0.0f));
  TEST_ASSERT_EQUAL_STRING("    2", ftostr5rj(1.5f));
  TEST_ASSERT_EQUAL_STRING("   12", ftostr5rj(12.345f));
  TEST_ASSERT_EQUAL_STRING(" 1000", ftostr5rj(999.99f));
  // LEGACY-BEHAVIOR: unsigned — a negative renders as its magnitude.
  TEST_ASSERT_EQUAL_STRING("    2", ftostr5rj(-1.5f));
}

MARLIN_TEST(numtostr, ftostr_one_decimal_right_justified) {
  TEST_ASSERT_EQUAL_STRING(" 0.0", ftostr31rj(0.0f));
  TEST_ASSERT_EQUAL_STRING("12.3", ftostr31rj(12.345f));
  TEST_ASSERT_EQUAL_STRING("  0.0", ftostr41rj(0.0f));
  TEST_ASSERT_EQUAL_STRING(" 12.3", ftostr41rj(12.345f));
  TEST_ASSERT_EQUAL_STRING("    0.0", ftostr61rj(0.0f));
  TEST_ASSERT_EQUAL_STRING(" 1000.0", ftostr61rj(999.99f));
  TEST_ASSERT_EQUAL_STRING("12345.6", ftostr61rj(12345.6f));
}

MARLIN_TEST(numtostr, ftostr_two_decimals_right_justified) {
  TEST_ASSERT_EQUAL_STRING(" 0.00", ftostr42rj(0.0f));
  TEST_ASSERT_EQUAL_STRING("12.35", ftostr42rj(12.345f));
  TEST_ASSERT_EQUAL_STRING("  0.00", ftostr52rj(0.0f));
  TEST_ASSERT_EQUAL_STRING("999.99", ftostr52rj(999.99f));
  TEST_ASSERT_EQUAL_STRING("   0.00", ftostr62rj(0.0f));
  TEST_ASSERT_EQUAL_STRING("2345.60", ftostr62rj(12345.6f));
  TEST_ASSERT_EQUAL_STRING("    0.00", ftostr72rj(0.0f));
  TEST_ASSERT_EQUAL_STRING("12345.60", ftostr72rj(12345.6f));
}

// Unlike the truncating conversions above, these two saturate at their maximum.
MARLIN_TEST(numtostr, some_conversions_clamp_instead_of_truncating) {
  TEST_ASSERT_EQUAL_STRING(" 999.99", ftostr52sprj(12345.6f));
  TEST_ASSERT_EQUAL_STRING("-999.99", ftostr52sprj(-12345.6f));
  TEST_ASSERT_EQUAL_STRING("999", utostr3(1000));
  TEST_ASSERT_EQUAL_STRING("999", utostr3(65535));
  // ...where the same overflow truncates elsewhere.
  TEST_ASSERT_EQUAL_STRING("345.60", ftostr42_52(12345.6f));
  TEST_ASSERT_EQUAL_STRING("000", ui16tostr3rj(1000));
}

MARLIN_TEST(numtostr, ftostr52sprj) {
  TEST_ASSERT_EQUAL_STRING("   0.00", ftostr52sprj(0.0f));
  TEST_ASSERT_EQUAL_STRING("  -1.50", ftostr52sprj(-1.5f));
  TEST_ASSERT_EQUAL_STRING("  12.35", ftostr52sprj(12.345f));
  TEST_ASSERT_EQUAL_STRING(" -12.35", ftostr52sprj(-12.345f));
}

MARLIN_TEST(numtostr, utostr3) {
  TEST_ASSERT_EQUAL_STRING("0", utostr3(0));
  TEST_ASSERT_EQUAL_STRING("42", utostr3(42));
  TEST_ASSERT_EQUAL_STRING("999", utostr3(999));
}

// The dispatch in ftostr42_52 widens the field once the value leaves -10..100.
MARLIN_TEST(numtostr, ftostr42_52_dispatch) {
  TEST_ASSERT_EQUAL_STRING(" 9.99", ftostr42_52(9.99f));
  TEST_ASSERT_EQUAL_STRING("10.00", ftostr42_52(10.0f));
  TEST_ASSERT_EQUAL_STRING("-9.99", ftostr42_52(-9.99f));
  TEST_ASSERT_EQUAL_STRING("-10.00", ftostr42_52(-10.0f));
  TEST_ASSERT_EQUAL_STRING("100.00", ftostr42_52(100.0f));
}

// LEGACY-BEHAVIOR: the dispatch tests the value before rounding, so a value just under
// the boundary takes the narrow field and then rounds up into a width that no longer
// fits. 99.999 renders as "00.00" — the leading 1 is lost and the result reads as zero
// rather than as a hundred.
MARLIN_TEST(numtostr, ftostr42_52_rounds_past_its_own_boundary) {
  TEST_ASSERT_EQUAL_STRING("00.00", ftostr42_52(99.999f));
  TEST_ASSERT_EQUAL_STRING("99.99", ftostr42_52(99.99f));
}

// The width of i16tostr3left steps at 10 and at 100.
MARLIN_TEST(numtostr, i16tostr3left_width_boundaries) {
  TEST_ASSERT_EQUAL_STRING("9", i16tostr3left(9));
  TEST_ASSERT_EQUAL_STRING("10", i16tostr3left(10));
  TEST_ASSERT_EQUAL_STRING("99", i16tostr3left(99));
  TEST_ASSERT_EQUAL_STRING("100", i16tostr3left(100));
  TEST_ASSERT_EQUAL_STRING("101", i16tostr3left(101));
}

// i16tostr4signrj picks a layout by magnitude: four digits, sign plus three, or
// right-justified with padding.
MARLIN_TEST(numtostr, i16tostr4signrj_layout_boundaries) {
  TEST_ASSERT_EQUAL_STRING("  99", i16tostr4signrj(99));
  TEST_ASSERT_EQUAL_STRING(" 100", i16tostr4signrj(100));
  TEST_ASSERT_EQUAL_STRING(" 999", i16tostr4signrj(999));
  TEST_ASSERT_EQUAL_STRING("1000", i16tostr4signrj(1000));
  TEST_ASSERT_EQUAL_STRING(" -99", i16tostr4signrj(-99));
  TEST_ASSERT_EQUAL_STRING("-100", i16tostr4signrj(-100));
  TEST_ASSERT_EQUAL_STRING("-999", i16tostr4signrj(-999));
  // LEGACY-BEHAVIOR: at -1000 the four-digit branch is taken because the test is on
  // the signed value, but the sign is then written over the thousands digit, so -1000
  // reads as "-000" rather than as a thousand.
  TEST_ASSERT_EQUAL_STRING("-000", i16tostr4signrj(-1000));
}

// ftostr52sprj chooses between three width bands.
MARLIN_TEST(numtostr, ftostr52sprj_bands) {
  TEST_ASSERT_EQUAL_STRING("   9.99", ftostr52sprj(9.99f));
  TEST_ASSERT_EQUAL_STRING("  10.00", ftostr52sprj(10.0f));
  TEST_ASSERT_EQUAL_STRING("  99.99", ftostr52sprj(99.99f));
  TEST_ASSERT_EQUAL_STRING(" 100.00", ftostr52sprj(100.0f));
  TEST_ASSERT_EQUAL_STRING(" 999.99", ftostr52sprj(999.99f));
  TEST_ASSERT_EQUAL_STRING("  -9.99", ftostr52sprj(-9.99f));
  TEST_ASSERT_EQUAL_STRING(" -10.00", ftostr52sprj(-10.0f));
  TEST_ASSERT_EQUAL_STRING(" -99.99", ftostr52sprj(-99.99f));
  TEST_ASSERT_EQUAL_STRING("-100.00", ftostr52sprj(-100.0f));
}

// ftostr53_63 widens outside -10..100, the same dispatch shape as ftostr42_52.
MARLIN_TEST(numtostr, ftostr53_63_dispatch) {
  TEST_ASSERT_EQUAL_STRING(" 9.990", ftostr53_63(9.99f));
  TEST_ASSERT_EQUAL_STRING("10.000", ftostr53_63(10.0f));
  TEST_ASSERT_EQUAL_STRING("-9.990", ftostr53_63(-9.99f));
  TEST_ASSERT_EQUAL_STRING("-10.000", ftostr53_63(-10.0f));
  TEST_ASSERT_EQUAL_STRING("99.990", ftostr53_63(99.99f));
  TEST_ASSERT_EQUAL_STRING("100.000", ftostr53_63(100.0f));
}

// Rounding is applied at the last kept digit, away from zero, for both signs.
MARLIN_TEST(numtostr, rounding_half_away_from_zero) {
  TEST_ASSERT_EQUAL_STRING("0.1", ftostr11ns(0.05f));
  TEST_ASSERT_EQUAL_STRING("0.2", ftostr11ns(0.15f));
  TEST_ASSERT_EQUAL_STRING("+00.1", ftostr31sign(0.05f));
  TEST_ASSERT_EQUAL_STRING("-00.1", ftostr31sign(-0.05f));
  TEST_ASSERT_EQUAL_STRING("+000.06", ftostr52sign(0.055f));
  TEST_ASSERT_EQUAL_STRING("-000.06", ftostr52sign(-0.055f));
  TEST_ASSERT_EQUAL_STRING("    1", ftostr5rj(0.5f));
  TEST_ASSERT_EQUAL_STRING("    0", ftostr5rj(0.4f));
}

// Values wide enough to reach the ten-thousands column of the right-justified fields.
MARLIN_TEST(numtostr, wide_values_fill_every_column) {
  TEST_ASSERT_EQUAL_STRING(" 9999", ui16tostr5rj(9999));
  TEST_ASSERT_EQUAL_STRING("10000", ui16tostr5rj(10000));
  TEST_ASSERT_EQUAL_STRING("9999", ui16tostr4rj(9999));
  TEST_ASSERT_EQUAL_STRING("+9999.9", ftostr51sign(9999.9f));
  TEST_ASSERT_EQUAL_STRING("+123.46", ftostr52sign(123.456f));
  TEST_ASSERT_EQUAL_STRING("999.99", ftostr52(999.99f));
}

// The widest right-justified fields have columns that only large values reach.
MARLIN_TEST(numtostr, widest_columns) {
  TEST_ASSERT_EQUAL_STRING("99999.90", ftostr72rj(99999.9f));
  TEST_ASSERT_EQUAL_STRING(" 1000.00", ftostr72rj(1000.0f));
  TEST_ASSERT_EQUAL_STRING("  100.00", ftostr72rj(100.0f));
  TEST_ASSERT_EQUAL_STRING("9999.90", ftostr62rj(99999.9f));
  TEST_ASSERT_EQUAL_STRING("1000.00", ftostr62rj(1000.0f));
  TEST_ASSERT_EQUAL_STRING("99999.9", ftostr61rj(99999.9f));
  TEST_ASSERT_EQUAL_STRING(" 1000.0", ftostr61rj(1000.0f));
  // LEGACY-BEHAVIOR: past the field width the high digits are dropped, so 123456.7
  // reads as 23456.70 rather than saturating.
  TEST_ASSERT_EQUAL_STRING("23456.70", ftostr72rj(123456.7f));
  TEST_ASSERT_EQUAL_STRING("3456.70", ftostr62rj(123456.7f));
}

// ftostr52sp blanks a trailing zero decimal, one decimal, or both.
MARLIN_TEST(numtostr, ftostr52sp_decimal_branches) {
  TEST_ASSERT_EQUAL_STRING(" 100   ", ftostr52sp(100.0f));   // no decimals at all
  TEST_ASSERT_EQUAL_STRING("   0.05", ftostr52sp(0.05f));    // both decimals
  TEST_ASSERT_EQUAL_STRING(" 999.9 ", ftostr52sp(99999.9f)); // first decimal only
}

MARLIN_TEST(numtostr, ftostr63_wide) {
  TEST_ASSERT_EQUAL_STRING("100.000", ftostr63(100.0f));
  TEST_ASSERT_EQUAL_STRING("000.050", ftostr63(0.05f));
  TEST_ASSERT_EQUAL_STRING("999.901", ftostr63(99999.9f));
}
