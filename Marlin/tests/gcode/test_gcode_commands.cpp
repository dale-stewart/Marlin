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
 * Tests for G-code commands that change a setting.
 *
 * These go in through the front door: a command line is parsed and dispatched exactly
 * as one arriving from a host would be, and the assertion is on the setting it was
 * supposed to change. That exercises the dispatcher and the handler together, and
 * exercises the parser indirectly — which is where a utility's coverage should come
 * from once it has real callers.
 */

#include "../test/unit_tests.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/core/serial.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/temperature.h"
#include "src/gcode/queue.h"
#include "simulated_sensors.h"
#include "serial_capture.h"
#include "src/module/stepper.h"
#include "src/module/settings.h"
#include "src/module/endstops.h"
#include "src/MarlinCore.h"
#include <string.h>

namespace {

  /**
   * Report-producing commands would otherwise hang the test binary.
   *
   * The native HAL's serial write busy-waits for room in a 128-byte transmit buffer
   * (HAL/LINUX/include/serial.h). In the simulator that buffer is drained by the UI; in
   * the unit test binary nothing drains it, so the first report that overflows it spins
   * forever. Marking the port as having no host attached — a state the firmware already
   * understands — makes write() return immediately instead.
   *
   * This does not change what the handler does, only where its output goes. A test that
   * needs to assert on the output would have to drain the buffer as it fills.
   */
  struct NoHostAttached {
    bool was;
    NoHostAttached() { was = MYSERIAL1.host_connected; MYSERIAL1.host_connected = false; }
    ~NoHostAttached() { MYSERIAL1.host_connected = was; }
  };

  // Send one command line, the way the queue would.
  void host_sends(const char * const line) {
    NoHostAttached quiet;
    static char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);   // true: skip the "ok" acknowledgement
  }

}

// M111 sets the debug flags used by the logging macros.
MARLIN_TEST(gcode_commands, M111_sets_debug_flags) {
  const uint8_t was = marlin_debug_flags;

  host_sends("M111 S0");
  TEST_ASSERT_EQUAL(0, marlin_debug_flags);

  host_sends("M111 S7");
  TEST_ASSERT_EQUAL(7, marlin_debug_flags);

  // Without S the flags are reported, not changed.
  host_sends("M111");
  TEST_ASSERT_EQUAL(7, marlin_debug_flags);

  marlin_debug_flags = was;
}

#if HAS_SOFTWARE_ENDSTOPS

  MARLIN_TEST(gcode_commands, M211_enables_and_disables_soft_endstops) {
    const bool was = motion.soft_endstop._enabled;

    host_sends("M211 S0");
    TEST_ASSERT_FALSE(motion.soft_endstop._enabled);

    host_sends("M211 S1");
    TEST_ASSERT_TRUE(motion.soft_endstop._enabled);

    // Without S the state is reported, not changed.
    host_sends("M211");
    TEST_ASSERT_TRUE(motion.soft_endstop._enabled);

    motion.soft_endstop._enabled = was;
  }

#endif

MARLIN_TEST(gcode_commands, M220_sets_the_feedrate_percentage) {
  const int16_t was = motion.feedrate_percentage;

  host_sends("M220 S50");
  TEST_ASSERT_EQUAL(50, motion.feedrate_percentage);

  host_sends("M220 S200");
  TEST_ASSERT_EQUAL(200, motion.feedrate_percentage);

  // Without any parameter the value is reported, not changed.
  host_sends("M220");
  TEST_ASSERT_EQUAL(200, motion.feedrate_percentage);

  motion.feedrate_percentage = was;
}

// B stores the current rate, R restores it — so a script can change the speed and put
// it back without knowing what it was.
MARLIN_TEST(gcode_commands, M220_backs_up_and_restores_the_feedrate) {
  const int16_t was = motion.feedrate_percentage;

  host_sends("M220 S100");
  host_sends("M220 B");            // remember 100
  host_sends("M220 S25");
  TEST_ASSERT_EQUAL(25, motion.feedrate_percentage);

  host_sends("M220 R");            // restore
  TEST_ASSERT_EQUAL(100, motion.feedrate_percentage);

  motion.feedrate_percentage = was;
}

