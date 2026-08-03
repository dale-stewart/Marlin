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
#ifdef __PLAT_LINUX__

#include "../../../inc/MarlinConfig.h"
#include "Clock.h"

// `startup` is deliberately absent: it is a function-local static in Clock.h, constructed
// on first use, so that a caller reading the clock during static initialisation cannot
// see a zero baseline. The two below are constant-initialised, which happens before any
// dynamic initialisation, so they carry no ordering hazard.
uint32_t Clock::frequency = F_CPU;
double Clock::time_multiplier = 1.0;

#endif // __PLAT_LINUX__
