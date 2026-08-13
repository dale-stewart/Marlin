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
#pragma once

/**
 * Substitute temperature sensor readings.
 *
 * Commands that wait for a temperature (M109, M190) loop until the measured value
 * reaches the target, so without a stand-in sensor they never return.
 *
 * This used to write `temp_hotend[0].celsius` directly, which worked only because the
 * ADC pipeline was dormant: nothing started MF_TIMER_TEMP in a test build, so nothing
 * ever overwrote what the test had said. Now that `Temperature::isr()` runs whenever
 * simulated time advances, a value written at that level survives only until the next
 * conversion. So the sensor is driven where the firmware reads it instead — a raw ADC
 * count on the pin `MarlinHAL::adc_value()` samples — and the firmware's own pipeline
 * turns that into degrees.
 *
 * **The contract has one consequence worth knowing.** A thermistor read through a
 * 10-bit ADC cannot express every temperature: near 120 C one count is about 0.15 C,
 * near 200 C about 0.45 C. `hotend_reads(c)` therefore drives the count *nearest* to
 * `c` and returns the temperature actually achieved. A test that cares about an exact
 * value should use the returned reading, or `hotend_would_read(c)` to ask in advance;
 * a test pinning a threshold should use `..._at_least()` / `..._below()`, which pick
 * the nearest representable reading on a chosen side of the boundary.
 *
 * Under the LINUX HAL the pipeline really is still dormant — its timers are POSIX
 * signals that a test build never starts — so there the settled value is placed
 * directly. It is the same value either way, quantisation included, so a test asserts
 * the same thing in both environments.
 *
 * Restores the previous readings on destruction so one test cannot leave another
 * believing the printer is hot.
 */

#include "src/module/temperature.h"
#include "src/MarlinCore.h"
#include "../support/simulated_hardware.h"

/**
 * How long the bed's control loop takes to reconsider.
 *
 * Under bang-bang control the bed is only looked at every `BED_CHECK_INTERVAL`, so a test
 * that wants the bed actually driving has to wait that long. Under `PIDTEMPBED` it is
 * regulated on every pass and the macro **does not exist** — a test naming it does not fail,
 * it fails to compile. Same shape as `STR_Z_LIMIT` in `simulated_endstops.h`: name the
 * property, not the macro that happens to express it on one machine.
 */
#if ENABLED(PIDTEMPBED)
  constexpr uint32_t BED_CONTROL_PERIOD_MS = 0;
#elif defined(BED_CHECK_INTERVAL)
  constexpr uint32_t BED_CONTROL_PERIOD_MS = BED_CHECK_INTERVAL;
#else
  constexpr uint32_t BED_CONTROL_PERIOD_MS = 0;
#endif

class SimulatedSensors {
public:
  SimulatedSensors() {
    SimulatedHardware::ensure_ready();
    TERN_(HAS_HOTEND, was_hotend_code = SimulatedHardware::adc_code(TEMP_0_PIN));
    TERN_(HAS_HEATED_BED, was_bed_code = SimulatedHardware::adc_code(TEMP_BED_PIN));
  }

  ~SimulatedSensors() {
    TERN_(HAS_HOTEND, SimulatedHardware::drive_adc(TEMP_0_PIN, was_hotend_code));
    TERN_(HAS_HEATED_BED, SimulatedHardware::drive_adc(TEMP_BED_PIN, was_bed_code));
    settle();
  }

  #if HAS_HOTEND

    // "The hotend sensor reads this." Returns the reading actually achieved.
    static celsius_float_t hotend_reads(const celsius_float_t c) {
      return drive_hotend(SimulatedHardware::code_nearest(hotend_conv, c));
    }

    // The nearest reading at or above `c`, for pinning the cold side of a threshold.
    static celsius_float_t hotend_reads_at_least(const celsius_float_t c) {
      return drive_hotend(code_on_side(true, c));
    }

    // The nearest reading strictly below `c`, for pinning the other side of it.
    static celsius_float_t hotend_reads_below(const celsius_float_t c) {
      return drive_hotend(code_on_side(false, c));
    }

    // What hotend_reads(c) would produce, without touching anything.
    static celsius_float_t hotend_would_read(const celsius_float_t c) {
      return celsius_of_hotend(SimulatedHardware::code_nearest(hotend_conv, c));
    }

  #endif // HAS_HOTEND

  #if HAS_HEATED_BED

    static celsius_float_t bed_reads(const celsius_float_t c) {
      const uint16_t code = SimulatedHardware::code_nearest(bed_conv, c);
      SimulatedHardware::drive_adc(TEMP_BED_PIN, code);
      settle();
      return thermalManager.degBed();
    }

