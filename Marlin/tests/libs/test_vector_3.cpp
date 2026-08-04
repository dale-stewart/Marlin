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
 * The geometry behind bed levelling.
 *
 * This file was unreachable until a configuration compiled it: `vector_3.cpp` has exactly
 * one caller in the firmware, the planar bed-levelling fit, and no configuration under test
 * enabled it. Phase 1 deferred it for that reason. It is the clearest example in the
 * codebase of code that is untestable not because it is entangled but because nothing in
 * the build asks for it.
 *
 * Every assertion here is a property the mathematics guarantees, not a number recorded from
 * a previous run: a cross product is perpendicular to both its operands, a normal has unit
 * length, a rotation followed by its inverse is the identity. Those hold for any input, so
 * they can be asserted against inputs chosen to be awkward rather than against inputs whose
 * answers are already known — and an implementation that is wrong in a way that happens to
 * suit one test case cannot satisfy them.
 *
 * Why it matters to the machine: the levelling transform is applied to every move once a
 * plane has been measured, and its transpose is applied to undo it. If the two are not
 * exact inverses, positions drift by a little on each conversion, in a way no single move
 * would reveal.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_LEVELING && ABL_PLANAR

#include "../test/unit_tests.h"
#include "src/libs/vector_3.h"

#include <math.h>

namespace {

  // Single-precision maths accumulates; a tolerance is part of the assertion, not an
  // excuse. 1e-5 is far tighter than any error that would matter to a printer and far
  // looser than the last bit of a float.
  constexpr float TOL = 1e-5f;

  float dot(const vector_3 &a, const vector_3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

  void assert_same_vector(const vector_3 &want, const vector_3 &got, const char * const what) {
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, want.x, got.x, what);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, want.y, got.y, what);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, want.z, got.z, what);
  }

  // A direction that is awkward on purpose: no zero components, no symmetry, not a unit
  // vector. Anything that passes with this is not passing by accident of the input.
  vector_3 awkward() { return vector_3(0.37f, -1.9f, 2.55f); }

}

/**
 * A cross product is perpendicular to both of the vectors it came from.
 *
 * This is the defining property, and it is what the levelling fit relies on: the bed's
 * normal is built by crossing two directions lying in the bed, and a normal that is not
 * perpendicular to the surface tilts every move by the error.
 */
MARLIN_TEST(vector_3, a_cross_product_is_perpendicular_to_both_operands) {
  const vector_3 a(1.0f, 2.0f, 3.0f), b(-4.0f, 0.5f, 2.0f);
  const vector_3 c = vector_3::cross(a, b);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, 0.0f, dot(c, a), "the cross product should be perpendicular to the first");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, 0.0f, dot(c, b), "the cross product should be perpendicular to the second");
}

/**
 * Swapping the operands reverses the result.
 *
 * Antisymmetry is what fixes the *sign* of the normal, and the sign is the difference
 * between a bed tilted one way and the same bed tilted the other. A magnitude-only test
 * cannot see it.
 */
MARLIN_TEST(vector_3, reversing_a_cross_product_reverses_its_direction) {
  const vector_3 a(1.0f, 2.0f, 3.0f), b(-4.0f, 0.5f, 2.0f);
  const vector_3 ab = vector_3::cross(a, b), ba = vector_3::cross(b, a);

  assert_same_vector(vector_3(-ba.x, -ba.y, -ba.z), ab, "a x b should be -(b x a)");
}

// A vector crossed with itself has no direction to point in.
MARLIN_TEST(vector_3, a_vector_crossed_with_itself_is_zero) {
  const vector_3 a = awkward();
  assert_same_vector(vector_3(0, 0, 0), vector_3::cross(a, a), "a x a should be zero");
}

/**
 * The right-handed convention, stated once.
 *
 * The properties above hold for a left-handed cross product too — it would be perpendicular
 * and antisymmetric and still wrong. One concrete case pins which of the two this is, and
 * the axes are the case worth pinning because that is the frame the machine uses.
 */
MARLIN_TEST(vector_3, the_axes_cross_in_the_right_handed_order) {
  const vector_3 x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);

  assert_same_vector(z, vector_3::cross(x, y), "x cross y should be z");
  assert_same_vector(x, vector_3::cross(y, z), "y cross z should be x");
  assert_same_vector(y, vector_3::cross(z, x), "z cross x should be y");
}

/**
 * A normal has unit length and the same direction as what it came from.
 *
 * Both halves are needed. Length alone is satisfied by any unit vector, direction alone by
 * the original; together they say the operation is a pure scaling. The direction is checked
 * by requiring the result to be a positive multiple of the input rather than by comparing
 * against a precomputed answer.
 */
