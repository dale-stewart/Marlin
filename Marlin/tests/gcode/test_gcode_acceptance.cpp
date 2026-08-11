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
 * Acceptance tests for the scenarios in features/.
 *
 * Everything here goes in the way a host does: bytes arrive on the serial port, the
 * queue reads them, and the outcome is whatever the printer then does or says. Nothing
 * below calls the parser, the dispatcher or a handler directly — those are exercised
 * because a real command passed through them, which is where their coverage should come
 * from.
 *
 * The steps are the only place where the language of the feature files becomes an API
 * call. A scenario that cannot be written from the steps below wants a new step, not a
 * reach into the internals.
 */

#include "../test/unit_tests.h"
#include "serial_capture.h"
#include "simulated_sensors.h"
#include "src/gcode/queue.h"
#include "src/gcode/gcode.h"
#include "src/core/serial.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/temperature.h"
#include <string.h>
#include <stdio.h>

namespace {

  //
  // ---- Given ----
  //

  // A printer with an empty queue, no leftover numbering, and nobody listening unless
  // a step asks to listen. Restores what it changed so scenarios stay independent.
  struct ConnectedPrinter {
    int16_t was_feedrate;
    uint8_t was_debug;
    bool was_connected;

    ConnectedPrinter() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      was_feedrate = motion.feedrate_percentage;
      was_debug = marlin_debug_flags;
      queue.clear();
      queue.set_current_line_number(0);
    }
    ~ConnectedPrinter() {
      queue.clear();
      motion.feedrate_percentage = was_feedrate;
      marlin_debug_flags = was_debug;
      MYSERIAL1.host_connected = was_connected;
    }
  };

  /**
   * Leaves the machine — and its storage — back at the configured defaults.
   *
   * Settings outlive the scenario that changed them, and under `006-eeprom` so does the stored
   * block, so without this a scenario would hand its calibration to whatever ran next.
   *
   * It puts things back the way a *user* would, with `M502` and `M500`, rather than by saving
   * and restoring the C++ structure. That is the same rule as the steps: this feature is the net
   * for a change to `planner.settings`, and infrastructure that named it would have to be edited
   * by the migration alongside the code, which is exactly what stops a test being evidence.
   */
  struct RestoredSettings {
    ~RestoredSettings() {
      for (const char *p = "M502"; *p; p++) MYSERIAL1.receive_buffer.write(uint8_t(*p));
      MYSERIAL1.receive_buffer.write(uint8_t('\n'));
      queue.get_available_commands();
      for (uint8_t g = 0; g < 64 && queue.has_commands_queued(); g++) queue.advance();
      #if ENABLED(EEPROM_SETTINGS)
        for (const char *p = "M500"; *p; p++) MYSERIAL1.receive_buffer.write(uint8_t(*p));
        MYSERIAL1.receive_buffer.write(uint8_t('\n'));
        queue.get_available_commands();
        for (uint8_t g = 0; g < 64 && queue.has_commands_queued(); g++) queue.advance();
      #endif
    }
  };

  // Returns the reading the sensor actually achieved, which is the nearest one it can
  // express — see as_reported() below.
  celsius_float_t the_hotend_is_already_at(const celsius_float_t c) {
    return SimulatedSensors::hotend_reads(c);
  }

  /**
   * How the hotend reading should appear in a report.
   *
   * The sensor is driven at the ADC now, so it reads the nearest count to what the
   * scenario asked for rather than the exact figure — near 123 C that is about 0.15 C
   * away. The scenario still says "the reply mentions the temperature the hotend is at";
   * this is that temperature, formatted the way M105 formats it.
   */
  std::string as_reported(const celsius_float_t c) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", double(c));
    return std::string(buf);
  }

  void the_printer_is_in_dry_run_mode() { marlin_debug_flags |= 8; }   // 8 = DRYRUN

  //
  // ---- When ----
  //

  void run_until_idle() {
    for (uint8_t guard = 0; guard < 64 && queue.has_commands_queued(); guard++)
      queue.advance();
  }

  // Put a line on the wire and let the printer act on it.
  void the_host_sends(const char * const line) {
    for (const char *p = line; *p; p++) MYSERIAL1.receive_buffer.write(uint8_t(*p));
    MYSERIAL1.receive_buffer.write(uint8_t('\n'));
    queue.get_available_commands();
    run_until_idle();
  }

  uint8_t checksum_of(const char * const line) {
    uint8_t sum = 0;
    for (const char *p = line; *p; p++) sum ^= uint8_t(*p);
    return sum;
  }

  // A well-behaved host numbers each line and appends a checksum over it.
  void the_host_sends_line(const long number, const char * const command) {
    char body[128], full[160];
    snprintf(body, sizeof(body), "N%ld %s", number, command);
    snprintf(full, sizeof(full), "%s*%u", body, unsigned(checksum_of(body)));
    the_host_sends(full);
  }

  void the_host_sends_with_a_damaged_checksum(const char * const command) {
    char body[128], full[160];
    snprintf(body, sizeof(body), "N1 %s", command);
    snprintf(full, sizeof(full), "%s*%u", body, unsigned(checksum_of(body) ^ 0xFF));
    the_host_sends(full);
  }

  void the_host_sends_with_a_correct_checksum(const char * const command) {
    the_host_sends_line(1, command);
  }

  void the_host_renumbers_to(const long number) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "M110 N%ld", number);
    the_host_sends_line(number, cmd);
  }

  // Ask a question and keep the reply.
  std::string the_reply_to(const char * const line) {
    SerialCapture capture;
    for (const char *p = line; *p; p++) MYSERIAL1.receive_buffer.write(uint8_t(*p));
    MYSERIAL1.receive_buffer.write(uint8_t('\n'));
    queue.get_available_commands();
    run_until_idle();
    return capture.finish();
  }

  //
  // ---- Then ----
  //

  void the_printer_is_running_at_speed(const int16_t percent) {
    TEST_ASSERT_EQUAL(percent, motion.feedrate_percentage);
  }

  void the_printer_is_not_running_at_speed(const int16_t percent) {
    TEST_ASSERT_NOT_EQUAL(percent, motion.feedrate_percentage);
  }

  void the_printer_is_expecting_line(const long number) {
    TEST_ASSERT_EQUAL(number - 1, queue.get_current_line_number());
  }

  void the_hotend_is_aiming_for(const celsius_t degrees) {
    TEST_ASSERT_EQUAL(degrees, thermalManager.degTargetHotend(0));
  }

  void the_printer_carries_out_nothing() {
    TEST_ASSERT_FALSE(queue.has_commands_queued());
  }

  void the_reply_mentions(const std::string &reply, const char * const text) {
    TEST_ASSERT_TRUE(reply.find(text) != std::string::npos);
  }

  void the_reply_mentions(const std::string &reply, const std::string &text) {
    TEST_ASSERT_TRUE(reply.find(text) != std::string::npos);
  }

  //
  // ---- Steps for: Keeping the settings the machine was tuned with ----
  //
  // Every one of these goes over the wire as G-code and reads the answer out of a report.
  // That is deliberate and load-bearing: these scenarios exist to hold still while the C++
  // surface behind `planner.settings` is rewritten, and a step that named that surface would
  // have to be edited by the very change it is supposed to be checking.
  //

  void the_host_sets_the_x_steps_per_mm_to(const char * const value) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "M92 X%s", value);
    the_host_sends(cmd);
  }

  void the_host_sets_the_max_y_feedrate_to(const char * const value) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "M203 Y%s", value);
    the_host_sends(cmd);
  }

  void the_host_restores_the_factory_settings() { the_host_sends("M502"); }

  #if ENABLED(EEPROM_SETTINGS)
    void the_host_saves_the_settings() { the_host_sends("M500"); }

    // A restart is a power cycle: the machine comes up and reads whatever is in storage. `M501`
    // is what the firmware itself does at boot, so this is the same path a real restart takes
    // without needing the process to end.
    void the_printer_is_restarted() { the_host_sends("M501"); }
  #endif

  std::string the_settings_report() { return the_reply_to("M503"); }

  void the_settings_report_gives(const char * const field, const char * const value) {
    char expected[48];
    snprintf(expected, sizeof(expected), "%s%s", field, value);
    const std::string report = the_settings_report();
    TEST_ASSERT_TRUE_MESSAGE(report.find(expected) != std::string::npos, expected);
  }

  void the_settings_report_no_longer_gives(const char * const field, const char * const value) {
    char unwanted[48];
    snprintf(unwanted, sizeof(unwanted), "%s%s", field, value);
    const std::string report = the_settings_report();
    TEST_ASSERT_TRUE_MESSAGE(report.find(unwanted) == std::string::npos, unwanted);
  }

}

