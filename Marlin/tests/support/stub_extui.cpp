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

// The concrete UI that `ui_api.cpp` links against here. See stub_extui.h for why it records.

#include "stub_extui.h"

#if defined(__PLAT_TEST__) && ENABLED(EXTENSIBLE_UI)

#include "src/lcd/extui/ui_api.h"

uint16_t RecordedUI::startups = 0, RecordedUI::idles = 0;
uint16_t RecordedUI::homing_starts = 0, RecordedUI::homing_dones = 0;
uint16_t RecordedUI::steppers_enabled = 0, RecordedUI::steppers_disabled = 0;
uint16_t RecordedUI::factory_resets = 0, RecordedUI::postprocesses = 0;
uint16_t RecordedUI::print_started = 0, RecordedUI::print_paused = 0, RecordedUI::print_stopped = 0;
uint16_t RecordedUI::heating_errors = 0, RecordedUI::min_temp_errors = 0, RecordedUI::max_temp_errors = 0;
uint16_t RecordedUI::killed = 0, RecordedUI::confirms_required = 0;
uint16_t RecordedUI::axes_enabled = 0, RecordedUI::axes_disabled = 0;
std::string RecordedUI::last_status;
std::string RecordedUI::last_confirm;

void RecordedUI::reset() {
  startups = idles = homing_starts = homing_dones = 0;
  steppers_enabled = steppers_disabled = factory_resets = postprocesses = 0;
  print_started = print_paused = print_stopped = 0;
  heating_errors = min_temp_errors = max_temp_errors = 0;
  killed = confirms_required = axes_enabled = axes_disabled = 0;
  last_status.clear();
  last_confirm.clear();
}

namespace ExtUI {

  void onStartup()                                  { RecordedUI::startups++; }
  void onIdle()                                     { RecordedUI::idles++; }

  void onHomingStart()                              { RecordedUI::homing_starts++; }
  void onHomingDone()                               { RecordedUI::homing_dones++; }

  void onSteppersEnabled()                          { RecordedUI::steppers_enabled++; }
  void onSteppersDisabled()                         { RecordedUI::steppers_disabled++; }
  void onAxisEnabled(const axis_t)                  { RecordedUI::axes_enabled++; }
  void onAxisDisabled(const axis_t)                 { RecordedUI::axes_disabled++; }

  void onFactoryReset()                             { RecordedUI::factory_resets++; }
  void onPostprocessSettings()                      { RecordedUI::postprocesses++; }

  void onPrintTimerStarted()                        { RecordedUI::print_started++; }
  void onPrintTimerPaused()                         { RecordedUI::print_paused++; }
  void onPrintTimerStopped()                        { RecordedUI::print_stopped++; }

  void onHeatingError(const heater_id_t)            { RecordedUI::heating_errors++; }
  void onMinTempError(const heater_id_t)            { RecordedUI::min_temp_errors++; }
  void onMaxTempError(const heater_id_t)            { RecordedUI::max_temp_errors++; }

  void onPrinterKilled(FSTR_P const, FSTR_P const)  { RecordedUI::killed++; }

  void onStatusChanged(const char * const msg) {
    RecordedUI::last_status = msg ? msg : "";
  }

  void onUserConfirmRequired(const char * const msg) {
    RecordedUI::confirms_required++;
    RecordedUI::last_confirm = msg ? msg : "";
  }

  #if ENABLED(PREVENT_COLD_EXTRUSION)
    void onSetMinExtrusionTemp(const celsius_t) {}
  #endif

  #if HAS_PID_HEATING
    void onPIDTuning(const pidresult_t) {}
    void onStartM303(const int, const heater_id_t, const celsius_t) {}
  #endif

}

#endif // __PLAT_TEST__ && EXTENSIBLE_UI
