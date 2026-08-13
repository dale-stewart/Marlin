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
 * A heater that gets hot when the firmware switches it on.
 *
 * `SimulatedSensors` says what a sensor reads and it stays said, which is what a test
 * about a threshold wants. It is the wrong thing for a test about *waiting*: M109 is
 * only interesting if the temperature it is waiting for arrives because the firmware
 * heated it, not because the test asserted it.
 *
 * Closing that loop needs somewhere to run between the firmware's PWM decision and the
 * next ADC conversion, and the HAL already has one. `Gpio` lets a `Peripheral` be
 * attached to a pin, and calls it on every write. `Temperature::isr()` writes the heater
 * pin every pass to modulate soft PWM, so a peripheral on that pin is called at the ISR
 * rate, with no polling and no new production seam. Each call integrates the interval
 * that just passed and drives the thermistor input to match — so the reading the
 * firmware takes next is a consequence of the power it applied last.
 *
 * The model is deliberately the simplest thing that behaves like a heater rather than
 * like a number: first order, with a time constant and a temperature it would reach if
 * left at full power. That is enough for the PID loop to be a real loop and for
 * residency to mean something; it is not a thermal simulation and should not be read as
 * one. Its parameters are chosen so a heat-up finishes in a few seconds of simulated
 * time, because a test pays for simulated time in real iterations.
 *
 * Only under the test HAL: under LINUX nothing advances the clock or runs the ISR, so
 * there is nothing for the model to integrate.
 */

#include "simulated_hardware.h"


#include "src/module/temperature.h"

/**
 * The thermistor curve, cached and searchable.
 *
 * The model has to answer "which ADC count reads as this temperature?" on every ISR,
 * and the conversion only runs the other way. Scanning all 1024 counts each time is
 * affordable once but not thousands of times a second, so the curve is built once and
 * then walked from where it was last, which is a step or two while a heater warms up.
 */
class AdcCurve {
public:
  template <typename Conv>
  void build(Conv conv) {
    for (uint16_t code = 0; code < 1024; code++) celsius[code] = float(conv(raw_adc_t(code * OVERSAMPLENR)));
    built = true;
  }

  bool is_built() const { return built; }
  float at(const uint16_t code) const { return celsius[code]; }

  // The count reading nearest `t`, starting from `hint`. The curve falls as the count
  // rises, so "too hot" means walking up and "too cold" means walking down.
  uint16_t nearest_from(const uint16_t hint, const float t) const {
    uint16_t code = hint;
    while (code < 1023 && celsius[code] > t) code++;
    while (code > 0 && celsius[code - 1] < t) code--;
    if (code > 0 && fabsf(celsius[code - 1] - t) < fabsf(celsius[code] - t)) code--;
    return code;
  }

private:
  float celsius[1024] = { 0 };
  bool built = false;
};

/**
 * One heater and the sensor that watches it.
 *
 * Attaches on construction and detaches on destruction, so a test that does not ask for
 * a live heater keeps the steady readings `SimulatedSensors` gives it.
 */
class SimulatedHeater : public Peripheral {
public:

  // Where an unpowered heater settles, and where a fully powered one would end up.
  static constexpr float AMBIENT_C = SimulatedHardware::AMBIENT_C;

  /**
   * A heater that loses heat as fast as it gains it is the ordinary case, and the
   * default. It is not the only case worth modelling: a well insulated block with a
   * powerful element rises in seconds and falls over minutes, and that asymmetry is
   * what decides which half of a relay cycle is the long one. Anything about the
   * control loop's *balance* — which way a bias moves, and how far — needs a heater
   * whose two directions can differ, so cooling has a time constant of its own.
   */
  SimulatedHeater(const pin_t heater_pin, const int8_t adc_channel,
                  const float full_power_c, const float time_constant_s,
                  const float cooling_time_constant_s = 0.0f)
    : heater_pin(heater_pin), adc_channel(adc_channel),
      full_power_c(full_power_c), tau_s(time_constant_s),
      cool_tau_s(cooling_time_constant_s > 0.0f ? cooling_time_constant_s : time_constant_s) {
    SimulatedHardware::ensure_ready();
    was_code = SimulatedHardware::adc_code(adc_channel);
    temp_c = AMBIENT_C;
    last_us = Clock::micros();
    powered = Gpio::get(heater_pin) != 0;
    Gpio::attachPeripheral(heater_pin, this);
  }

