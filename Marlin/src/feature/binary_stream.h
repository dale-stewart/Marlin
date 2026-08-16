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
#pragma once

/**
 * The framed packet protocol for sending a file to the printer as bytes.
 *
 * This header declares the stream; the protocol itself lives in binary_stream.cpp, along with
 * SDFileTransferProtocol, which is an implementation detail of dispatch() and is referenced
 * nowhere outside that file.
 */

#include "../inc/MarlinConfig.h"

#define BINARY_STREAM_COMPRESSION

class BinaryStream {
public:
  enum class Protocol : uint8_t { CONTROL, FILE_TRANSFER };

  enum class ProtocolControl : uint8_t { SYNC = 1, CLOSE };

  enum class StreamState : uint8_t { PACKET_RESET, PACKET_WAIT, PACKET_HEADER, PACKET_DATA, PACKET_FOOTER,
                                     PACKET_PROCESS, PACKET_RESEND, PACKET_TIMEOUT, PACKET_ERROR };

  struct Packet { // 10 byte protocol overhead, ascii with checksum and line number has a minimum of 7 increasing with line

    union Header {
      static constexpr uint16_t header_token = 0xB5AD;
      struct [[gnu::packed]] {
        uint16_t token;       // packet start token
        uint8_t sync;         // stream sync, resend id and packet loss detection
        uint8_t meta;         // 4 bit protocol,
                              // 4 bit packet type
        uint16_t size;        // data length
        uint16_t checksum;    // header checksum
      };
      uint8_t protocol();
      uint8_t type();
      void reset();
      uint8_t data[2];
    };

    union Footer {
      struct [[gnu::packed]] {
        uint16_t checksum; // full packet checksum
      };
      void reset();
      uint8_t data[1];
    };

    Header header;
    Footer footer;
    uint32_t bytes_received;
    uint16_t checksum, header_checksum;
    millis_t timeout;
    char* buffer;

    void reset();
  } packet{};

  void reset();

  /**
   * Read whatever has arrived on the transfer port, and answer it.
   *
   * The buffer is where a packet's payload is assembled, and its length is part of the protocol
   * rather than an implementation detail: it is reported to the sender in the `ss` reply as the
   * largest packet it may send, and it bounds the overrun check.
   *
   * Callers pass the array and the length is taken from it. The array form is a one-line forward
   * to the pointer form deliberately — it keeps the length derived from the buffer at every call
   * site, which a pointer and a separately-stated size cannot, while leaving the protocol itself
   * in a translation unit where it can be measured.
   */
  template<const size_t buffer_size>
  void receive(char (&buffer)[buffer_size]) { receive(&buffer[0], buffer_size); }

  void receive(char * const buffer, const size_t buffer_size);

private:
  // fletchers 16 checksum
  uint32_t checksum(uint32_t cs, uint8_t value);

  // read the next byte from the data stream keeping track of
  // whether the stream times out from data starvation
  // takes the data variable by reference in order to return status
  bool stream_read(uint8_t& data);

  void dispatch();

  void idle();

  static const uint16_t packet_max_wait = 500, rx_timeslice = 20, max_retries = 0, version_major = 0, version_minor = 1, version_patch = 0;
  uint8_t  packet_retries, sync;
  uint16_t buffer_next_index;
  uint32_t bytes_received;
  StreamState stream_state = StreamState::PACKET_RESET;
};

extern BinaryStream binaryStream[NUM_SERIAL];
