/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
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
 * About Marlin
 *
 * This firmware is a mashup between Sprinter and grbl.
 *  - https://github.com/kliment/Sprinter
 *  - https://github.com/grbl/grbl
 */

#include "MarlinCore.h"

#include "HAL/shared/Delay.h"
#include "HAL/shared/esp_wifi.h"
#include "HAL/shared/cpu_exception/exception_hook.h"

#if ENABLED(WIFISUPPORT)
  #include "HAL/shared/esp_wifi.h"
#endif

#ifdef ARDUINO
  #include <pins_arduino.h>
#endif
#include <math.h>

#include "module/endstops.h"
#include "module/motion.h"
#include "module/planner.h"
#include "module/printcounter.h" // PrintCounter or Stopwatch
#include "module/settings.h"
#include "module/stepper.h"
#include "module/temperature.h"
#if ENABLED(FT_MOTION)
  #include "module/ft_motion.h"
#endif

#include "gcode/gcode.h"
#include "gcode/parser.h"
#include "gcode/queue.h"

#include "feature/pause.h"
#include "sd/cardreader.h"

#include "lcd/marlinui.h"
#if HAS_TOUCH_BUTTONS
  #include "lcd/touch/touch_buttons.h"
#endif

#if HAS_TFT_LVGL_UI
  #include "lcd/extui/mks_ui/tft_lvgl_configuration.h"
  #include "lcd/extui/mks_ui/draw_ui.h"
  #include "lcd/extui/mks_ui/mks_hardware.h"
  #include <lvgl.h>
#endif

#if HAS_DWIN_E3V2
  #include "lcd/dwin/common/encoder.h"
  #if ENABLED(DWIN_CREALITY_LCD)
    #include "lcd/dwin/creality/dwin.h"
  #elif ENABLED(DWIN_CREALITY_LCD_JYERSUI)
    #include "lcd/dwin/jyersui/dwin.h"
  #elif ENABLED(SOVOL_SV06_RTS)
    #include "lcd/sovol_rts/sovol_rts.h"
  #endif
#endif

#if HAS_ETHERNET
  #include "feature/ethernet.h"
#endif

#if ENABLED(IIC_BL24CXX_EEPROM)
  #include "libs/BL24CXX.h"
#endif

#if ENABLED(DIRECT_STEPPING)
  #include "feature/direct_stepping.h"
#endif

#if ENABLED(HOST_ACTION_COMMANDS)
  #include "feature/host_actions.h"
#endif

#if HAS_BEEPER
  #include "libs/buzzer.h"
#endif

#if ENABLED(EXTERNAL_CLOSED_LOOP_CONTROLLER)
  #include "feature/closedloop.h"
#endif

#if HAS_MOTOR_CURRENT_I2C
  #include "feature/digipot/digipot.h"
#endif

#if ENABLED(MIXING_EXTRUDER)
  #include "feature/mixing.h"
#endif

#if ENABLED(MAX7219_DEBUG)
  #include "feature/max7219.h"
#endif

#if HAS_COLOR_LEDS
  #include "feature/leds/leds.h"
#endif

#if ENABLED(BLTOUCH)
  #include "feature/bltouch.h"
#endif

#if ENABLED(BD_SENSOR)
  #include "feature/bedlevel/bdl/bdl.h"
#endif

#if ENABLED(POLL_JOG)
  #include "feature/joystick.h"
#endif

#if HAS_SERVOS
  #include "module/servo.h"
#endif

#if HAS_MOTOR_CURRENT_DAC
  #include "feature/dac/stepper_dac.h"
#endif

#if ENABLED(EXPERIMENTAL_I2CBUS)
  #include "feature/twibus.h"
#endif

#if ENABLED(I2C_POSITION_ENCODERS)
  #include "feature/encoder_i2c.h"
#endif

#if HAS_TRINAMIC_CONFIG
  #include "module/stepper/trinamic.h"
#endif

#if HAS_CUTTER
  #include "feature/spindle_laser.h"
#endif

#if ENABLED(DELTA)
  #include "module/delta.h"
#elif ENABLED(POLARGRAPH)
  #include "module/polargraph.h"
#elif IS_SCARA
  #include "module/scara.h"
#elif ENABLED(POLAR)
  #include "module/polar.h"
#endif

#if HAS_LEVELING
  #include "feature/bedlevel/bedlevel.h"
#endif

#if ENABLED(GCODE_REPEAT_MARKERS)
  #include "feature/repeat.h"
#endif

#if ENABLED(POWER_LOSS_RECOVERY)
  #include "feature/powerloss.h"
#endif

#if ENABLED(CANCEL_OBJECTS)
  #include "feature/cancel_object.h"
#endif

#if HAS_FILAMENT_SENSOR
  #include "feature/runout.h"
#endif

#if ANY(PROBE_TARE, HAS_Z_SERVO_PROBE)
  #include "module/probe.h"
#endif

#if ENABLED(HOTEND_IDLE_TIMEOUT)
  #include "feature/hotend_idle.h"
#endif

#if ENABLED(TEMP_STAT_LEDS)
  #include "feature/leds/tempstat.h"
#endif