  virtual ~SimulatedHeater() {
    Gpio::attachPeripheral(heater_pin, nullptr);
    SimulatedHardware::drive_adc(adc_channel, was_code);
  }

  // Called by Gpio on every write to the heater pin — that is, by the temperature ISR.
  void interrupt(GpioEvent) override { advance(); }
  void update() override { advance(); }

  // Where the model thinks the heater is, independent of what the firmware has read.
  float temperature() const { return temp_c; }

  // Start somewhere other than ambient. Returns the temperature the sensor will read,
  // which is the nearest count to what was asked for rather than the figure itself.
  float starts_at(const float c) {
    code = curve().nearest_from(code, c);
    temp_c = curve().at(code);              // model and sensor agree exactly
    SimulatedHardware::drive_adc(adc_channel, code);
    return temp_c;
  }

  /**
   * Start at the highest reading the sensor can give that is still below `limit`.
   *
   * A test that wants to be "just under the target" cannot name a temperature, because
   * a 10-bit ADC may not be able to express one on the right side of the line. This
   * names the side instead and lets the hardware pick the value.
   */
  float starts_just_below(const float limit) {
    uint16_t c = curve().nearest_from(code, limit);
    while (c < 1023 && !(curve().at(c) < limit)) c++;
    code = c;
    temp_c = curve().at(code);
    SimulatedHardware::drive_adc(adc_channel, code);
    return temp_c;
  }

private:

  void advance() {
    const uint64_t now = Clock::micros();
    if (now <= last_us) return;
    const float dt = float(now - last_us) * 1e-6f;
    last_us = now;

    // First order towards full power or towards ambient, whichever is switched on,
    // each at its own rate.
    const float towards = powered ? full_power_c : AMBIENT_C;
    const float step = dt / (powered ? tau_s : cool_tau_s);
    temp_c += (towards - temp_c) * (step < 1.0f ? step : 1.0f);

    // The state that will apply over the interval starting now.
    powered = Gpio::get(heater_pin) != 0;

    code = curve().nearest_from(code, temp_c);
    SimulatedHardware::drive_adc(adc_channel, code);
  }

  // Built lazily so the conversion tables are ready by the time it is asked for.
  AdcCurve &curve() {
    if (!curve_.is_built()) {
      #if HAS_HOTEND
        if (adc_channel == TEMP_0_PIN)
          curve_.build([](const raw_adc_t r) { return thermalManager.analog_to_celsius_hotend(r, 0); });
        else
      #endif
      #if HAS_HEATED_BED
        if (adc_channel == TEMP_BED_PIN)
          curve_.build([](const raw_adc_t r) { return thermalManager.analog_to_celsius_bed(r); });
        else
      #endif
        curve_.build([](const raw_adc_t) { return celsius_float_t(0); });
    }
    return curve_;
  }

  pin_t heater_pin;
  int8_t adc_channel;
  float full_power_c, tau_s, cool_tau_s;
  float temp_c = AMBIENT_C;
  uint64_t last_us = 0;
  bool powered = false;
  uint16_t code = 0, was_code = 0;
  AdcCurve curve_;
};

#if HAS_HOTEND
  // A hotend that reaches 300 C at full power with a 4 second time constant: fast
  // enough that a heat-up costs a few seconds of simulated time, slow enough that the
  // configured PID gains behave like gains rather than like a bang-bang switch.
  class SimulatedHotend : public SimulatedHeater {
  public:
    SimulatedHotend() : SimulatedHeater(HEATER_0_PIN, TEMP_0_PIN, 400.0f, 20.0f) {}
  };
#endif

#if HAS_HEATED_BED
  /**
   * A bed has to be modelled much more slowly than a hotend, and not only for realism.
   * The bed is bang-bang controlled and `manage_heated_bed()` only reconsiders every
   * BED_CHECK_INTERVAL (5 s), so whatever the bed does between checks is overshoot. A
   * bed that gained 5 C a second would swing far beyond TEMP_BED_HYSTERESIS every cycle
   * and M190 would never see it settle. 110 C at full power with a two minute time
   * constant keeps the swing inside the hysteresis, which is what a real bed's thermal
   * mass does.
   */
  class SimulatedBed : public SimulatedHeater {
  public:
    SimulatedBed() : SimulatedHeater(HEATER_BED_PIN, TEMP_BED_PIN, 110.0f, 120.0f) {}
  };
#endif