#if HAS_EXTRUDERS

  MARLIN_TEST(gcode_commands, M221_sets_the_flow_percentage) {
    const int16_t was = planner.flow_percentage[0];

    host_sends("M221 S75");
    TEST_ASSERT_EQUAL(75, planner.flow_percentage[0]);

    host_sends("M221 S100");
    TEST_ASSERT_EQUAL(100, planner.flow_percentage[0]);

    planner.set_flow(0, was);
  }

#endif

#if ENABLED(PREVENT_COLD_EXTRUSION)

  MARLIN_TEST(gcode_commands, M302_sets_the_cold_extrusion_limit) {
    const celsius_t was_temp = thermalManager.extrude_min_temp;
    const bool was_allowed = thermalManager.allow_cold_extrude;

    host_sends("M302 S180");
    TEST_ASSERT_EQUAL(180, thermalManager.extrude_min_temp);
    TEST_ASSERT_FALSE(thermalManager.allow_cold_extrude);

    // P1 allows cold extrusion outright.
    host_sends("M302 P1");
    TEST_ASSERT_TRUE(thermalManager.allow_cold_extrude);

    host_sends("M302 P0");
    TEST_ASSERT_FALSE(thermalManager.allow_cold_extrude);

    // S0 means "no minimum", which is the same as allowing it.
    host_sends("M302 S0");
    TEST_ASSERT_EQUAL(0, thermalManager.extrude_min_temp);
    TEST_ASSERT_TRUE(thermalManager.allow_cold_extrude);

    thermalManager.extrude_min_temp = was_temp;
    thermalManager.allow_cold_extrude = was_allowed;
  }

#endif

// An unknown command must not be mistaken for a known one.
MARLIN_TEST(gcode_commands, an_unknown_command_changes_nothing) {
  const int16_t was = motion.feedrate_percentage;
  host_sends("M220 S123");
  TEST_ASSERT_EQUAL(123, motion.feedrate_percentage);

  host_sends("M99999 S50");
  TEST_ASSERT_EQUAL(123, motion.feedrate_percentage);

  motion.feedrate_percentage = was;
}

MARLIN_TEST(gcode_commands, M92_sets_steps_per_mm) {
  const float was_x = planner.settings.axis_steps_per_mm[X_AXIS],
              was_y = planner.settings.axis_steps_per_mm[Y_AXIS];

  host_sends("M92 X100 Y200");
  TEST_ASSERT_EQUAL_FLOAT(100.0f, planner.settings.axis_steps_per_mm[X_AXIS]);
  TEST_ASSERT_EQUAL_FLOAT(200.0f, planner.settings.axis_steps_per_mm[Y_AXIS]);

  // An axis that is not mentioned keeps its value.
  host_sends("M92 X80");
  TEST_ASSERT_EQUAL_FLOAT(80.0f, planner.settings.axis_steps_per_mm[X_AXIS]);
  TEST_ASSERT_EQUAL_FLOAT(200.0f, planner.settings.axis_steps_per_mm[Y_AXIS]);

  planner.settings.axis_steps_per_mm[X_AXIS] = was_x;
  planner.settings.axis_steps_per_mm[Y_AXIS] = was_y;
}

MARLIN_TEST(gcode_commands, M203_sets_max_feedrate) {
  const float was = planner.settings.max_feedrate_mm_s[X_AXIS];

  host_sends("M203 X250");
  TEST_ASSERT_EQUAL_FLOAT(250.0f, planner.settings.max_feedrate_mm_s[X_AXIS]);

  host_sends("M203 X300");
  TEST_ASSERT_EQUAL_FLOAT(300.0f, planner.settings.max_feedrate_mm_s[X_AXIS]);

  planner.settings.max_feedrate_mm_s[X_AXIS] = was;
}

