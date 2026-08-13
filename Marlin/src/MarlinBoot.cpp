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
 * MarlinBoot.cpp - bringing the board up, and the loop that never returns.
 *
 * `setup()` and `loop()` are the two entry points the platform calls, and they are the two
 * functions in this firmware that a test can never execute. `setup()` brings up real hardware
 * in a fixed order and ends by declaring the machine running; `loop()` does not return. A test
 * build stands in for both — `SimulatedHardware::ensure_ready()` initialises the timers and
 * the stepper, and every test drives `marlin.idle()` and `queue.advance()` itself, which is
 * what `loop()` would have done.
 *
 * They live in their own translation unit for two reasons, and neither is tidiness:
 *
 *   - **Coverage.** Together they were a third of `MarlinCore.cpp` and none of it was
 *     reachable, so the file reported 36% and there was no way to tell the untestable part
 *     from the untested part. `Makefile`'s gcovr invocation excludes this file, so the figure
 *     for `MarlinCore.cpp` now means "of the code a test could run".
 *   - **Pressure.** Anything that ends up in here is, by construction, code nobody can assert
 *     on. Making that boundary a file rather than a comment means adding to it is a visible
 *     decision. The rule for this file is that it should only ever get *smaller*: work that
 *     can be named and called belongs in `MarlinCore.cpp` where it can be tested, and this
 *     file should be the calls and the ordering.
 *
 * See `docs/defect-register.md` and `CLAUDE.md` for what was extracted and what is left.
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

inline void tmc_standby_setup() {
  #if PIN_EXISTS(X_STDBY)
    SET_INPUT_PULLDOWN(X_STDBY_PIN);
  #endif
  #if PIN_EXISTS(X2_STDBY)
    SET_INPUT_PULLDOWN(X2_STDBY_PIN);
  #endif
  #if PIN_EXISTS(Y_STDBY)
    SET_INPUT_PULLDOWN(Y_STDBY_PIN);
  #endif
  #if PIN_EXISTS(Y2_STDBY)
    SET_INPUT_PULLDOWN(Y2_STDBY_PIN);
  #endif
  #if PIN_EXISTS(Z_STDBY)
    SET_INPUT_PULLDOWN(Z_STDBY_PIN);
  #endif
  #if PIN_EXISTS(Z2_STDBY)
    SET_INPUT_PULLDOWN(Z2_STDBY_PIN);
  #endif
  #if PIN_EXISTS(Z3_STDBY)
    SET_INPUT_PULLDOWN(Z3_STDBY_PIN);
  #endif
  #if PIN_EXISTS(Z4_STDBY)
    SET_INPUT_PULLDOWN(Z4_STDBY_PIN);
  #endif
  #if PIN_EXISTS(I_STDBY)
    SET_INPUT_PULLDOWN(I_STDBY_PIN);
  #endif
  #if PIN_EXISTS(J_STDBY)
    SET_INPUT_PULLDOWN(J_STDBY_PIN);
  #endif
  #if PIN_EXISTS(K_STDBY)
    SET_INPUT_PULLDOWN(K_STDBY_PIN);
  #endif
  #if PIN_EXISTS(U_STDBY)
    SET_INPUT_PULLDOWN(U_STDBY_PIN);
  #endif
  #if PIN_EXISTS(V_STDBY)
    SET_INPUT_PULLDOWN(V_STDBY_PIN);
  #endif
  #if PIN_EXISTS(W_STDBY)
    SET_INPUT_PULLDOWN(W_STDBY_PIN);
  #endif
  #if PIN_EXISTS(E0_STDBY)
    SET_INPUT_PULLDOWN(E0_STDBY_PIN);
  #endif
  #if PIN_EXISTS(E1_STDBY)
    SET_INPUT_PULLDOWN(E1_STDBY_PIN);
  #endif
  #if PIN_EXISTS(E2_STDBY)
    SET_INPUT_PULLDOWN(E2_STDBY_PIN);
  #endif
  #if PIN_EXISTS(E3_STDBY)
    SET_INPUT_PULLDOWN(E3_STDBY_PIN);
  #endif
  #if PIN_EXISTS(E4_STDBY)
    SET_INPUT_PULLDOWN(E4_STDBY_PIN);
  #endif
  #if PIN_EXISTS(E5_STDBY)
    SET_INPUT_PULLDOWN(E5_STDBY_PIN);
  #endif
  #if PIN_EXISTS(E6_STDBY)
    SET_INPUT_PULLDOWN(E6_STDBY_PIN);
  #endif
  #if PIN_EXISTS(E7_STDBY)
    SET_INPUT_PULLDOWN(E7_STDBY_PIN);
  #endif
} // tmc_standby_setup()