    // The nearest reading at or above `c`, and the nearest strictly below it — the same
    // pair the hotend has, for pinning a threshold from both sides. A test that wants a
    // *fault* has to use these rather than `bed_reads(limit + 5)`: quantisation near the
    // top of the table is several degrees per count, so "about five over" can land under.
    static celsius_float_t bed_reads_at_least(const celsius_float_t c) {
      return drive_bed(bed_code_on_side(true, c));
    }

    static celsius_float_t bed_reads_below(const celsius_float_t c) {
      return drive_bed(bed_code_on_side(false, c));
    }

    static celsius_float_t bed_would_read(const celsius_float_t c) {
      return thermalManager.analog_to_celsius_bed(
        raw_adc_t(SimulatedHardware::code_nearest(bed_conv, c) * OVERSAMPLENR));
    }

  #endif // HAS_HEATED_BED

  /**
   * Run the machine until the readings the pins are driving have reached the manager.
   *
   * The ISR accumulates OVERSAMPLENR conversions per sensor before a set of raw values
   * is ready, and `Temperature::task()` is what turns raw values into degrees, so a
   * driven pin does not become a reading until both have happened. This waits for that
   * rather than for a fixed number of milliseconds, so it costs no more simulated time
   * than it has to. If the pipeline stops it gives up after SETTLE_LIMIT_MS rather than
   * hanging, and the caller's own assertion on the reading is what reports it.
   */
  static void settle() {
    // task() reports through the serial port on error, and nothing drains it here.
    const bool was_connected = MYSERIAL1.host_connected;
    MYSERIAL1.host_connected = false;
    // task() does nothing at all while Marlin is still starting up.
    const MarlinState was_state = marlin.state;
    if (marlin.is(MF_INITIALIZING)) marlin.setState(MF_RUNNING);

    for (uint16_t i = 0; i < SETTLE_LIMIT_MS; i++) {
      if (settled()) break;
      HAL_test_advance_millis(1);
      thermalManager.task();
    }

    marlin.setState(was_state);
    MYSERIAL1.host_connected = was_connected;
  }

private:

  static constexpr uint16_t SETTLE_LIMIT_MS = 500;

  #if HAS_HOTEND
    static celsius_float_t hotend_conv(const raw_adc_t r) { return thermalManager.analog_to_celsius_hotend(r, 0); }
    static celsius_float_t celsius_of_hotend(const uint16_t code) { return hotend_conv(raw_adc_t(code * OVERSAMPLENR)); }

    static celsius_float_t drive_hotend(const uint16_t code) {
      SimulatedHardware::drive_adc(TEMP_0_PIN, code);
      settle();
      return thermalManager.degHotend(0);
    }

    // The representable reading closest to `c` on one side of it.
    static uint16_t code_on_side(const bool at_or_above, const celsius_float_t c) {
      uint16_t best = 0;
      float best_err = 1e30f;
      for (uint16_t code = 0; code < 1024; code++) {
        const float v = float(celsius_of_hotend(code));
        if (at_or_above ? (v < float(c)) : (v >= float(c))) continue;
        const float err = fabsf(v - float(c));
        if (err < best_err) { best_err = err; best = code; }
      }
      return best;
    }
  #endif

  #if HAS_HEATED_BED
    static celsius_float_t bed_conv(const raw_adc_t r) { return thermalManager.analog_to_celsius_bed(r); }
    static celsius_float_t celsius_of_bed(const uint16_t code) { return bed_conv(raw_adc_t(code * OVERSAMPLENR)); }

    static celsius_float_t drive_bed(const uint16_t code) {
      SimulatedHardware::drive_adc(TEMP_BED_PIN, code);
      settle();
      return thermalManager.degBed();
    }

    static uint16_t bed_code_on_side(const bool at_or_above, const celsius_float_t c) {
      uint16_t best = 0;
      float best_err = 1e30f;
      for (uint16_t code = 0; code < 1024; code++) {
        const float v = float(celsius_of_bed(code));
        if (at_or_above ? (v < float(c)) : (v >= float(c))) continue;
        const float err = fabsf(v - float(c));
        if (err < best_err) { best_err = err; best = code; }
      }
      return best;
    }
  #endif

  // Has every driven pin become the reading the manager reports?
  static bool settled() {
    bool ok = true;
    #if HAS_HOTEND
      ok = ok && thermalManager.degHotend(0) == celsius_of_hotend(SimulatedHardware::adc_code(TEMP_0_PIN));
    #endif
    #if HAS_HEATED_BED
      ok = ok && thermalManager.degBed() == celsius_of_bed(SimulatedHardware::adc_code(TEMP_BED_PIN));
    #endif
    return ok;
  }

  uint16_t was_hotend_code = 0, was_bed_code = 0;
};