MARLIN_TEST(gcode_commands, M201_sets_max_acceleration) {
  const uint32_t was = planner.settings.max_acceleration_mm_per_s2[X_AXIS];

  host_sends("M201 X1500");
  TEST_ASSERT_EQUAL(1500, planner.settings.max_acceleration_mm_per_s2[X_AXIS]);

  planner.settings.max_acceleration_mm_per_s2[X_AXIS] = was;
}

MARLIN_TEST(gcode_commands, M204_sets_accelerations) {
  const float was_p = planner.settings.acceleration,
              was_r = planner.settings.retract_acceleration,
              was_t = planner.settings.travel_acceleration;

  host_sends("M204 P500 R1000 T2000");
  TEST_ASSERT_EQUAL_FLOAT(500.0f, planner.settings.acceleration);
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, planner.settings.retract_acceleration);
  TEST_ASSERT_EQUAL_FLOAT(2000.0f, planner.settings.travel_acceleration);

  planner.settings.acceleration = was_p;
  planner.settings.retract_acceleration = was_r;
  planner.settings.travel_acceleration = was_t;
}

MARLIN_TEST(gcode_commands, M205_sets_minimum_feedrates) {
  const float was_s = planner.settings.min_feedrate_mm_s,
              was_t = planner.settings.min_travel_feedrate_mm_s;

  host_sends("M205 S10 T5");
  TEST_ASSERT_EQUAL_FLOAT(10.0f, planner.settings.min_feedrate_mm_s);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, planner.settings.min_travel_feedrate_mm_s);

  planner.settings.min_feedrate_mm_s = was_s;
  planner.settings.min_travel_feedrate_mm_s = was_t;
}

MARLIN_TEST(gcode_commands, M206_sets_the_home_offset) {
  const float was = motion.home_offset.x;

  host_sends("M206 X5");
  TEST_ASSERT_EQUAL_FLOAT(5.0f, motion.home_offset.x);

  host_sends("M206 X-2.5");
  TEST_ASSERT_EQUAL_FLOAT(-2.5f, motion.home_offset.x);

  motion.set_home_offset(X_AXIS, was);
}

// M428 sets the home offset from the current position, so the current spot becomes the
// new origin.
MARLIN_TEST(gcode_commands, M110_sets_the_line_number) {
  host_sends("M110 N42");
  TEST_ASSERT_EQUAL(42, queue.get_current_line_number());

  host_sends("M110 N0");
  TEST_ASSERT_EQUAL(0, queue.get_current_line_number());

  // A line number arriving with the command is itself recorded.
  host_sends("N7 M110 N7");
  TEST_ASSERT_EQUAL(7, queue.get_current_line_number());
}

// G92 does not move the machine: it makes the spot the tool is already at read as the
// given coordinate, by shifting the workspace. The native position is untouched, so a
// subsequent move goes where the new coordinate system says.
MARLIN_TEST(gcode_commands, G92_shifts_the_workspace_not_the_machine) {
  const float was_native = motion.position.x;
  const float was_offset = TERN0(HAS_WORKSPACE_OFFSET, motion.workspace_offset.x);

  host_sends("G92 X10");
  TEST_ASSERT_EQUAL_FLOAT(was_native, motion.position.x);        // machine has not moved
  #if HAS_WORKSPACE_OFFSET
    // The offset now maps this native spot to logical X10.
    TEST_ASSERT_EQUAL_FLOAT(10.0f, motion.position.asLogical().x);
  #endif

  host_sends("G92 X0");
  TEST_ASSERT_EQUAL_FLOAT(was_native, motion.position.x);
  #if HAS_WORKSPACE_OFFSET
    TEST_ASSERT_EQUAL_FLOAT(0.0f, motion.position.asLogical().x);
    motion.workspace_offset.x = was_offset;
  #endif
}

// M104 sets a hotend target without waiting for it.
MARLIN_TEST(gcode_commands, M104_sets_the_hotend_target) {
  const celsius_t was = thermalManager.degTargetHotend(0);

  host_sends("M104 S200");
  TEST_ASSERT_EQUAL(200, thermalManager.degTargetHotend(0));

  host_sends("M104 S0");
  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));

  thermalManager.setTargetHotend(was, 0);
}