/**
 * Marlin Firmware entry-point. Abandon Hope All Ye Who Enter Here.
 * Setup before the program loop:
 *
 *  - Call any special pre-init set for the board
 *  - Put TMC drivers into Low Power Standby mode
 *  - Init the serial ports (so setup can be debugged)
 *  - Set up the kill and suicide pins
 *  - Prepare (disable) board JTAG and Debug ports
 *  - Init serial for a connected MKS TFT with WiFi
 *  - Install Marlin custom Exception Handlers, if set.
 *  - Init Marlin's HAL interfaces (for SPI, i2c, etc.)
 *  - Init some optional hardware and features:
 *    • MAX Thermocouple pins
 *    • Duet Smart Effector
 *    • Filament Runout Sensor
 *    • TMC220x Stepper Drivers (Serial)
 *    • PSU control
 *    • Power-loss Recovery
 *    • Stepper Driver Reset: DISABLE
 *    • TMC Stepper Drivers (SPI)
 *    • Run hal.init_board() for additional pins setup
 *    • ESP WiFi
 *  - Get the Reset Reason and report it
 *  - Print startup messages and diagnostics
 *  - Calibrate the HAL DELAY for precise timing
 *  - Init the buzzer, possibly a custom timer
 *  - Init more optional hardware:
 *    • Color LED illumination
 *    • NeoPixel illumination
 *    • Controller Fan
 *    • Creality DWIN LCD (show boot image)
 *    • Tare the Probe if possible
 *  - Mount the (most likely external) SD Card
 *  - Load settings from EEPROM (or use defaults)
 *  - Init the Ethernet Port
 *  - Init Touch Buttons (for emulated DOGLCD)
 *  - Adjust the (certainly wrong) current position by the home offset
 *  - Init the Planner::position (steps) based on current (native) position
 *  - Initialize more managers and peripherals:
 *    • Temperatures
 *    • Print Job Timer
 *    • Endstops and Endstop Interrupts
 *    • Stepper ISR - Kind of Important!
 *    • Servos
 *    • Servo-based Probe
 *    • Photograph Pin
 *    • Laser/Spindle tool Power / PWM
 *    • Coolant Control
 *    • Bed Probe
 *    • Stepper Driver Reset: ENABLE
 *    • Digipot I2C - Stepper driver current control
 *    • Stepper DAC - Stepper driver current control
 *    • Solenoid (probe, or for other use)
 *    • Home Pin
 *    • Custom User Buttons
 *    • Red/Blue Status LEDs
 *    • Case Light
 *    • Prusa MMU filament changer
 *    • Fan Multiplexer
 *    • Mixing Extruder
 *    • BLTouch Probe
 *    • I2C Position Encoders
 *    • Custom I2C Bus handlers
 *    • Enhanced tools or extruders:
 *      • Switching Extruder
 *      • Switching Nozzle
 *      • Parking Extruder
 *      • Magnetic Parking Extruder
 *      • Switching Toolhead
 *      • Electromagnetic Switching Toolhead
 *    • Watchdog Timer - Also Kind of Important!
 *    • Closed Loop Controller
 *  - Run Startup Commands, if defined
 *  - Tell host to close Host Prompts
 *  - Test Trinamic driver connections
 *  - Init Prusa MMU2 filament changer
 *  - Init and test BL24Cxx EEPROM
 *  - Init Creality DWIN encoder, show faux progress bar
 *  - Reset Status Message / Show Service Messages
 *  - Init MAX7219 LED Matrix
 *  - Init Direct Stepping (Klipper-style motion control)
 *  - Init TFT LVGL UI (with 3D Graphics)
 *  - Apply Password Lock - Hold for Authentication
 *  - Open Touch Screen Calibration screen, if not calibrated
 *  - Set Marlin to RUNNING State
 *  - Stop print timer
 */