#if ENABLED(CASE_LIGHT_ENABLE)
  #include "feature/caselight.h"
#endif

#if HAS_FANMUX
  #include "feature/fanmux.h"
#endif

#if HAS_TOOLCHANGE
  #include "module/tool_change.h"
#endif

#if HAS_FANCHECK
  #include "feature/fancheck.h"
#endif

#if ENABLED(USE_CONTROLLER_FAN)
  #include "feature/controllerfan.h"
#endif

#if HAS_PRUSA_MMU3
  #include "feature/mmu3/mmu3.h"
  #include "feature/mmu3/mmu3_reporting.h"
  #include "feature/mmu3/SpoolJoin.h"
#elif HAS_PRUSA_MMU2
  #include "feature/mmu/mmu2.h"
#elif HAS_PRUSA_MMU1
  #include "feature/mmu/mmu.h"
#endif

#if ENABLED(PASSWORD_FEATURE)
  #include "feature/password/password.h"
#endif

#if DGUS_LCD_UI_MKS
  #include "lcd/extui/dgus/DGUSScreenHandler.h"
#endif

#if HAS_DRIVER_SAFE_POWER_PROTECT
  #include "feature/stepper_driver_safety.h"
#endif

#if ENABLED(PSU_CONTROL)
  #include "feature/power.h"
#endif

#if ENABLED(EASYTHREED_UI)
  #include "feature/easythreed_ui.h"
#endif

#if ENABLED(MARLIN_TEST_BUILD)
  #include "tests/marlin_tests.h"
#endif

#if HAS_RS485_SERIAL
  #include "feature/rs485.h"
#endif

#if ENABLED(SOFT_FEED_HOLD)
  #include "feature/e_parser.h"
#endif

/**
 * Spin in place here while keeping temperature processing alive
 */
void safe_delay(millis_t ms) {
  while (ms > 50) {
    ms -= 50;
    delay(50);
    thermalManager.task();
  }
  delay(ms);
  thermalManager.task(); // This keeps us safe if too many small safe_delay() calls are made
}

// Singleton for Marlin global data and methods
Marlin marlin;

// Marlin static data
#if ENABLED(CONFIGURABLE_MACHINE_NAME)
  MString<64> Marlin::machine_name;
#endif

// Global state of the firmware
MarlinState Marlin::state = MF_INITIALIZING;

// For M109 and M190, this flag may be cleared (by M108) to exit the wait loop
bool Marlin::wait_for_heatup = false;

#if !HAS_MEDIA
  CardReader card; // Stub instance with "no media" methods
#endif

PGMSTR(M112_KILL_STR, "M112 Shutdown");

// For M0/M1, this flag may be cleared (by M108) to exit the wait-for-user loop
#if HAS_RESUME_CONTINUE
  bool Marlin::wait_for_user; // = false

  void Marlin::wait_for_user_response(millis_t ms/*=0*/, const bool no_sleep/*=false*/) {
    UNUSED(no_sleep);
    KEEPALIVE_STATE(PAUSED_FOR_USER);
    wait_start();
    if (ms) ms += millis(); // expire time
    while (wait_for_user && !(ms && ELAPSED(millis(), ms)))
      idle(TERN_(ADVANCED_PAUSE_FEATURE, no_sleep));
    user_resume();
    while (ui.button_pressed()) safe_delay(50);
  }

#endif

/**
 * ***************************************************************************
 * ******************************** FUNCTIONS ********************************
 * ***************************************************************************
 */

/**
 * Stepper Reset (RigidBoard, et.al.)
 */
#if HAS_STEPPER_RESET
  void disableStepperDrivers() { OUT_WRITE(STEPPER_RESET_PIN, LOW); } // Drive down to keep motor driver chips in reset
  void enableStepperDrivers()  { SET_INPUT(STEPPER_RESET_PIN); }      // Set to input, allowing pullups to pull the pin high
#endif

/**
 * Sensitive pin test for M42, M226
 */

#include "pins/sensitive_pins.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"
#pragma GCC diagnostic ignored "-Wsign-compare"

bool Marlin::pin_is_protected(const pin_t pin) {
  #define pgm_read_pin(P) (sizeof(pin_t) == 2 ? (pin_t)pgm_read_word(P) : (pin_t)pgm_read_byte(P))
  for (uint8_t i = 0; i < COUNT(sensitive_dio); ++i)
    if (pin == pgm_read_pin(&sensitive_dio[i])) return true;
  for (uint8_t i = 0; i < COUNT(sensitive_aio); ++i)
    if (pin == analogInputToDigitalPin(pgm_read_pin(&sensitive_aio[i]))) return true;
  return false;
}

#pragma GCC diagnostic pop

/**
 * Why the board came up.
 *
 * The reset source is the only evidence an operator has of *why* a printer restarted, and the
 * five causes mean very different things: a watchdog reset says the firmware stopped answering,
 * a brown-out says the supply sagged, an external reset says somebody pressed the button. They
 * are flags rather than an enumeration, and more than one can be set — a brown-out that also
 * tripped the watchdog reports both.
 *
 * Extracted from `setup()` so it can be asserted. It is a pure function of the mask, which is
 * what makes it worth having on this side of the boundary: `setup()` reads the register, this
 * decides what that means.
 */
