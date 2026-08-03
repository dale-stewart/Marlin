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
 * An SPI bus with nothing attached to it.
 *
 * Enabling media compiles a block-device driver that talks to a card over SPI, so these
 * symbols have to exist for the firmware to link at all — neither native HAL defined
 * them, which is why media could not be built for a test until now.
 *
 * The behaviour modelled here is an idle bus, not a working card: MISO floats high, so
 * every read is 0xFF and every write goes nowhere. That is what a real controller sees
 * with an empty slot, and it makes the honest outcome — "no card" — the default.
 *
 * Tests that want media supply their own storage instead of pretending this bus has a
 * card on it — see `Marlin/tests/support/simulated_media.h`, a RAM-backed FAT16 volume
 * injected through `CardReader::changeMedia()`. None of the code below is on that path.
 * Simulating a card at the wire level would mean implementing SD's command protocol to
 * test code that sits well above it.
 */

#ifdef __PLAT_LINUX__

#include "../../inc/MarlinConfig.h"
#include "../shared/HAL_SPI.h"

void spiBegin() {}

void spiInit(uint8_t) {}

void spiSend(uint8_t) {}

uint8_t spiRec() { return 0xFF; }   // MISO idles high: no device is driving the line

void spiRead(uint8_t *buf, uint16_t nbyte) {
  for (uint16_t i = 0; i < nbyte; ++i) buf[i] = 0xFF;
}

void spiSendBlock(uint8_t, const uint8_t*) {}

#endif // __PLAT_LINUX__