void setup() {
  #ifdef FASTIO_INIT
    FASTIO_INIT();
  #endif

  #ifdef BOARD_PREINIT
    BOARD_PREINIT(); // Low-level init (before serial init)
  #endif

  tmc_standby_setup();  // TMC Low Power Standby pins must be set early or they're not usable

  // Check startup - does nothing if bootloader sets MCUSR to 0
  const byte mcu = hal.get_reset_source();
  hal.clear_reset_source();

  #if ENABLED(MARLIN_DEV_MODE)
    auto log_current_ms = [&](PGM_P const msg) {
      SERIAL_ECHO_START();
      TSS('[', millis(), F("] ")).echo();
      SERIAL_ECHOLNPGM_P(msg);
    };
    #define SETUP_LOG(M) log_current_ms(PSTR(M))
  #else
    #define SETUP_LOG(...) NOOP
  #endif
  #define SETUP_RUN(C) do{ SETUP_LOG(STRINGIFY(C)); C; }while(0)

  MYSERIAL1.begin(BAUDRATE);
  millis_t serial_connect_timeout = millis() + 1000UL;
  while (!MYSERIAL1.connected() && PENDING(millis(), serial_connect_timeout)) { /*nada*/ }

  #if ENABLED(SOVOL_SV06_RTS)
    LCD_SERIAL.begin(BAUDRATE);
    serial_connect_timeout = millis() + 1000UL;
    while (!LCD_SERIAL.connected() && PENDING(millis(), serial_connect_timeout)) { /*nada*/ }
  #endif

  #if HAS_MULTI_SERIAL && !HAS_ETHERNET
    #ifndef BAUDRATE_2
      #define BAUDRATE_2 BAUDRATE
    #endif
    MYSERIAL2.begin(BAUDRATE_2);
    serial_connect_timeout = millis() + 1000UL;
    while (!MYSERIAL2.connected() && PENDING(millis(), serial_connect_timeout)) { /*nada*/ }
    #ifdef SERIAL_PORT_3
      #ifndef BAUDRATE_3
        #define BAUDRATE_3 BAUDRATE
      #endif
      MYSERIAL3.begin(BAUDRATE_3);
      serial_connect_timeout = millis() + 1000UL;
      while (!MYSERIAL3.connected() && PENDING(millis(), serial_connect_timeout)) { /*nada*/ }
    #endif
  #endif
  SERIAL_ECHOLNPGM("start");

  // Set up these pins early to prevent suicide
  #if HAS_KILL
    SETUP_LOG("KILL_PIN");
    #if KILL_PIN_STATE
      SET_INPUT_PULLDOWN(KILL_PIN);
    #else
      SET_INPUT_PULLUP(KILL_PIN);
    #endif
  #endif

  #if ENABLED(FREEZE_FEATURE) && DISABLED(NO_FREEZE_PIN)
    SETUP_LOG("FREEZE_PIN");
    #if FREEZE_STATE
      SET_INPUT_PULLDOWN(FREEZE_PIN);
    #else
      SET_INPUT_PULLUP(FREEZE_PIN);
    #endif
  #endif

  #if HAS_SUICIDE
    SETUP_LOG("SUICIDE_PIN");
    OUT_WRITE(SUICIDE_PIN, !SUICIDE_PIN_STATE);
  #endif

  #ifdef JTAGSWD_RESET
    SETUP_LOG("JTAGSWD_RESET");
    JTAGSWD_RESET();
  #endif

  // Disable any hardware debug to free up pins for IO
  #if ENABLED(DISABLE_DEBUG) && defined(JTAGSWD_DISABLE)
    delay(10);
    SETUP_LOG("JTAGSWD_DISABLE");
    JTAGSWD_DISABLE();
  #elif ENABLED(DISABLE_JTAG) && defined(JTAG_DISABLE)
    delay(10);
    SETUP_LOG("JTAG_DISABLE");
    JTAG_DISABLE();
  #endif

  TERN_(DYNAMIC_VECTORTABLE, hook_cpu_exceptions()); // If supported, install Marlin exception handlers at runtime

  SETUP_RUN(hal.init());

  // Init and disable SPI thermocouples; this is still needed
  #if TEMP_SENSOR_IS_MAX_TC(0) || (TEMP_SENSOR_IS_MAX_TC(REDUNDANT) && REDUNDANT_TEMP_MATCH(SOURCE, E0))
    OUT_WRITE(TEMP_0_CS_PIN, HIGH);  // Disable
  #endif
  #if TEMP_SENSOR_IS_MAX_TC(1) || (TEMP_SENSOR_IS_MAX_TC(REDUNDANT) && REDUNDANT_TEMP_MATCH(SOURCE, E1))
    OUT_WRITE(TEMP_1_CS_PIN, HIGH);
  #endif
  #if TEMP_SENSOR_IS_MAX_TC(2) || (TEMP_SENSOR_IS_MAX_TC(REDUNDANT) && REDUNDANT_TEMP_MATCH(SOURCE, E2))
    OUT_WRITE(TEMP_2_CS_PIN, HIGH);
  #endif
  #if TEMP_SENSOR_IS_MAX_TC(BED)
    OUT_WRITE(TEMP_BED_CS_PIN, HIGH);
  #endif

  #if ENABLED(DUET_SMART_EFFECTOR) && PIN_EXISTS(SMART_EFFECTOR_MOD)
    OUT_WRITE(SMART_EFFECTOR_MOD_PIN, LOW);   // Put Smart Effector into NORMAL mode
  #endif

  #if HAS_FILAMENT_SENSOR
    SETUP_RUN(runout.setup());
  #endif

  #if HAS_TMC_UART
    SETUP_RUN(tmc_serial_begin());
  #endif

  #if HAS_TMC_SPI
    #if DISABLED(TMC_USE_SW_SPI)
      SETUP_RUN(SPI.begin());
    #endif
    SETUP_RUN(tmc_init_cs_pins());
  #endif

  #if ENABLED(PSU_CONTROL)
    SETUP_LOG("PSU_CONTROL");
    powerManager.init();
  #endif

  #if ENABLED(POWER_LOSS_RECOVERY)
    SETUP_RUN(recovery.setup());
  #endif

  #if HAS_STEPPER_RESET
    SETUP_RUN(disableStepperDrivers());
  #endif

  SETUP_RUN(hal.init_board());

  #if ENABLED(WIFISUPPORT)
    SETUP_RUN(esp_wifi_init());
  #endif

  marlin.report_reset_reason(mcu);
  marlin.report_firmware_identity();

  // Some HAL need precise delay adjustment
  calibrate_delay_loop();

  // Init buzzer pin(s)
  #if HAS_BEEPER
    SETUP_RUN(buzzer.init());
  #endif

  // Set up LEDs early
  #if HAS_COLOR_LEDS
    SETUP_RUN(leds.setup());
  #endif

  #if ENABLED(NEOPIXEL2_SEPARATE)
    SETUP_RUN(leds2.setup());
  #endif

  #if ENABLED(USE_CONTROLLER_FAN)     // Set up fan controller to initialize also the default configurations.
    SETUP_RUN(controllerFan.setup());
  #endif

  TERN_(HAS_FANCHECK, fan_check.init());

  // UI must be initialized before EEPROM
  // (because EEPROM code calls the UI).
  #if ENABLED(SOVOL_SV06_RTS)
    SETUP_RUN(RTS_Update());
  #else
    SETUP_RUN(ui.init());
  #endif

  #if PIN_EXISTS(SAFE_POWER)
    #if HAS_DRIVER_SAFE_POWER_PROTECT
      SETUP_RUN(stepper_driver_backward_check());
    #else
      SETUP_LOG("SAFE_POWER");
      OUT_WRITE(SAFE_POWER_PIN, HIGH);
    #endif
  #endif

  #if HAS_MEDIA
    SETUP_RUN(card.init());           // Prepare for media usage
    #if ANY(SDCARD_EEPROM_EMULATION, POWER_LOSS_RECOVERY)
      SETUP_RUN(card.mount());        // Mount media with settings before first_load
    #endif
  #endif

  //#if ENABLED(PRINTJOB_TIMER_AUTOSTART)
  //  // Stop timer and set welcome message
  //  if (TERN1(POWER_LOSS_RECOVERY, !recovery.check()))
  //    thermalManager.auto_job_check_timer(false, true);
  //#endif

  // Prepare some LCDs to display early
  #if HAS_EARLY_LCD_SETTINGS
    SETUP_RUN(settings.load_lcd_state());
  #endif

  #if ALL(HAS_WIRED_LCD, SHOW_BOOTSCREEN)
    SETUP_RUN(ui.show_bootscreen());
    const millis_t bootscreen_ms = millis();
  #endif

  SETUP_RUN(settings.first_load());   // Load data from EEPROM if available (or use defaults)
                                      // This also updates variables in the planner, elsewhere

  #if ENABLED(CONFIGURABLE_MACHINE_NAME)
    SETUP_RUN(ui.reset_status(false)); // machine_name Initialized by settings.load()
  #endif

  #if ENABLED(PROBE_TARE)
    SETUP_RUN(probe.tare_init());
  #endif

  #if HAS_ETHERNET
    SETUP_RUN(ethernet.init());
  #endif

  #if HAS_TOUCH_BUTTONS
    SETUP_RUN(touchBt.init());
  #endif

  TERN_(HAS_HOME_OFFSET, motion.position += motion.home_offset); // Init current position based on home_offset

  motion.sync_plan_position();        // Vital to init stepper/planner equivalent for motion.position

  SETUP_RUN(thermalManager.init());   // Initialize temperature loop

  SETUP_RUN(print_job_timer.init());  // Initial setup of print job timer

  SETUP_RUN(endstops.init());         // Init endstops and pullups

  #if ENABLED(DELTA) && !HAS_SOFTWARE_ENDSTOPS
    SETUP_RUN(refresh_delta_clip_start_height()); // Init safe delta height without soft endstops
  #endif

  SETUP_RUN(stepper.init());          // Init stepper. This enables interrupts!

  #if HAS_SERVOS
    SETUP_RUN(servo_init());
  #endif

  #if HAS_Z_SERVO_PROBE
    SETUP_RUN(probe.servo_probe_init());
  #endif

  #if HAS_PHOTOGRAPH
    OUT_WRITE(PHOTOGRAPH_PIN, LOW);
  #endif

  #if HAS_CUTTER
    SETUP_RUN(cutter.init());
  #endif

  #if ENABLED(COOLANT_MIST)
    OUT_WRITE(COOLANT_MIST_PIN, COOLANT_MIST_INVERT);   // Init Mist Coolant OFF
  #endif
  #if ENABLED(COOLANT_FLOOD)
    OUT_WRITE(COOLANT_FLOOD_PIN, COOLANT_FLOOD_INVERT); // Init Flood Coolant OFF
  #endif

  #if HAS_BED_PROBE
    #if PIN_EXISTS(PROBE_ENABLE)
      OUT_WRITE(PROBE_ENABLE_PIN, LOW); // Disable
    #endif
    SETUP_RUN(endstops.enable_z_probe(false));
  #endif

  #if HAS_STEPPER_RESET
    SETUP_RUN(enableStepperDrivers());
  #endif

  #if HAS_MOTOR_CURRENT_I2C
    SETUP_RUN(digipot_i2c.init());
  #endif

  #if HAS_MOTOR_CURRENT_DAC
    SETUP_RUN(stepper_dac.init());
  #endif

  #if ANY(Z_PROBE_SLED, SOLENOID_PROBE) && HAS_SOLENOID_1
    OUT_WRITE(SOL1_PIN, LOW); // OFF
  #endif

  #if HAS_HOME
    SET_INPUT_PULLUP(HOME_PIN);
  #endif

  #if ENABLED(CUSTOM_USER_BUTTONS)
    #define INIT_CUSTOM_USER_BUTTON_PIN(N) do{ SET_INPUT(BUTTON##N##_PIN); WRITE(BUTTON##N##_PIN, !BUTTON##N##_HIT_STATE); }while(0)

    #if HAS_CUSTOM_USER_BUTTON(1)
      INIT_CUSTOM_USER_BUTTON_PIN(1);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(2)
      INIT_CUSTOM_USER_BUTTON_PIN(2);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(3)
      INIT_CUSTOM_USER_BUTTON_PIN(3);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(4)
      INIT_CUSTOM_USER_BUTTON_PIN(4);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(5)
      INIT_CUSTOM_USER_BUTTON_PIN(5);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(6)
      INIT_CUSTOM_USER_BUTTON_PIN(6);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(7)
      INIT_CUSTOM_USER_BUTTON_PIN(7);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(8)
      INIT_CUSTOM_USER_BUTTON_PIN(8);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(9)
      INIT_CUSTOM_USER_BUTTON_PIN(9);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(10)
      INIT_CUSTOM_USER_BUTTON_PIN(10);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(11)
      INIT_CUSTOM_USER_BUTTON_PIN(11);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(12)
      INIT_CUSTOM_USER_BUTTON_PIN(12);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(13)
      INIT_CUSTOM_USER_BUTTON_PIN(13);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(14)
      INIT_CUSTOM_USER_BUTTON_PIN(14);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(15)
      INIT_CUSTOM_USER_BUTTON_PIN(15);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(16)
      INIT_CUSTOM_USER_BUTTON_PIN(16);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(17)
      INIT_CUSTOM_USER_BUTTON_PIN(17);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(18)
      INIT_CUSTOM_USER_BUTTON_PIN(18);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(19)
      INIT_CUSTOM_USER_BUTTON_PIN(19);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(20)
      INIT_CUSTOM_USER_BUTTON_PIN(20);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(21)
      INIT_CUSTOM_USER_BUTTON_PIN(21);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(22)
      INIT_CUSTOM_USER_BUTTON_PIN(22);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(23)
      INIT_CUSTOM_USER_BUTTON_PIN(23);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(24)
      INIT_CUSTOM_USER_BUTTON_PIN(24);
    #endif
    #if HAS_CUSTOM_USER_BUTTON(25)
      INIT_CUSTOM_USER_BUTTON_PIN(25);
    #endif
  #endif

  #if PIN_EXISTS(STAT_LED_RED)
    OUT_WRITE(STAT_LED_RED_PIN, LOW); // OFF
  #endif
  #if PIN_EXISTS(STAT_LED_BLUE)
    OUT_WRITE(STAT_LED_BLUE_PIN, LOW); // OFF
  #endif

  #if ENABLED(CASE_LIGHT_ENABLE)
    SETUP_RUN(caselight.init());
  #endif

  #if HAS_PRUSA_MMU1
    SETUP_RUN(mmu_init());
  #endif

  #if HAS_FANMUX
    SETUP_RUN(fanmux_init());
  #endif

  #if ENABLED(MIXING_EXTRUDER)
    SETUP_RUN(mixer.init());
  #endif

  #if ENABLED(BLTOUCH)
    SETUP_RUN(bltouch.init(/*set_voltage=*/true));
  #endif

  #if ENABLED(MAGLEV4)
    OUT_WRITE(MAGLEV_TRIGGER_PIN, LOW);
  #endif

  #if ENABLED(I2C_POSITION_ENCODERS)
    SETUP_RUN(I2CPEM.init());
  #endif

  #if ENABLED(EXPERIMENTAL_I2CBUS) && I2C_SLAVE_ADDRESS > 0
    SETUP_LOG("i2c...");
    i2c.onReceive(i2c_on_receive);
    i2c.onRequest(i2c_on_request);
  #endif

  #if DO_SWITCH_EXTRUDER
    SETUP_RUN(move_extruder_servo(0));  // Initialize extruder servo
  #endif

  #if ENABLED(SWITCHING_NOZZLE)
    SETUP_LOG("SWITCHING_NOZZLE");
    // Initialize nozzle servo(s)
    #if SWITCHING_NOZZLE_TWO_SERVOS
      lower_nozzle(0);
      raise_nozzle(1);
    #else
      move_nozzle_servo(0);
    #endif
  #endif

  #if ENABLED(PARKING_EXTRUDER)
    SETUP_RUN(pe_solenoid_init());
  #elif ENABLED(MAGNETIC_PARKING_EXTRUDER)
    SETUP_RUN(mpe_settings_init());
  #elif ENABLED(SWITCHING_TOOLHEAD)
    SETUP_RUN(swt_init());
  #elif ENABLED(ELECTROMAGNETIC_SWITCHING_TOOLHEAD)
    SETUP_RUN(est_init());
  #endif

  #if ENABLED(USE_WATCHDOG)
    SETUP_RUN(hal.watchdog_init());   // Reinit watchdog after hal.get_reset_source call
  #endif

  #if ENABLED(EXTERNAL_CLOSED_LOOP_CONTROLLER)
    SETUP_RUN(closedloop.init());
  #endif

  #ifdef STARTUP_COMMANDS
    SETUP_LOG("STARTUP_COMMANDS");
    queue.inject(F(STARTUP_COMMANDS));
  #endif

  #if ENABLED(HOST_PROMPT_SUPPORT)
    SETUP_RUN(hostui.prompt_end());
  #endif

  #if HAS_DRIVER_SAFE_POWER_PROTECT
    SETUP_RUN(stepper_driver_backward_report());
  #endif

  #if HAS_PRUSA_MMU3
    if (mmu3.mmu_hw_enabled) SETUP_RUN(mmu3.start());
    SETUP_RUN(mmu3.status());
    SETUP_RUN(spooljoin.initStatus());
  #elif HAS_PRUSA_MMU2
    SETUP_RUN(mmu2.init());
  #endif

  #if ENABLED(IIC_BL24CXX_EEPROM)
    BL24CXX::init();
    const uint8_t err = BL24CXX::check();
    SERIAL_ECHO_TERNARY(err, "BL24CXX Check ", "failed", "succeeded", "!\n");
  #endif

  #if ENABLED(DWIN_CREALITY_LCD)
    SETUP_RUN(dwinInitScreen());
  #elif ENABLED(SOVOL_SV06_RTS)
    SETUP_RUN(rts.init());
  #endif

  #if HAS_SERVICE_INTERVALS && DISABLED(DWIN_CREALITY_LCD)
    SETUP_RUN(ui.reset_status(true));  // Show service messages or keep current status
  #endif

  #if ENABLED(MAX7219_DEBUG)
    SETUP_RUN(max7219.init());
  #endif

  #if ENABLED(DIRECT_STEPPING)
    SETUP_RUN(page_manager.init());
  #endif

  #if HAS_TFT_LVGL_UI
    #if HAS_MEDIA
      if (!card.isMounted()) SETUP_RUN(card.mount()); // Mount SD to load graphics and fonts
    #endif
    SETUP_RUN(tft_lvgl_init());
  #endif

  #if ALL(HAS_WIRED_LCD, SHOW_BOOTSCREEN)
    const millis_t elapsed = millis() - bootscreen_ms;
    #if ENABLED(MARLIN_DEV_MODE)
      SERIAL_ECHOLNPGM("elapsed=", elapsed);
    #endif
    SETUP_RUN(ui.bootscreen_completion(elapsed));
  #endif

  #if ENABLED(PASSWORD_ON_STARTUP)
    SETUP_RUN(password.lock_machine());      // Will not proceed until correct password provided
  #endif

  #if ALL(HAS_MARLINUI_MENU, TOUCH_SCREEN_CALIBRATION) && ANY(TFT_CLASSIC_UI, TFT_COLOR_UI)
    SETUP_RUN(ui.check_touch_calibration());
  #endif

  #if ENABLED(EASYTHREED_UI)
    SETUP_RUN(easythreed_ui.init());
  #endif

  #if HAS_TRINAMIC_CONFIG && DISABLED(PSU_DEFAULT_OFF)
    SETUP_RUN(test_tmc_connection());
  #endif

  #if ENABLED(BD_SENSOR)
    SETUP_RUN(bdl.init(I2C_BD_SDA_PIN, I2C_BD_SCL_PIN, I2C_BD_DELAY));
  #endif

  #if HAS_RS485_SERIAL
    SETUP_RUN(rs485_init());
  #endif

  #if ENABLED(FT_MOTION)
    SETUP_RUN(ftMotion.init());
  #endif

  marlin.setState(MF_RUNNING);

  #ifdef STARTUP_TUNE
    // Play a short startup tune before continuing.
    constexpr uint16_t tune[] = STARTUP_TUNE;
    for (uint8_t i = 0; i < COUNT(tune) - 1; i += 2) BUZZ(tune[i + 1], tune[i]);
  #endif

  SETUP_LOG("setup() completed.");

  TERN_(MARLIN_TEST_BUILD, runStartupTests());
} // setup()

/**
 * The main Marlin program loop
 *
 *  - Call marlin.idle() to handle all tasks between G-code commands
 *      Note that no G-codes from the queue can be executed during idle()
 *      but many G-codes can be called directly anytime like macros.
 *  - Check whether SD card auto-start is needed now.
 *  - Check whether SD print finishing is needed now.
 *  - Run one G-code command from the immediate or main command queue
 *    and open up one space. Commands in the main queue may come from sd
 *    card, host, or by direct injection. The queue will continue to fill
 *    as long as idle() or manage_inactivity() are being called.
 */
void loop() {
  do {
    marlin.idle();

    #if HAS_MEDIA
      if (card.flag.abort_sd_printing) abortSDPrinting();
      if (marlin.is(MF_SD_COMPLETE)) finishSDPrinting();
    #endif

    queue.advance();

    #if ANY(POWER_OFF_TIMER, POWER_OFF_WAIT_FOR_COOLDOWN)
      powerManager.checkAutoPowerOff();
    #endif

    endstops.event_handler();

    TERN_(HAS_TFT_LVGL_UI, printer_state_polling());

    TERN_(MARLIN_TEST_BUILD, runPeriodicTests());

  } while (ENABLED(__AVR__)); // Loop forever on slower (AVR) boards
}