void Marlin::report_reset_reason(const uint8_t mcu) {
  if (mcu & RST_POWER_ON)  SERIAL_ECHOLNPGM(STR_POWERUP);
  if (mcu & RST_EXTERNAL)  SERIAL_ECHOLNPGM(STR_EXTERNAL_RESET);
  if (mcu & RST_BROWN_OUT) SERIAL_ECHOLNPGM(STR_BROWNOUT_RESET);
  if (mcu & RST_WATCHDOG)  SERIAL_ECHOLNPGM(STR_WATCHDOG_RESET);
  if (mcu & RST_SOFTWARE)  SERIAL_ECHOLNPGM(STR_SOFTWARE_RESET);
}

/**
 * Who this firmware is.
 *
 * The banner a host reads to decide what it is talking to — the version it will match its
 * capabilities against, the build date, and the memory figures that say whether the planner
 * buffer is the size the configuration asked for. Also extracted from `setup()`: none of it
 * touches hardware beyond asking the HAL how much memory is free.
 */
void Marlin::report_firmware_identity() {
  SERIAL_ECHOLNPGM("Marlin " SHORT_BUILD_VERSION);
  #ifdef STRING_DISTRIBUTION_DATE
    SERIAL_ECHO_MSG(
      " Last Updated: " STRING_DISTRIBUTION_DATE
      " | Author: " STRING_CONFIG_H_AUTHOR
    );
  #endif
  SERIAL_ECHO_MSG(" Compiled: " __DATE__);
  SERIAL_ECHO_MSG(STR_FREE_MEMORY, hal.freeMemory(), STR_PLANNER_BUFFER_BYTES, sizeof(block_t) * (BLOCK_BUFFER_SIZE));
}

bool Marlin::printer_busy() {
  return planner.has_blocks_queued() || printingIsActive();
}

/**
 * A Print Job exists when the timer is running or SD is printing
 */
bool Marlin::printJobOngoing() { return print_job_timer.isRunning() || card.isStillPrinting(); }

/**
 * Printing is active when a job is underway but not paused
 */
bool Marlin::printingIsActive() { return !did_pause_print && printJobOngoing(); }

/**
 * Printing is paused according to SD or host indicators
 */
bool Marlin::printingIsPaused() {
  return did_pause_print || print_job_timer.isPaused() || card.isPaused();
}

void Marlin::startOrResumeJob() {
  if (!printingIsPaused()) {
    TERN_(GCODE_REPEAT_MARKERS, repeat.reset());
    TERN_(CANCEL_OBJECTS, cancelable.reset());
    TERN_(LCD_SHOW_E_TOTAL, motion.e_move_accumulator = 0);
    TERN_(SET_REMAINING_TIME, ui.reset_remaining_time());
    TERN_(HAS_PRUSA_MMU3, MMU3::operation_statistics.reset_per_print_stats());
  }
  print_job_timer.start();
}

#if HAS_MEDIA

  void abortSDPrinting() {
    IF_DISABLED(NO_SD_AUTOSTART, card.autofile_cancel());
    card.abortFilePrintNow(TERN_(SD_RESORT, true));

    queue.clear();
    motion.quickstop_stepper();

    print_job_timer.abort();

    IF_DISABLED(SD_ABORT_NO_COOLDOWN, thermalManager.disable_all_heaters());

    TERN(HAS_CUTTER, cutter.kill(), thermalManager.zero_fan_speeds()); // Full cutter shutdown including ISR control

    marlin.heatup_done();

    TERN_(POWER_LOSS_RECOVERY, recovery.purge());

    #ifdef EVENT_GCODE_SD_ABORT
      queue.inject(F(EVENT_GCODE_SD_ABORT));
    #endif

    TERN_(PASSWORD_AFTER_SD_PRINT_ABORT, password.lock_machine());
  }

  void finishSDPrinting() {
    if (queue.enqueue_one(F("M1001"))) {  // Keep trying until it gets queued
      marlin.setState(MF_RUNNING);        // Signal to stop trying
      TERN_(PASSWORD_AFTER_SD_PRINT_END, password.lock_machine());
      TERN_(DGUS_LCD_UI_MKS, screen.sdPrintingFinished());
    }
  }

#endif // HAS_MEDIA

/**
 * Minimal management of Marlin's core activities:
 *  - Keep the command buffer full
 *  - Check for maximum inactive time between commands
 *  - Check for maximum inactive time between stepper commands
 *  - Check if CHDK_PIN needs to go LOW
 *  - Check for KILL button held down
 *  - Check for HOME button held down
 *  - Check for CUSTOM USER button held down
 *  - Check if cooling fan needs to be switched on
 *  - Check if an idle but hot extruder needs filament extruded (EXTRUDER_RUNOUT_PREVENT)
 *  - Pulse FET_SAFETY_PIN if it exists
 */