#if HAS_HEATED_BED

  MARLIN_TEST(gcode_commands, M140_sets_the_bed_target) {
    const celsius_t was = thermalManager.degTargetBed();

    host_sends("M140 S60");
    TEST_ASSERT_EQUAL(60, thermalManager.degTargetBed());

    host_sends("M140 S0");
    TEST_ASSERT_EQUAL(0, thermalManager.degTargetBed());

    thermalManager.setTargetBed(was);
  }

#endif

// A dry run parses commands but must not actually heat anything — it is how a host
// checks a file without melting plastic.
MARLIN_TEST(gcode_commands, a_dry_run_does_not_set_a_temperature) {
  const uint8_t was_flags = marlin_debug_flags;
  const celsius_t was = thermalManager.degTargetHotend(0);

  host_sends("M104 S0");
  host_sends("M111 S8");                 // 8 = DRYRUN
  host_sends("M104 S250");
  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));

  marlin_debug_flags = was_flags;
  thermalManager.setTargetHotend(was, 0);
}

#if HAS_FAN

  MARLIN_TEST(gcode_commands, M106_and_M107_set_the_fan_speed) {
    const uint8_t was = thermalManager.fan_speed[0];

    host_sends("M106 S128");
    TEST_ASSERT_EQUAL(128, thermalManager.fan_speed[0]);

    // S is optional: M106 alone means full speed.
    host_sends("M106");
    TEST_ASSERT_EQUAL(255, thermalManager.fan_speed[0]);

    host_sends("M107");
    TEST_ASSERT_EQUAL(0, thermalManager.fan_speed[0]);

    thermalManager.fan_speed[0] = was;
  }

  // Speeds are clamped to a byte rather than wrapping.
  MARLIN_TEST(gcode_commands, fan_speed_above_the_maximum_is_clamped) {
    const uint8_t was = thermalManager.fan_speed[0];
    host_sends("M106 S999");
    TEST_ASSERT_EQUAL(255, thermalManager.fan_speed[0]);
    thermalManager.fan_speed[0] = was;
  }

#endif

/**
 * M109 and M190 wait until a temperature is reached, so a test must say what the
 * sensor reads or the command never returns. See simulated_sensors.h.
 */
MARLIN_TEST(gcode_commands, M109_returns_once_the_hotend_is_hot_enough) {
  const celsius_t was = thermalManager.degTargetHotend(0);
  SimulatedSensors sensors;

  SimulatedSensors::hotend_reads(205.0f);
  host_sends("M109 S200");                  // already at temperature: returns at once
  TEST_ASSERT_EQUAL(200, thermalManager.degTargetHotend(0));

  thermalManager.setTargetHotend(was, 0);
}

// Asking to cool does not wait for the hotend to actually cool down — a print that ends
// with M109 S0 should not sit there until the nozzle is cold.
MARLIN_TEST(gcode_commands, M109_does_not_wait_when_cooling) {
  const celsius_t was = thermalManager.degTargetHotend(0);
  SimulatedSensors sensors;

  SimulatedSensors::hotend_reads(220.0f);
  host_sends("M109 S180");                  // hotter than the target: cooling, no wait
  TEST_ASSERT_EQUAL(180, thermalManager.degTargetHotend(0));

  thermalManager.setTargetHotend(was, 0);
}

MARLIN_TEST(gcode_commands, M109_S0_returns_immediately) {
  const celsius_t was = thermalManager.degTargetHotend(0);
  host_sends("M109 S0");
  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));
  thermalManager.setTargetHotend(was, 0);
}