//
// ======== Feature: Receiving a job from a host ========
//

MARLIN_TEST(receiving_a_job, a_plain_command_is_carried_out) {
  ConnectedPrinter printer;
  the_host_sends("M220 S45");
  the_printer_is_running_at_speed(45);
}

MARLIN_TEST(receiving_a_job, lines_are_numbered_so_nothing_is_missed) {
  ConnectedPrinter printer;
  the_host_sends_line(1, "M220 S11");
  the_host_sends_line(2, "M220 S22");
  the_printer_is_running_at_speed(22);
  the_printer_is_expecting_line(3);
}

MARLIN_TEST(receiving_a_job, a_line_out_of_order_is_refused) {
  ConnectedPrinter printer;
  the_host_sends_line(1, "M220 S11");
  the_host_sends_line(9, "M220 S99");
  the_printer_is_running_at_speed(11);
}

MARLIN_TEST(receiving_a_job, a_line_already_run_is_not_run_twice) {
  ConnectedPrinter printer;
  the_host_sends_line(1, "M220 S11");

  motion.feedrate_percentage = 50;              // something else happened meanwhile
  the_host_sends_line(1, "M220 S11");           // the host repeats itself
  the_printer_is_running_at_speed(50);          // the repeat was ignored
}

MARLIN_TEST(receiving_a_job, a_damaged_line_is_refused) {
  ConnectedPrinter printer;
  the_host_sends_with_a_damaged_checksum("M220 S88");
  the_printer_is_not_running_at_speed(88);
}

