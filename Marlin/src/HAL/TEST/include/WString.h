/**
 * Arduino's String, which this HAL does not have and does not need.
 *
 * `lcd/dwin/creality/dwin.cpp` includes <WString.h>, and on an Arduino-based target that
 * brings in the dynamically-allocated `String` class. It never uses the type — checked with
 * a word-boundary grep across the driver and its siblings; every hit is `dwinDrawString`,
 * which is Marlin's own function taking a `char*`.
 *
 * So the include is vestigial, and this header exists to satisfy it rather than to provide
 * anything. Deliberately empty: adding a `String` implementation would let a future edit
 * start depending on heap allocation in firmware that has none, and the compiler would not
 * complain.
 */
#pragma once