#if HAS_HEATED_BED

  MARLIN_TEST(gcode_commands, M190_returns_once_the_bed_is_hot_enough) {
    const celsius_t was = thermalManager.degTargetBed();
    SimulatedSensors sensors;

    SimulatedSensors::bed_reads(62.0f);
    host_sends("M190 S60");
    TEST_ASSERT_EQUAL(60, thermalManager.degTargetBed());

    thermalManager.setTargetBed(was);
  }

  MARLIN_TEST(gcode_commands, M190_S0_returns_immediately) {
    const celsius_t was = thermalManager.degTargetBed();
    host_sends("M190 S0");
    TEST_ASSERT_EQUAL(0, thermalManager.degTargetBed());
    thermalManager.setTargetBed(was);
  }

#endif

// M105 reports whatever the sensors read, which is what a host graphs.
// The sensor is driven at the ADC, so what it can read is quantised; the assertion is
// against the conversion the driven count predicts, not against a round number.
MARLIN_TEST(gcode_commands, M105_reports_the_temperature_the_sensor_reads) {
  SimulatedSensors sensors;
  SimulatedSensors::hotend_reads(123.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.25f, 123.0f, thermalManager.degHotend(0));
  TEST_ASSERT_EQUAL_FLOAT(SimulatedSensors::hotend_would_read(123.0f), thermalManager.degHotend(0));
}

MARLIN_TEST(gcode_commands, M118_echoes_its_argument_to_the_host) {
  SerialCapture capture;
  static char buf[64];
  strcpy(buf, "M118 hello world");
  parser.parse(buf);
  gcode.process_parsed_command(true);
  TEST_ASSERT_TRUE(capture.finish().find("hello world") != std::string::npos);
}

// A leading E1 means "prefix the line with echo:" rather than being part of the text.
MARLIN_TEST(gcode_commands, M118_flags_are_not_part_of_the_message) {
  SerialCapture capture;
  static char buf[64];
  strcpy(buf, "M118 E1 hello");
  parser.parse(buf);
  gcode.process_parsed_command(true);
  const std::string reply = capture.finish();
  TEST_ASSERT_TRUE(reply.find("hello") != std::string::npos);
  TEST_ASSERT_TRUE(reply.find("echo:") != std::string::npos);
  TEST_ASSERT_TRUE(reply.find("E1") == std::string::npos);
}

#if ENABLED(PIDTEMP)

  MARLIN_TEST(gcode_commands, M301_sets_the_pid_constants) {
    const float p = thermalManager.temp_hotend[0].pid.p(),
                i = thermalManager.temp_hotend[0].pid.i(),
                d = thermalManager.temp_hotend[0].pid.d();

    host_sends("M301 P11.5 I2.25 D63.75");
    TEST_ASSERT_EQUAL_FLOAT(11.5f, thermalManager.temp_hotend[0].pid.p());
    TEST_ASSERT_EQUAL_FLOAT(63.75f, thermalManager.temp_hotend[0].pid.d());

    SET_HOTEND_PID(Kp, 0, p); SET_HOTEND_PID(Ki, 0, i); SET_HOTEND_PID(Kd, 0, d);
    thermalManager.updatePID();
  }

  MARLIN_TEST(gcode_reports_pid, M301_reports_what_it_was_given) {
    const float p = thermalManager.temp_hotend[0].pid.p();
    host_sends("M301 P33.25");
    SerialCapture capture;
    static char buf[32];
    strcpy(buf, "M301");
    parser.parse(buf);
    gcode.process_parsed_command(true);
    TEST_ASSERT_TRUE(capture.finish().find("P33.25") != std::string::npos);
    SET_HOTEND_PID(Kp, 0, p);
    thermalManager.updatePID();
  }

#endif

// Settings can be reset to the configured defaults and reported back.
MARLIN_TEST(gcode_commands, M502_restores_the_configured_defaults) {
  const float was = planner.settings.axis_steps_per_mm[X_AXIS];

  host_sends("M92 X999");
  TEST_ASSERT_EQUAL_FLOAT(999.0f, planner.settings.axis_steps_per_mm[X_AXIS]);

  host_sends("M502");
  TEST_ASSERT_NOT_EQUAL(999.0f, planner.settings.axis_steps_per_mm[X_AXIS]);

  planner.settings.axis_steps_per_mm[X_AXIS] = was;
}

