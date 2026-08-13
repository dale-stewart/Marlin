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
 * What the machine says when it comes up.
 *
 * Both of these were inside `setup()`, which no test can execute, and both are pure
 * reporting — a decision about what to print, given a value. Pulling them out is what the
 * `MarlinBoot.cpp` split is *for*: `setup()` reads the reset register and hands the mask over,
 * and the deciding is done somewhere a test can reach.
 *
 * They are worth asserting rather than merely reachable. The reset reason is the only evidence
 * an operator has of why a printer restarted mid-print, and the identity banner is what a host
 * matches its capabilities against before it starts sending G-code.
 */

#include "../test/unit_tests.h"
#include "../gcode/serial_capture.h"
#include "src/MarlinCore.h"
#include "src/module/planner.h"
#include <string>

namespace {

  std::string reported_for(const uint8_t mcu) {
    SerialCapture capture;
    marlin.report_reset_reason(mcu);
    return capture.finish();
  }

  bool contains(const std::string &haystack, const char * const needle) {
    return haystack.find(needle) != std::string::npos;
  }

}

/**
 * Each cause is reported, and only when it happened.
 *
 * Five independent flags read from one register, each with its own line. A test that set the
 * whole mask would pass with every comparison replaced by "true", and one that set a single
 * flag would pass with every comparison replaced by that flag — so each is raised on its own
 * and the other four are asserted absent.
 *
 * The distinction matters to whoever is reading the log: a watchdog reset means the firmware
 * stopped answering, a brown-out means the supply sagged, an external reset means somebody
 * pressed the button. Reporting the wrong one sends the diagnosis in the wrong direction.
 */
MARLIN_TEST(startup_report, each_reset_cause_is_reported_on_its_own) {
  struct Cause { uint8_t bit; const char *words; };
  const Cause causes[] = {
    { RST_POWER_ON,  STR_POWERUP },
    { RST_EXTERNAL,  STR_EXTERNAL_RESET },
    { RST_BROWN_OUT, STR_BROWNOUT_RESET },
    { RST_WATCHDOG,  STR_WATCHDOG_RESET },
    { RST_SOFTWARE,  STR_SOFTWARE_RESET }
  };
  constexpr size_t COUNT = sizeof(causes) / sizeof(causes[0]);

  for (size_t raised = 0; raised < COUNT; raised++) {
    const std::string report = reported_for(causes[raised].bit);
    for (size_t i = 0; i < COUNT; i++) {
      char msg[160];
      snprintf(msg, sizeof(msg), "with only %s set, the report %s say %s",
               causes[raised].words, i == raised ? "should" : "should not", causes[i].words);
      TEST_ASSERT_EQUAL_MESSAGE(i == raised, contains(report, causes[i].words), msg);
    }
  }
}

/**
 * Nothing set means nothing said.
 *
 * The other side of the bracket, and the case the comment in `setup()` names: "does nothing if
 * bootloader sets MCUSR to 0". A board whose bootloader clears the register must not have a
 * reset cause invented for it — a log that always claimed a power-on would make the one time
 * it mattered indistinguishable from every other boot.
 */
MARLIN_TEST(startup_report, a_cleared_register_reports_no_cause_at_all) {
  const std::string report = reported_for(0);

  TEST_ASSERT_TRUE_MESSAGE(report.empty(),
    "with no reset flags set the machine should say nothing about why it restarted");
}

/**
 * More than one cause can be true at once, and both are reported.
 *
 * These are flags, not an enumeration: a supply that sagged far enough to trip the watchdog
 * sets two bits, and that pair is a more specific diagnosis than either alone. Code written as
 * a chain of `else if` would report only the first and would pass every test above.
 */
MARLIN_TEST(startup_report, two_causes_at_once_are_both_reported) {
  const std::string report = reported_for(RST_BROWN_OUT | RST_WATCHDOG);

  TEST_ASSERT_TRUE_MESSAGE(contains(report, STR_BROWNOUT_RESET), "the brown-out should be reported");
  TEST_ASSERT_TRUE_MESSAGE(contains(report, STR_WATCHDOG_RESET), "and the watchdog as well");
}

/**
 * The identity banner names the firmware and the size of the planner buffer.
 *
 * A host reads the version to decide what it may send. The planner figure is the one number in
 * the banner that is derived rather than fixed — `sizeof(block_t) * BLOCK_BUFFER_SIZE` — so it
 * is asserted against the same product computed here from the configuration, which is a
 * different route to the same answer rather than a copy of the code's own.
 */
MARLIN_TEST(startup_report, the_banner_names_the_version_and_the_planner_buffer) {
  std::string banner;
  { SerialCapture capture; marlin.report_firmware_identity(); banner = capture.finish(); }

  TEST_ASSERT_TRUE_MESSAGE(contains(banner, "Marlin " SHORT_BUILD_VERSION),
    "the banner should name the firmware and its version");

  char expected[64];
  snprintf(expected, sizeof(expected), "%s%u", STR_PLANNER_BUFFER_BYTES,
           unsigned(sizeof(block_t) * (BLOCK_BUFFER_SIZE)));
  TEST_ASSERT_TRUE_MESSAGE(contains(banner, expected),
    "and should report the planner buffer the configuration actually asked for");
}