MARLIN_TEST(vector_3, a_normal_is_unit_length_and_points_the_same_way) {
  const vector_3 a = awkward();
  const vector_3 n = a.get_normal();

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, 1.0f, n.magnitude(), "a normal should be unit length");

  const float scale = a.magnitude();
  assert_same_vector(a, vector_3(n.x * scale, n.y * scale, n.z * scale),
    "scaling the normal back up should recover the original");
}

// normalize() is get_normal() applied in place — the same operation, not a second one.
MARLIN_TEST(vector_3, normalizing_in_place_matches_taking_a_normal) {
  vector_3 a = awkward();
  const vector_3 taken = a.get_normal();
  a.normalize();

  assert_same_vector(taken, a, "normalize() and get_normal() should agree");
}

// Magnitude is the Euclidean length, checked on a triple whose answer is exact in binary.
MARLIN_TEST(vector_3, magnitude_is_the_euclidean_length) {
  TEST_ASSERT_FLOAT_WITHIN(TOL, 5.0f, vector_3(3.0f, 4.0f, 0.0f).magnitude());
  TEST_ASSERT_FLOAT_WITHIN(TOL, 13.0f, vector_3(3.0f, 4.0f, 12.0f).magnitude());
  TEST_ASSERT_FLOAT_WITHIN(TOL, 0.0f, vector_3(0, 0, 0).magnitude());
}

// The identity leaves a vector where it was, which is the base case every rotation is
// measured against.
MARLIN_TEST(vector_3, the_identity_matrix_moves_nothing) {
  matrix_3x3 identity;
  identity.set_to_identity();

  vector_3 a = awkward();
  const vector_3 before = a;
  a.apply_rotation(identity);

  assert_same_vector(before, a, "the identity should not move a vector");
}

/**
 * Transposing twice gets back the original matrix.
 *
 * An involution is a strong statement about a transform for how cheap it is to check: it
 * says every element ends up where it started, so no pair of indices can be swapped the
 * wrong way round. A matrix built from three distinct rows is what makes it meaningful —
 * with a symmetric matrix the assertion is free.
 */
MARLIN_TEST(vector_3, transposing_twice_is_the_original_matrix) {
  const matrix_3x3 m = matrix_3x3::create_from_rows(
    vector_3(1, 2, 3), vector_3(4, 5, 6), vector_3(7, 8, 9));

  const matrix_3x3 twice = matrix_3x3::transpose(matrix_3x3::transpose(m));

  for (uint8_t i = 0; i < 3; ++i)
    assert_same_vector(m.vectors[i], twice.vectors[i], "transposing twice should change nothing");
}

// Transposing once really does swap the off-diagonal elements.
MARLIN_TEST(vector_3, transposing_exchanges_rows_and_columns) {
  const matrix_3x3 m = matrix_3x3::create_from_rows(
    vector_3(1, 2, 3), vector_3(4, 5, 6), vector_3(7, 8, 9));
  const matrix_3x3 t = matrix_3x3::transpose(m);

  for (uint8_t i = 0; i < 3; ++i)
    for (uint8_t j = 0; j < 3; ++j)
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, m.vectors[j][i], t.vectors[i][j],
        "element (i,j) of the transpose should be element (j,i) of the original");
}

/**
 * A bed that is already flat needs no correction.
 *
 * `create_look_at` builds the rotation that takes a measured surface normal onto the Z
 * axis. Given the Z axis it must therefore produce the identity — the one case where the
 * right answer is known without computing anything, and the case a correctly levelled
 * machine is in.
 */
MARLIN_TEST(vector_3, looking_at_the_z_axis_is_the_identity) {
  const matrix_3x3 rot = matrix_3x3::create_look_at(vector_3(0, 0, 1));

  matrix_3x3 identity;
  identity.set_to_identity();

  for (uint8_t i = 0; i < 3; ++i)
    assert_same_vector(identity.vectors[i], rot.vectors[i],
      "a surface already normal to Z should need no rotation");
}

