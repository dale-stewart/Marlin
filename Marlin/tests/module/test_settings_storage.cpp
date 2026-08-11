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
 * Settings that outlive the power being switched off.
 *
 * `settings.cpp` flattens everything the user can change into one block of bytes, writes it to
 * storage with a version tag and a checksum, and reads it back. The correctness property is a
 * round trip: whatever went in has to come out, byte for byte, through code that packs dozens
 * of unrelated fields in a fixed order. A field written in the wrong place, read back in the
 * wrong order, or silently dropped is not visible from any single-value check — but it is
 * always visible from a round trip.
 *
 * That is why this needs its own configuration. `EEPROM_SETTINGS` is off in every other
 * configuration under `test/`, so `M500` and `M501` are not compiled and only `reset()` could be
 * reached; the file measured 91% covered and 16% mutation-killed, which is what code that runs
 * without being asserted looks like. `test/006-eeprom.ini` turns the storage on, and the test
 * HAL backs it with a real file, so the bytes genuinely go out and come back.
 */

#include "src/inc/MarlinConfig.h"

#if defined(__PLAT_TEST__) && ENABLED(EEPROM_SETTINGS)

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../gcode/serial_capture.h"
#include "src/module/settings.h"
#include "src/HAL/shared/eeprom_api.h"
#include "src/module/planner.h"
#include "src/module/motion.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"

#include <string.h>
#include <string>

namespace {

  std::string host_sends(const char * const line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    SerialCapture capture;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    return capture.finish();
  }

  /**
   * Whatever the stored settings end up as, put the machine back to its configured defaults
   * afterwards and store *those*.
   *
   * The store is a file that outlives the test, so a test that left a perturbed block behind
   * would hand it to whatever ran next — and to the next run of the suite. Resetting and saving
   * is the only way to leave it in a state that does not depend on this test having run.
   */
  struct StoredSettingsRestored {
    ~StoredSettingsRestored() {
      host_sends("M502");
      host_sends("M500");
    }
  };

  // A spread of settings, deliberately unrelated to each other and living in different parts of
  // the block: a per-axis table, a scalar, another per-axis table, and an offset that belongs to
  // `motion` rather than `planner`.
  struct Chosen {
    static constexpr float STEPS_X = 123.25f, FEEDRATE_Y = 137.5f, ACCEL = 1234.0f, OFFSET_X = -3.75f;

    static void apply() {
      host_sends("M92 X123.25");
      host_sends("M203 Y137.5");
      host_sends("M204 P1234");
      TERN_(HAS_HOME_OFFSET, host_sends("M206 X-3.75"));
    }

    static void scramble() {
      host_sends("M92 X55");
      host_sends("M203 Y44");
      host_sends("M204 P3300");
      TERN_(HAS_HOME_OFFSET, host_sends("M206 X9"));
    }

    static void assert_present(const char * const when) {
      char why[96];
      snprintf(why, sizeof(why), "X steps/mm %s", when);
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(STEPS_X, planner.settings.axis_steps_per_mm[X_AXIS], why);
      snprintf(why, sizeof(why), "Y max feedrate %s", when);
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(FEEDRATE_Y, planner.settings.max_feedrate_mm_s[Y_AXIS], why);
      snprintf(why, sizeof(why), "printing acceleration %s", when);
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(ACCEL, planner.settings.acceleration, why);
      #if HAS_HOME_OFFSET
        snprintf(why, sizeof(why), "X home offset %s", when);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(OFFSET_X, motion.home_offset.x, why);
      #endif
    }
  };

}

/**
 * What was saved is what comes back.
 *
 * The whole point of the file. Several unrelated settings are set, stored, then deliberately
 * scrambled in memory so that "it came back" cannot be confused with "it was never lost", and
 * then read back.
 *
 * The scramble matters more than it looks: without it a load that did nothing at all would pass,
 * and doing nothing is exactly what a failed checksum or a version mismatch makes `load()` do.
 */
MARLIN_TEST(settings_storage, what_was_saved_is_what_comes_back) {
  StoredSettingsRestored restore;

  Chosen::apply();
  Chosen::assert_present("should be set before saving");

  host_sends("M500");

  Chosen::scramble();
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(55.0f, planner.settings.axis_steps_per_mm[X_AXIS],
    "the scramble should have taken effect, or the load below proves nothing");

  host_sends("M501");

  Chosen::assert_present("should have come back from storage");
}

/**
 * Resetting the machine does not reach into storage.
 *
 * `M502` restores the configured defaults *in memory*; the stored block is untouched until an
 * `M500` follows. That distinction is the whole reason the two commands are separate — it lets
 * a user try the defaults and then take their settings back — and a reset that also wiped the
 * store would make that irreversible.
 */
MARLIN_TEST(settings_storage, a_reset_leaves_the_stored_settings_alone) {
  StoredSettingsRestored restore;

  Chosen::apply();
  host_sends("M500");

  host_sends("M502");
  TEST_ASSERT_NOT_EQUAL_MESSAGE(Chosen::STEPS_X, planner.settings.axis_steps_per_mm[X_AXIS],
    "a reset should have put the configured default back in memory");

  host_sends("M501");
  Chosen::assert_present("should still have been in storage after a reset");
}

/**
 * Saving twice stores the second set, not the first.
 *
 * A store that wrote only when empty, or that appended rather than replaced, would pass the
 * round trip above — the first save would be the one read back and it would look right. Two
 * different values through the same field is what separates "it can store" from "it stores what
 * you last told it".
 */
MARLIN_TEST(settings_storage, saving_again_replaces_what_was_stored) {
  StoredSettingsRestored restore;

  Chosen::apply();
  host_sends("M500");

  host_sends("M92 X321.5");
  host_sends("M500");

  Chosen::scramble();
  host_sends("M501");

  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(321.5f, planner.settings.axis_steps_per_mm[X_AXIS],
    "the second save should be the one that comes back");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(Chosen::FEEDRATE_Y, planner.settings.max_feedrate_mm_s[Y_AXIS],
    "and the fields the second save did not change should still be there");
}

/**
 * The stored block is validated, not trusted.
 *
 * Storage that has never been written, or that belongs to an older firmware, holds bytes that
 * mean something different — loading them as settings would give the machine arbitrary steps per
 * millimetre and accelerations. So the block carries a version and a checksum, and a load that
 * does not like what it finds falls back to the configured defaults rather than using it.
 *
 * Provoked by corrupting the stored bytes underneath the firmware, which is the one thing a test
 * can do that a user cannot.
 */
MARLIN_TEST(settings_storage, a_corrupt_store_falls_back_to_the_defaults) {
  StoredSettingsRestored restore;

  Chosen::apply();
  host_sends("M500");

  // Overwrite a byte in the middle of the block. Not the header: this has to be a change the
  // checksum catches rather than one the version check does. 100 is `EEPROM_OFFSET`, which is a
  // private #define in settings.cpp — repeated here because there is no way to ask for it.
  persistentStore.access_start();
  persistentStore.write_data(100 + 64, uint8_t(0x5A));
  persistentStore.access_finish();

  host_sends("M501");

  constexpr float configured_steps[] = DEFAULT_AXIS_STEPS_PER_UNIT;
  TEST_ASSERT_NOT_EQUAL_MESSAGE(Chosen::STEPS_X, planner.settings.axis_steps_per_mm[X_AXIS],
    "a store that fails its checksum should not be loaded as though it were good");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(configured_steps[X_AXIS], planner.settings.axis_steps_per_mm[X_AXIS],
    "it should fall back to the configured default, not to whatever the bytes happened to say");
}

#endif // __PLAT_TEST__ && EEPROM_SETTINGS