void Marlin::manage_inactivity(const bool no_stepper_sleep/*=false*/) {

  queue.get_available_commands();

  const millis_t ms = millis();

  // Prevent steppers timing-out
  const bool do_reset_timeout = no_stepper_sleep
                               || TERN0(PAUSE_PARK_NO_STEPPER_TIMEOUT, did_pause_print);

  // Reset both the M18/M84 activity timeout and the M85 max 'kill' timeout
  if (do_reset_timeout) gcode.reset_stepper_timeout(ms);

  if (gcode.stepper_max_timed_out(ms)) {
    SERIAL_ERROR_START();
    SERIAL_ECHOLN(F(STR_KILL_PRE), F(STR_KILL_INACTIVE_TIME), parser.command_ptr);
    kill();
  }

  const bool has_blocks = planner.has_blocks_queued();  // Any moves in the planner?
  if (has_blocks) gcode.reset_stepper_timeout(ms);      // Reset timeout for M18/M84, M85 max 'kill', and laser.

  // M18 / M84 : Handle steppers inactive time timeout
  #if HAS_DISABLE_IDLE_AXES
    if (gcode.stepper_inactive_time) {

      static bool already_shutdown_steppers; // = false

      if (!has_blocks && !do_reset_timeout && gcode.stepper_inactive_timeout()) {
        if (!already_shutdown_steppers) {
          already_shutdown_steppers = true;

          // Individual axes will be disabled if configured
          TERN_(DISABLE_IDLE_X, stepper.disable_axis(X_AXIS));
          TERN_(DISABLE_IDLE_Y, stepper.disable_axis(Y_AXIS));
          TERN_(DISABLE_IDLE_Z, stepper.disable_axis(Z_AXIS));
          TERN_(DISABLE_IDLE_I, stepper.disable_axis(I_AXIS));
          TERN_(DISABLE_IDLE_J, stepper.disable_axis(J_AXIS));
          TERN_(DISABLE_IDLE_K, stepper.disable_axis(K_AXIS));
          TERN_(DISABLE_IDLE_U, stepper.disable_axis(U_AXIS));
          TERN_(DISABLE_IDLE_V, stepper.disable_axis(V_AXIS));
          TERN_(DISABLE_IDLE_W, stepper.disable_axis(W_AXIS));
          TERN_(DISABLE_IDLE_E, stepper.disable_e_steppers());

          TERN_(AUTO_BED_LEVELING_UBL, bedlevel.steppers_were_disabled());
        }
      }
      else
        already_shutdown_steppers = false;
    }
  #endif

  #if ENABLED(PHOTO_GCODE) && PIN_EXISTS(CHDK)
    // Check if CHDK should be set to LOW (after M240 set it HIGH)
    extern millis_t chdk_timeout;
    if (chdk_timeout && ELAPSED(ms, chdk_timeout)) {
      chdk_timeout = 0;
      WRITE(CHDK_PIN, LOW);
    }
  #endif

  #if HAS_KILL

    // Check if the kill button was pressed and wait to ensure the signal is not noise
    // typically caused by poor insulation and grounding on LCD cables.
    // Lower numbers here will increase response time and therefore safety rating.
    // It is recommended to set this as low as possible without false triggers.
    // -------------------------------------------------------------------------------
    #ifndef KILL_DELAY
      #define KILL_DELAY 250
    #endif

    static int killCount = 0;   // make the inactivity button a bit less responsive
    if (kill_state())
      killCount++;
    else if (killCount > 0)
      killCount--;

    // Exceeded threshold and we can confirm that it was not accidental
    // KILL the machine
    // ----------------------------------------------------------------
    if (killCount >= KILL_DELAY) {
      SERIAL_ERROR_START();
      SERIAL_ECHOLN(F(STR_KILL_PRE), F(STR_KILL_BUTTON));
      kill();
    }
  #endif

  // Handle the FREEZE button
  #if ANY(FREEZE_FEATURE, SOFT_FEED_HOLD)
    stepper.set_frozen_triggered(
      TERN0(FREEZE_FEATURE, READ(FREEZE_PIN) == FREEZE_STATE)
      #if ALL(SOFT_FEED_HOLD, REALTIME_REPORTING_COMMANDS)
        || realtime_ramping_pause_flag
      #endif
    );
  #endif

  #if HAS_HOME
    // Handle a standalone HOME button
    constexpr millis_t HOME_DEBOUNCE_DELAY = 1000UL;
    static millis_t next_home_key_ms; // = 0
    if (!card.isStillPrinting() && !READ(HOME_PIN)) { // HOME_PIN goes LOW when pressed
      if (ELAPSED(ms, next_home_key_ms)) {
        next_home_key_ms = ms + HOME_DEBOUNCE_DELAY;
        LCD_MESSAGE(MSG_AUTO_HOME);
        queue.inject_P(G28_STR);
      }
    }
  #endif

  #if ENABLED(CUSTOM_USER_BUTTONS)
    // Handle a custom user button if defined
    const bool printer_not_busy = !printingIsActive();
    #define HAS_CUSTOM_USER_BUTTON(N) (PIN_EXISTS(BUTTON##N) && defined(BUTTON##N##_HIT_STATE) && defined(BUTTON##N##_GCODE))
    #define HAS_BETTER_USER_BUTTON(N) HAS_CUSTOM_USER_BUTTON(N) && defined(BUTTON##N##_DESC)
    #define _CHECK_CUSTOM_USER_BUTTON(N, CODE) do{                     \
      constexpr millis_t CUB_DEBOUNCE_DELAY_##N = 250UL;               \
      static millis_t next_cub_ms_##N;                                 \
      if (BUTTON##N##_HIT_STATE == READ(BUTTON##N##_PIN)               \
        && (ENABLED(BUTTON##N##_WHEN_PRINTING) || printer_not_busy)    \
      ) {                                                              \
        if (ELAPSED(ms, next_cub_ms_##N)) {                            \
          next_cub_ms_##N = ms + CUB_DEBOUNCE_DELAY_##N;               \
          CODE;                                                        \
          if (ENABLED(BUTTON##N##_IMMEDIATE))                          \
            gcode.process_subcommands_now(F(BUTTON##N##_GCODE));       \
          else                                                         \
            queue.inject(F(BUTTON##N##_GCODE));                        \
          TERN_(HAS_MARLINUI_MENU, ui.quick_feedback());               \
        }                                                              \
      }                                                                \
    }while(0)

    #define CHECK_CUSTOM_USER_BUTTON(N) _CHECK_CUSTOM_USER_BUTTON(N, NOOP)
    #define CHECK_BETTER_USER_BUTTON(N) _CHECK_CUSTOM_USER_BUTTON(N, if (strlen(BUTTON##N##_DESC)) LCD_MESSAGE_F(BUTTON##N##_DESC))

    #if HAS_BETTER_USER_BUTTON(1)
      CHECK_BETTER_USER_BUTTON(1);
    #elif HAS_CUSTOM_USER_BUTTON(1)
      CHECK_CUSTOM_USER_BUTTON(1);
    #endif
    #if HAS_BETTER_USER_BUTTON(2)
      CHECK_BETTER_USER_BUTTON(2);
    #elif HAS_CUSTOM_USER_BUTTON(2)
      CHECK_CUSTOM_USER_BUTTON(2);
    #endif
    #if HAS_BETTER_USER_BUTTON(3)
      CHECK_BETTER_USER_BUTTON(3);
    #elif HAS_CUSTOM_USER_BUTTON(3)
      CHECK_CUSTOM_USER_BUTTON(3);
    #endif
    #if HAS_BETTER_USER_BUTTON(4)
      CHECK_BETTER_USER_BUTTON(4);
    #elif HAS_CUSTOM_USER_BUTTON(4)
      CHECK_CUSTOM_USER_BUTTON(4);
    #endif
    #if HAS_BETTER_USER_BUTTON(5)
      CHECK_BETTER_USER_BUTTON(5);
    #elif HAS_CUSTOM_USER_BUTTON(5)
      CHECK_CUSTOM_USER_BUTTON(5);
    #endif
    #if HAS_BETTER_USER_BUTTON(6)
      CHECK_BETTER_USER_BUTTON(6);
    #elif HAS_CUSTOM_USER_BUTTON(6)
      CHECK_CUSTOM_USER_BUTTON(6);
    #endif
    #if HAS_BETTER_USER_BUTTON(7)
      CHECK_BETTER_USER_BUTTON(7);
    #elif HAS_CUSTOM_USER_BUTTON(7)
      CHECK_CUSTOM_USER_BUTTON(7);
    #endif
    #if HAS_BETTER_USER_BUTTON(8)
      CHECK_BETTER_USER_BUTTON(8);
    #elif HAS_CUSTOM_USER_BUTTON(8)
      CHECK_CUSTOM_USER_BUTTON(8);
    #endif
    #if HAS_BETTER_USER_BUTTON(9)
      CHECK_BETTER_USER_BUTTON(9);
    #elif HAS_CUSTOM_USER_BUTTON(9)
      CHECK_CUSTOM_USER_BUTTON(9);
    #endif
    #if HAS_BETTER_USER_BUTTON(10)
      CHECK_BETTER_USER_BUTTON(10);
    #elif HAS_CUSTOM_USER_BUTTON(10)
      CHECK_CUSTOM_USER_BUTTON(10);
    #endif
    #if HAS_BETTER_USER_BUTTON(11)
      CHECK_BETTER_USER_BUTTON(11);
    #elif HAS_CUSTOM_USER_BUTTON(11)
      CHECK_CUSTOM_USER_BUTTON(11);
    #endif
    #if HAS_BETTER_USER_BUTTON(12)
      CHECK_BETTER_USER_BUTTON(12);
    #elif HAS_CUSTOM_USER_BUTTON(12)
      CHECK_CUSTOM_USER_BUTTON(12);
    #endif
    #if HAS_BETTER_USER_BUTTON(13)
      CHECK_BETTER_USER_BUTTON(13);
    #elif HAS_CUSTOM_USER_BUTTON(13)
      CHECK_CUSTOM_USER_BUTTON(13);
    #endif
    #if HAS_BETTER_USER_BUTTON(14)
      CHECK_BETTER_USER_BUTTON(14);
    #elif HAS_CUSTOM_USER_BUTTON(14)
      CHECK_CUSTOM_USER_BUTTON(14);
    #endif
    #if HAS_BETTER_USER_BUTTON(15)
      CHECK_BETTER_USER_BUTTON(15);
    #elif HAS_CUSTOM_USER_BUTTON(15)
      CHECK_CUSTOM_USER_BUTTON(15);
    #endif
    #if HAS_BETTER_USER_BUTTON(16)
      CHECK_BETTER_USER_BUTTON(16);
    #elif HAS_CUSTOM_USER_BUTTON(16)
      CHECK_CUSTOM_USER_BUTTON(16);
    #endif
    #if HAS_BETTER_USER_BUTTON(17)
      CHECK_BETTER_USER_BUTTON(17);
    #elif HAS_CUSTOM_USER_BUTTON(17)
      CHECK_CUSTOM_USER_BUTTON(17);
    #endif
    #if HAS_BETTER_USER_BUTTON(18)
      CHECK_BETTER_USER_BUTTON(18);
    #elif HAS_CUSTOM_USER_BUTTON(18)
      CHECK_CUSTOM_USER_BUTTON(18);
    #endif
    #if HAS_BETTER_USER_BUTTON(19)
      CHECK_BETTER_USER_BUTTON(19);
    #elif HAS_CUSTOM_USER_BUTTON(19)
      CHECK_CUSTOM_USER_BUTTON(19);
    #endif
    #if HAS_BETTER_USER_BUTTON(20)
      CHECK_BETTER_USER_BUTTON(20);
    #elif HAS_CUSTOM_USER_BUTTON(20)
      CHECK_CUSTOM_USER_BUTTON(20);
    #endif
    #if HAS_BETTER_USER_BUTTON(21)
      CHECK_BETTER_USER_BUTTON(21);
    #elif HAS_CUSTOM_USER_BUTTON(21)
      CHECK_CUSTOM_USER_BUTTON(21);
    #endif
    #if HAS_BETTER_USER_BUTTON(22)
      CHECK_BETTER_USER_BUTTON(22);
    #elif HAS_CUSTOM_USER_BUTTON(22)
      CHECK_CUSTOM_USER_BUTTON(22);
    #endif
    #if HAS_BETTER_USER_BUTTON(23)
      CHECK_BETTER_USER_BUTTON(23);
    #elif HAS_CUSTOM_USER_BUTTON(23)
      CHECK_CUSTOM_USER_BUTTON(23);
    #endif
    #if HAS_BETTER_USER_BUTTON(24)
      CHECK_BETTER_USER_BUTTON(24);
    #elif HAS_CUSTOM_USER_BUTTON(24)
      CHECK_CUSTOM_USER_BUTTON(24);
    #endif
    #if HAS_BETTER_USER_BUTTON(25)
      CHECK_BETTER_USER_BUTTON(25);
    #elif HAS_CUSTOM_USER_BUTTON(25)
      CHECK_CUSTOM_USER_BUTTON(25);
    #endif
  #endif

  TERN_(EASYTHREED_UI, easythreed_ui.run());

  TERN_(USE_CONTROLLER_FAN, controllerFan.update()); // Check if fan should be turned on to cool stepper drivers down

  TERN_(AUTO_POWER_CONTROL, powerManager.check(!ui.on_status_screen() || printJobOngoing() || printingIsPaused()));

  TERN_(HOTEND_IDLE_TIMEOUT, hotend_idle.check());

  #if ANY(PSU_CONTROL, AUTO_POWER_CONTROL) && PIN_EXISTS(PS_ON_EDM)
    if ( ELAPSED(ms, powerManager.last_state_change_ms, PS_EDM_RESPONSE)
      && (READ(PS_ON_PIN) != READ(PS_ON_EDM_PIN) || TERN0(PSU_OFF_REDUNDANT, extDigitalRead(PS_ON1_PIN) != extDigitalRead(PS_ON1_EDM_PIN)))
    ) kill(GET_TEXT_F(MSG_POWER_EDM_FAULT));
  #endif

  #if ENABLED(EXTRUDER_RUNOUT_PREVENT)
    if (thermalManager.degHotend(motion.extruder) > (EXTRUDER_RUNOUT_MINTEMP)
      && ELAPSED(ms, gcode.previous_move_ms, SEC_TO_MS(EXTRUDER_RUNOUT_SECONDS))
      && !planner.has_blocks_queued()
    ) {
      const int8_t e_stepper = TERN(HAS_SWITCHING_EXTRUDER, motion.extruder / 2, motion.extruder);
      const bool e_off = !stepper.AXIS_IS_ENABLED(E_AXIS, e_stepper);
      if (e_off) stepper.ENABLE_EXTRUDER(e_stepper);

      const float olde = motion.position.e;
      motion.position.e += EXTRUDER_RUNOUT_EXTRUDE;
      motion.goto_current_position(MMM_TO_MMS(EXTRUDER_RUNOUT_SPEED));
      motion.position.e = olde;
      motion.sync_plan_position_e();
      planner.synchronize();

      if (e_off) stepper.DISABLE_EXTRUDER(e_stepper);

      gcode.reset_stepper_timeout(ms);
    }
  #endif // EXTRUDER_RUNOUT_PREVENT

  #if ENABLED(DUAL_X_CARRIAGE)
    // handle delayed move timeout
    if (motion.delayed_move_time && ELAPSED(ms, motion.delayed_move_time) && isRunning()) {
      // travel moves have been received so enact them
      motion.delayed_move_time = UINT32_MAX; // force moves to be done
      motion.destination = motion.position;
      motion.prepare_line_to_destination();
      planner.synchronize();
    }
  #endif

  TERN_(TEMP_STAT_LEDS, handle_status_leds());

  TERN_(MONITOR_DRIVER_STATUS, monitor_tmc_drivers());

  // Limit check_axes_activity frequency to 10Hz
  static millis_t next_check_axes_ms = 0;
  if (ELAPSED(ms, next_check_axes_ms)) {
    planner.check_axes_activity();
    next_check_axes_ms = ms + 100UL;
  }

  #if PIN_EXISTS(FET_SAFETY)
    static millis_t FET_next;
    if (ELAPSED(ms, FET_next)) {
      FET_next = ms + FET_SAFETY_DELAY;  // 2µs pulse every FET_SAFETY_DELAY mS
      OUT_WRITE(FET_SAFETY_PIN, !FET_SAFETY_INVERTED);
      DELAY_US(2);
      WRITE(FET_SAFETY_PIN, FET_SAFETY_INVERTED);
    }
  #endif

} // Marlin::manage_inactivity()

#if ALL(EP_BABYSTEPPING, EMERGENCY_PARSER)
  #include "feature/babystep.h"
#endif

/**
 * Standard idle routine keeps the machine alive:
 *  - Core Marlin activities
 *  - Manage heaters (and Watchdog)
 *  - Max7219 heartbeat, animation, etc.
 *
 *  Only after setup() is complete:
 *  - Handle filament runout sensors
 *  - Run HAL idle tasks
 *  - Handle Power-Loss Recovery
 *  - Run StallGuard endstop checks
 *  - Handle SD Card insert / remove
 *  - Handle USB Flash Drive insert / remove
 *  - Announce Host Keepalive state (if any)
 *  - Update the Print Job Timer state
 *  - Update the Beeper queue
 *  - Read Buttons and Update the LCD
 *  - Run i2c Position Encoders
 *  - Auto-report Temperatures / SD Status
 *  - Update the Průša MMU2
 *  - Handle Joystick jogging
 */
void Marlin::idle(const bool no_stepper_sleep/*=false*/) {
  #ifdef MAX7219_DEBUG_PROFILE
    CodeProfiler idle_profiler;
  #endif

  #if ENABLED(MARLIN_DEV_MODE)
    static uint16_t idle_depth = 0;
    if (++idle_depth > 5) SERIAL_ECHOLNPGM("Marlin::idle() call depth: ", idle_depth);
  #endif

  // Bed Distance Sensor task
  TERN_(BD_SENSOR, bdl.process());

  // Core Marlin activities
  manage_inactivity(no_stepper_sleep);

  // Manage Heaters (and Watchdog)
  thermalManager.task();

  // Max7219 heartbeat, animation, etc
  TERN_(MAX7219_DEBUG, max7219.idle_tasks());

  // Return if setup() isn't completed
  if (is(MF_INITIALIZING)) goto IDLE_DONE;

  // TODO: Still causing errors
  TERN_(TOOL_SENSOR, (void)check_tool_sensor_stats(motion.extruder, true));

  // Handle filament runout sensors
  #if HAS_FILAMENT_SENSOR
    if (TERN1(HAS_PRUSA_MMU2, !mmu2.enabled()) && TERN1(HAS_PRUSA_MMU3, !mmu3.enabled()))
      runout.run();
  #endif

  // Run HAL idle tasks
  hal.idletask();

  // Check network connection
  TERN_(HAS_ETHERNET, ethernet.check());

  // Handle Power-Loss Recovery
  #if ENABLED(POWER_LOSS_RECOVERY) && PIN_EXISTS(POWER_LOSS)
    if (card.isStillPrinting()) recovery.outage();
  #endif

  // Run StallGuard endstop checks
  #if ENABLED(SPI_ENDSTOPS)
    if (endstops.tmc_spi_homing.any && TERN1(IMPROVE_HOMING_RELIABILITY, ELAPSED(millis(), sg_guard_period)))
      for (uint8_t i = 0; i < 4; ++i) if (endstops.tmc_spi_homing_check()) break; // Read SGT 4 times per idle loop
  #endif

  // Handle SD Card insert / remove
  TERN_(HAS_MEDIA, card.manage_media());

  // Announce Host Keepalive state (if any)
  TERN_(HOST_KEEPALIVE_FEATURE, gcode.host_keepalive());

  // Update the Print Job Timer state
  TERN_(PRINTCOUNTER, print_job_timer.tick());

  // Update the Beeper queue
  TERN_(HAS_BEEPER, buzzer.tick());

  // Async Babystepping via the Emergency Parser
  #if ALL(EP_BABYSTEPPING, EMERGENCY_PARSER)
    babystep.do_ep_steps();
  #endif

  // Direct Stepping
  TERN_(DIRECT_STEPPING, page_manager.write_responses());

  // Manage Fixed-time Motion Control
  TERN_(FT_MOTION, ftMotion.loop());

  // Handle UI input / draw events
  #if ENABLED(SOVOL_SV06_RTS)
    RTS_Update();
  #else
    ui.update();
  #endif

  // Run i2c Position Encoders
  #if ENABLED(I2C_POSITION_ENCODERS)
  {
    static millis_t i2cpem_next_update_ms;
    if (planner.has_blocks_queued()) {
      const millis_t ms = millis();
      if (ELAPSED(ms, i2cpem_next_update_ms)) {
        I2CPEM.update();
        i2cpem_next_update_ms = ms + I2CPE_MIN_UPD_TIME_MS;
      }
    }
  }
  #endif

  // Auto-report Temperatures / SD Status
  #if HAS_AUTO_REPORTING
    if (!gcode.autoreport_paused) {
      TERN_(AUTO_REPORT_TEMPERATURES, thermalManager.auto_reporter.tick());
      TERN_(AUTO_REPORT_FANS, fan_check.auto_reporter.tick());
      TERN_(AUTO_REPORT_SD_STATUS, card.auto_reporter.tick());
      TERN_(AUTO_REPORT_POSITION, motion.position_auto_reporter.tick());
      TERN_(BUFFER_MONITORING, queue.auto_report_buffer_statistics());
    }
  #endif

  // Update the Průša MMU2
  #if HAS_PRUSA_MMU3
    mmu3.mmu_loop();
  #elif HAS_PRUSA_MMU2
    mmu2.mmu_loop();
  #endif

  // Handle Joystick jogging
  TERN_(POLL_JOG, joystick.inject_jog_moves());

  // Update the LVGL interface
  TERN_(HAS_TFT_LVGL_UI, LV_TASK_HANDLER());

  IDLE_DONE:
  TERN_(MARLIN_DEV_MODE, idle_depth--);

  return;

} // Marlin::idle()

/**
 * Kill all activity and lock the machine.
 * After this the machine will need to be reset.
 */
void Marlin::kill(FSTR_P const lcd_error/*=nullptr*/, FSTR_P const lcd_component/*=nullptr*/, const bool steppers_off/*=false*/) {
  thermalManager.disable_all_heaters();

  TERN_(HAS_CUTTER, cutter.kill()); // Full cutter shutdown including ISR control

  // Echo the LCD message to serial for extra context
  if (lcd_error) { SERIAL_ECHO_START(); SERIAL_ECHOLN(lcd_error); }

  #if HAS_DISPLAY
    ui.kill_screen(lcd_error ?: GET_TEXT_F(MSG_KILLED), lcd_component ?: FPSTR(NUL_STR));
  #else
    UNUSED(lcd_error); UNUSED(lcd_component);
  #endif

  TERN_(HAS_TFT_LVGL_UI, lv_draw_error_message(lcd_error));

  // "Error:Printer halted. kill() called!"
  SERIAL_ERROR_MSG(STR_ERR_KILLED);

  #ifdef ACTION_ON_KILL
    hostui.kill();
  #endif

  minkill(steppers_off);
}

void Marlin::minkill(const bool steppers_off/*=false*/) {

  // Wait a short time (allows messages to get out before shutting down.
  for (int i = 1000; i--;) DELAY_US(600);

  cli(); // Stop interrupts

  // Wait to ensure all interrupts stopped
  for (int i = 1000; i--;) DELAY_US(250);

  // Reiterate heaters off
  thermalManager.disable_all_heaters();

  TERN_(HAS_CUTTER, cutter.kill());  // Reiterate cutter shutdown

  // Power off all steppers (for M112) or just the E steppers
  steppers_off ? stepper.disable_all_steppers() : stepper.disable_e_steppers();

  TERN_(PSU_CONTROL, powerManager.power_off());

  TERN_(HAS_SUICIDE, suicide());

  #if ANY(HAS_KILL, SOFT_RESET_ON_KILL)

    // Wait for both KILL and ENC to be released
    while (TERN0(HAS_KILL, kill_state()) || TERN0(SOFT_RESET_ON_KILL, ui.button_pressed()))
      hal.watchdog_refresh();

    // Wait for either KILL or ENC to be pressed again
    while (TERN1(HAS_KILL, !kill_state()) && TERN1(SOFT_RESET_ON_KILL, !ui.button_pressed()))
      hal.watchdog_refresh();

    // Reboot the board
    hal.reboot();

  #else

    for (;;) hal.watchdog_refresh();  // Wait for RESET button or power-cycle

  #endif

} // Marlin::minkill

/**
 * Turn off heaters and stop the print in progress
 * After a stop the machine may be resumed with M999
 */
void Marlin::stop() {
  thermalManager.disable_all_heaters(); // 'unpause' taken care of in here

  print_job_timer.stop();

  #if ANY(PROBING_FANS_OFF, ADVANCED_PAUSE_FANS_PAUSE)
    thermalManager.set_fans_paused(false); // Un-pause fans for safety
  #endif

  if (!isStopped()) {
    SERIAL_ERROR_MSG(STR_ERR_STOPPED);
    LCD_MESSAGE(MSG_STOPPED);
    safe_delay(350);         // Allow enough time for messages to get out before stopping
    setState(MF_STOPPED);
  }
} // Marlin::stop()