/**
 * Which direction the transform runs, stated so it cannot be assumed.
 *
 * `create_look_at` names a *target* and `apply_rotation` names a rotation, and between them
 * it is easy to believe the matrix stands the measured normal upright. It does the opposite:
 * `apply_rotation` multiplies by the transpose of the row-major matrix — note that it reads
 * `vectors[j][k]` with the *row* index varying with the input component — and `create_look_at`
 * says as much in a one-line comment ("already correctly transposed") that is easy to miss.
 *
 * So the matrix takes the Z axis *onto* the bed's normal, which is the direction the planner
 * wants: `bed_level_matrix.apply_rotation_xyz()` converts a requested position into machine
 * coordinates, and `transpose(bed_level_matrix)` converts back. Getting this backwards would
 * tilt a print by twice the bed's error rather than correcting it, and nothing in the naming
 * would say so.
 *
 * Both directions are asserted, because either alone is satisfied by a matrix that does
 * nothing to the axis it was given.
 */
MARLIN_TEST(vector_3, the_transform_turns_the_z_axis_onto_the_measured_normal) {
  const vector_3 tilted = awkward();
  const matrix_3x3 rot = matrix_3x3::create_look_at(tilted);

  // Forward: straight up becomes the bed's normal.
  vector_3 up(0, 0, 1);
  up.apply_rotation(rot);
  assert_same_vector(tilted.get_normal(), up, "the transform should take Z onto the bed normal");

  // Backward: the bed's normal becomes straight up, and keeps its length, because a
  // rotation does not scale. Asserting the length too rules out a projection, which would
  // also flatten the normal onto Z while quietly discarding the other two components.
  vector_3 turned = tilted;
  turned.apply_rotation(matrix_3x3::transpose(rot));

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, 0.0f, turned.x, "the unrotated normal should have no X");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, 0.0f, turned.y, "the unrotated normal should have no Y");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, tilted.magnitude(), turned.z,
    "a rotation should not change the length");
}

/**
 * The rotation is orthonormal, so its transpose undoes it exactly.
 *
 * This is the property the machine depends on and the one worth having most. Levelling
 * applies the matrix to convert a requested position into machine coordinates and its
 * transpose to convert back; if the two are not exact inverses, every conversion loses a
 * little and the error accumulates over a print rather than showing up in one move.
 *
 * Checked on a vector that has nothing to do with the plane, so the round trip is a
 * statement about the transform rather than about the direction it was built from.
 */
MARLIN_TEST(vector_3, rotating_and_unrotating_returns_the_original_position) {
  const matrix_3x3 rot = matrix_3x3::create_look_at(awkward());
  const matrix_3x3 back = matrix_3x3::transpose(rot);

  const vector_3 start(12.5f, -30.25f, 4.75f);
  vector_3 there = start;

  there.apply_rotation(rot);
  TEST_ASSERT_FALSE_MESSAGE(
    fabsf(there.x - start.x) < TOL && fabsf(there.y - start.y) < TOL && fabsf(there.z - start.z) < TOL,
    "the fixture chose a rotation that does nothing, so the round trip proves nothing");

  there.apply_rotation(back);
  assert_same_vector(start, there, "a rotation and its transpose should cancel exactly");
}

/**
 * The three rows of the rotation are a unit basis at right angles to each other.
 *
 * Orthonormality is what makes the transpose an inverse, so this is the same fact as the
 * round trip above approached from the other side — and it localises a failure, because it
 * says *which* row is wrong rather than only that the round trip drifted.
 */
MARLIN_TEST(vector_3, the_rotation_rows_are_an_orthonormal_basis) {
  const matrix_3x3 rot = matrix_3x3::create_look_at(awkward());

  for (uint8_t i = 0; i < 3; ++i)
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, 1.0f, rot.vectors[i].magnitude(), "each row should be a unit vector");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, 0.0f, dot(rot.vectors[0], rot.vectors[1]), "rows 0 and 1 should be perpendicular");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, 0.0f, dot(rot.vectors[1], rot.vectors[2]), "rows 1 and 2 should be perpendicular");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL, 0.0f, dot(rot.vectors[0], rot.vectors[2]), "rows 0 and 2 should be perpendicular");
}

// The three-argument form is the same transform as the vector form, applied to loose
// coordinates rather than to a vector.
MARLIN_TEST(vector_3, rotating_loose_coordinates_matches_rotating_a_vector) {
  const matrix_3x3 rot = matrix_3x3::create_look_at(awkward());

  vector_3 as_vector(12.5f, -30.25f, 4.75f);
  float x = as_vector.x, y = as_vector.y, z = as_vector.z;

  as_vector.apply_rotation(rot);
  const_cast<matrix_3x3&>(rot).apply_rotation_xyz(x, y, z);

  assert_same_vector(as_vector, vector_3(x, y, z), "both forms should be the same rotation");
}

#endif // HAS_LEVELING && ABL_PLANAR