MARLIN_TEST(gcode_commands, M503_reports_the_settings) {
  SerialCapture capture;
  static char buf[32];
  strcpy(buf, "M503");
  parser.parse(buf);
  gcode.process_parsed_command(true);
  const std::string reply = capture.finish();
  TEST_ASSERT_TRUE(reply.find("M92") != std::string::npos);      // steps per mm
  TEST_ASSERT_TRUE(reply.find("M203") != std::string::npos);     // feedrate limits
}

// M17 and M18/M84 turn the motors on and off, which is what lets a user push the
// carriage by hand between prints.
MARLIN_TEST(gcode_commands, M17_and_M84_enable_and_disable_steppers) {
  host_sends("M17");
  TEST_ASSERT_TRUE(stepper.axis_is_enabled(X_AXIS));

  host_sends("M84");
  TEST_ASSERT_FALSE(stepper.axis_is_enabled(X_AXIS));

  // A named axis can be enabled on its own.
  host_sends("M17 X");
  TEST_ASSERT_TRUE(stepper.axis_is_enabled(X_AXIS));
  host_sends("M84");
}

// M120/M121 turn endstop checking on and off, which is how a macro moves past a limit
// deliberately.
MARLIN_TEST(gcode_commands, M120_and_M121_toggle_endstop_checking) {
  host_sends("M121");
  TEST_ASSERT_FALSE(endstops.global_enabled());

  host_sends("M120");
  TEST_ASSERT_TRUE(endstops.global_enabled());
}

MARLIN_TEST(gcode_commands, M113_sets_the_keepalive_interval) {
  host_sends("M113 S30");
  TEST_ASSERT_EQUAL(30, gcode.host_keepalive_interval);

  // The interval is capped so a host cannot ask to be ignored indefinitely.
  host_sends("M113 S200");
  TEST_ASSERT_EQUAL(60, gcode.host_keepalive_interval);

  host_sends("M113 S2");
}

MARLIN_TEST(gcode_reports_keepalive, M113_reports_the_interval) {
  host_sends("M113 S7");
  SerialCapture capture;
  static char buf[32];
  strcpy(buf, "M113");
  parser.parse(buf);
  gcode.process_parsed_command(true);
  TEST_ASSERT_TRUE(capture.finish().find("M113 S7") != std::string::npos);
  host_sends("M113 S2");
}

MARLIN_TEST(gcode_commands, M85_sets_the_inactivity_timeout) {
  const millis_t was = gcode.max_inactive_time;

  host_sends("M85 S90");
  TEST_ASSERT_EQUAL(90000, gcode.max_inactive_time);

  host_sends("M85 S0");                    // 0 disables the timeout
  TEST_ASSERT_EQUAL(0, gcode.max_inactive_time);

  gcode.max_inactive_time = was;
}

// M999 clears an error state so the printer will accept commands again.
MARLIN_TEST(gcode_commands, M999_returns_the_printer_to_running) {
  const MarlinState was = marlin.state;

  marlin.setState(MF_STOPPED);
  TEST_ASSERT_FALSE(marlin.isRunning());

  host_sends("M999 S1");                   // S1 skips the resend request
  TEST_ASSERT_TRUE(marlin.isRunning());

  marlin.setState(was);
}

// M108 breaks out of a wait, which is how a user abandons a heat-up that will not
// finish.
MARLIN_TEST(gcode_commands, M108_ends_a_wait) {
  const MarlinState was = marlin.state;
  marlin.setState(MF_RUNNING);

  host_sends("M108");
  TEST_ASSERT_FALSE(marlin.is_heating());

  marlin.setState(was);
}

#if HAS_POWER_SWITCH
  MARLIN_TEST(gcode_commands, M80_and_M81_switch_the_power_supply) {
    host_sends("M80");
    TEST_ASSERT_TRUE(powerManager.psu_on);

    host_sends("M81");
    TEST_ASSERT_FALSE(powerManager.psu_on);
  }
#endif