MARLIN_TEST(receiving_a_job, an_intact_line_is_carried_out) {
  ConnectedPrinter printer;
  the_host_sends_with_a_correct_checksum("M220 S66");
  the_printer_is_running_at_speed(66);
}

MARLIN_TEST(receiving_a_job, the_host_can_restart_the_numbering) {
  ConnectedPrinter printer;
  the_host_sends_line(1, "M220 S11");
  the_host_renumbers_to(500);
  the_printer_is_expecting_line(501);

  the_host_sends_line(501, "M220 S33");
  the_printer_is_running_at_speed(33);
}

MARLIN_TEST(receiving_a_job, lines_that_carry_no_command_are_ignored) {
  ConnectedPrinter printer;
  for (const char *line : { "", "; comment" }) {
    the_host_sends(line);
    the_printer_carries_out_nothing();
  }
}

//
// ======== Feature: Preparing the printer for a job ========
//

MARLIN_TEST(preparing_to_print, setting_the_hotend_temperature) {
  ConnectedPrinter printer;
  SimulatedSensors sensors;
  the_host_sends("M104 S200");
  the_hotend_is_aiming_for(200);
  the_host_sends("M104 S0");
}

MARLIN_TEST(preparing_to_print, waiting_until_the_hotend_is_hot_enough) {
  ConnectedPrinter printer;
  SimulatedSensors sensors;
  the_hotend_is_already_at(205.0f);
  the_host_sends("M109 S200");                  // returns rather than hanging
  the_hotend_is_aiming_for(200);
  the_host_sends("M104 S0");
}

MARLIN_TEST(preparing_to_print, cooling_down_does_not_hold_up_the_job) {
  ConnectedPrinter printer;
  SimulatedSensors sensors;
  the_hotend_is_already_at(220.0f);
  the_host_sends("M109 S180");
  the_hotend_is_aiming_for(180);
  the_host_sends("M104 S0");
}

MARLIN_TEST(preparing_to_print, setting_the_print_speed) {
  ConnectedPrinter printer;
  the_host_sends("M220 S150");
  the_printer_is_running_at_speed(150);
  the_reply_mentions(the_reply_to("M220"), "FR:150%");
}

#if HAS_EXTRUDERS
  MARLIN_TEST(preparing_to_print, setting_how_much_filament_to_push) {
    ConnectedPrinter printer;
    const int16_t was = planner.flow_percentage[0];
    the_host_sends("M221 S80");
    the_reply_mentions(the_reply_to("M221"), "Flow: 80%");
    planner.set_flow(0, was);
  }
#endif

MARLIN_TEST(preparing_to_print, a_dry_run_heats_nothing) {
  ConnectedPrinter printer;
  SimulatedSensors sensors;
  the_host_sends("M104 S0");
  the_printer_is_in_dry_run_mode();
  the_host_sends("M104 S250");
  the_hotend_is_aiming_for(0);
}

#if HAS_FAN
  MARLIN_TEST(preparing_to_print, turning_the_fan_on_and_off) {
    ConnectedPrinter printer;
    const uint8_t was = thermalManager.fan_speed[0];

    the_host_sends("M106 S128");
    TEST_ASSERT_EQUAL(128, thermalManager.fan_speed[0]);

    the_host_sends("M107");
    TEST_ASSERT_EQUAL(0, thermalManager.fan_speed[0]);

    thermalManager.fan_speed[0] = was;
  }
#endif

//
// ======== Feature: Telling the host what the printer is doing ========
//

MARLIN_TEST(reporting_status, reporting_temperatures) {
  ConnectedPrinter printer;
  SimulatedSensors sensors;
  const celsius_float_t at = the_hotend_is_already_at(123.0f);

  const std::string reply = the_reply_to("M105");
  the_reply_mentions(reply, "T:" + as_reported(at));
  the_reply_mentions(reply, "B:");
}

MARLIN_TEST(reporting_status, reporting_position) {
  ConnectedPrinter printer;
  const std::string reply = the_reply_to("M114");
  the_reply_mentions(reply, "X:");
  the_reply_mentions(reply, "Y:");
  the_reply_mentions(reply, "Z:");
}

MARLIN_TEST(reporting_status, reporting_what_the_firmware_can_do) {
  ConnectedPrinter printer;
  the_reply_mentions(the_reply_to("M115"), "Marlin");
}

//
// ======== Feature: Keeping the settings the machine was tuned with ========
//
// The safety net for the `planner.settings` migration. Nothing here names a C++ symbol that
// the migration touches, so the whole feature can stay green and unedited while the surface
// underneath it is replaced — which is the only way a test can be evidence that a refactor
// preserved behaviour.
//

// `M92 X` is the calibration that decides whether a 20 mm cube measures 20 mm.
MARLIN_TEST(keeping_its_settings, changing_a_setting_and_seeing_it_take) {
  ConnectedPrinter printer;
  RestoredSettings settings;

  the_host_sets_the_x_steps_per_mm_to("123.25");

  the_settings_report_gives("M92 X", "123.25");
}

// A machine that lost an unrelated setting every time one was changed would be untunable.
MARLIN_TEST(keeping_its_settings, changing_one_setting_leaves_the_others_alone) {
  ConnectedPrinter printer;
  RestoredSettings settings;

  the_host_sets_the_x_steps_per_mm_to("123.25");
  the_host_sets_the_max_y_feedrate_to("137.5");

  the_settings_report_gives("M92 X", "123.25");
  the_settings_report_gives("M203 X300.00 Y", "137.50");
}

// The way back to a known state when a machine is behaving oddly.
MARLIN_TEST(keeping_its_settings, going_back_to_how_the_firmware_was_built) {
  ConnectedPrinter printer;
  RestoredSettings settings;

  the_host_sets_the_x_steps_per_mm_to("123.25");
  the_host_restores_the_factory_settings();

  the_settings_report_no_longer_gives("M92 X", "123.25");
}

#if ENABLED(EEPROM_SETTINGS)

// The whole point of saving: the calibration is still there next time the printer is on.
MARLIN_TEST(keeping_its_settings, settings_kept_across_a_power_cycle) {
  ConnectedPrinter printer;
  RestoredSettings settings;

  the_host_sets_the_x_steps_per_mm_to("123.25");
  the_host_saves_the_settings();
  the_printer_is_restarted();

  the_settings_report_gives("M92 X", "123.25");
}

// ...and the other arm: a change that was never saved does not survive, or nobody could try
// a setting out without committing to it.
MARLIN_TEST(keeping_its_settings, a_machine_that_was_never_saved_comes_up_as_it_was_built) {
  ConnectedPrinter printer;
  RestoredSettings settings;

  the_host_restores_the_factory_settings();
  the_host_saves_the_settings();               // a known starting point in storage

  the_host_sets_the_x_steps_per_mm_to("123.25");
  the_printer_is_restarted();

  the_settings_report_no_longer_gives("M92 X", "123.25");
}

// `M502` and `M500` are separate commands so that trying the defaults is reversible.
MARLIN_TEST(keeping_its_settings, restoring_the_factory_settings_does_not_discard_what_was_saved) {
  ConnectedPrinter printer;
  RestoredSettings settings;

  the_host_sets_the_x_steps_per_mm_to("123.25");
  the_host_saves_the_settings();

  the_host_restores_the_factory_settings();
  the_printer_is_restarted();

  the_settings_report_gives("M92 X", "123.25");
}

#endif // EEPROM_SETTINGS
