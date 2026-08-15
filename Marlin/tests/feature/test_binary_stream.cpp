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
 * Sending a file to the printer as bytes rather than as text.
 *
 * Ordinary G-code upload is ASCII with a checksum per line, which costs about a third of the link
 * to encoding. The binary protocol replaces it with framed packets — token, header, payload,
 * footer — and it is what `CUSTOM_FIRMWARE_UPLOAD` sends a firmware image over. That is the reason
 * to care: a corrupted G-code line prints one bad layer, and a corrupted firmware image bricks the
 * board. Every guarantee below exists to stop the second.
 *
 * The file was at **0%** over 240 lines. It is a protocol, which makes it good test material for
 * the same reason `e_parser.cpp` was: the assertions can be exact, because the protocol publishes
 * its own vocabulary. `ss` acknowledges a sync, `ok<n>` acknowledges packet *n*, and the failures
 * are named — `Packet header(n?) corrupt`, `Packet(n) payload corrupt`, `Datastream packet out of
 * order`. Those are what a sender parses, so those are what a test should assert.
 *
 * ## Speaking the protocol
 *
 * The tests build packets byte by byte rather than calling anything in the firmware to do it,
 * which is the point: a test that asked the code under test to frame its own input would agree
 * with any framing at all. The layout, read from the header:
 *
 *     token   uint16  0xB5AD, little-endian
 *     sync    uint8   sequence number
 *     meta    uint8   protocol in the high nibble, packet type in the low
 *     size    uint16  payload length
 *     cksum   uint16  Fletcher-16 over the four bytes above it — *not* the token
 *
 * That last detail is worth stating because it is not obvious and it is not written down
 * anywhere: the running checksum starts at zero in `PACKET_HEADER`, which the reader enters only
 * *after* the token has matched, and it is snapshotted when six of the eight header bytes have
 * arrived. So the token contributes nothing and the checksum field cannot cover itself.
 */

#include "../test/unit_tests.h"
#include "src/inc/MarlinConfig.h"

#if ENABLED(BINARY_FILE_TRANSFER)

#include "../gcode/serial_capture.h"
#include "src/sd/cardreader.h"
#include "src/feature/binary_stream.h"
#include <string>
#include <vector>

namespace {

  // Fletcher-16, written from the algorithm rather than copied from the reader: the low byte
  // accumulates the data, the high byte accumulates the low byte, both modulo 255.
  uint16_t fletcher16(const std::vector<uint8_t> &bytes) {
    uint16_t low = 0, high = 0;
    for (const uint8_t b : bytes) { low = uint16_t((low + b) % 255); high = uint16_t((high + low) % 255); }
    return uint16_t((high << 8) | low);
  }

  enum : uint8_t { PROTOCOL_CONTROL = 0, PROTOCOL_FILE = 1 };
  enum : uint8_t { CONTROL_SYNC = 1, CONTROL_CLOSE = 2 };

  /**
   * A framed packet, ready to be pushed at the port.
   *
   * `corrupt_header` and `corrupt_payload` damage exactly one checksum each, so a test can say
   * which guarantee it is exercising rather than sending noise and hoping.
   */
  std::vector<uint8_t> packet(const uint8_t sync, const uint8_t protocol, const uint8_t type,
                              const std::string &payload = "",
                              const bool corrupt_header = false,
                              const bool corrupt_payload = false) {
    const uint16_t size = uint16_t(payload.size());
    const uint8_t meta = uint8_t((protocol << 4) | (type & 0xF));

    const std::vector<uint8_t> covered = { sync, meta, uint8_t(size & 0xFF), uint8_t(size >> 8) };
    uint16_t hcs = fletcher16(covered);
    if (corrupt_header) hcs = uint16_t(hcs ^ 0xFFFF);

    std::vector<uint8_t> out = {
      0xAD, 0xB5, sync, meta, uint8_t(size & 0xFF), uint8_t(size >> 8),
      uint8_t(hcs & 0xFF), uint8_t(hcs >> 8)
    };

    if (size) {
      std::vector<uint8_t> data(payload.begin(), payload.end());
      out.insert(out.end(), data.begin(), data.end());
      uint16_t pcs = fletcher16(data);
      if (corrupt_payload) pcs = uint16_t(pcs ^ 0xFFFF);
      out.push_back(uint8_t(pcs & 0xFF));
      out.push_back(uint8_t(pcs >> 8));
    }
    return out;
  }

  /**
   * Push a packet at the port and let the reader consume it, returning what it answered.
   *
   * The port's receive buffer is 128 bytes and the reader takes what is there and returns, so a
   * packet has to fit — which is why the payloads here are short. `receive()` is called twice
   * because a single call returns as soon as the data runs out, and the state machine may need
   * another pass to reach the state that answers.
   */
  std::string send(const std::vector<uint8_t> &bytes) {
    static char line_buffer[256];
    SerialCapture reply;
    for (const uint8_t b : bytes) MYSERIAL1.receive_buffer.write(b);
    binaryStream[card.transfer_port_index.index].receive(line_buffer);
    binaryStream[card.transfer_port_index.index].receive(line_buffer);
    return reply.finish();
  }

  bool said(const std::string &reply, const char * const words) {
    return reply.find(words) != std::string::npos;
  }

  /**
   * The stream is a process-wide object with a sync counter, so a test that advances it changes
   * where every later test starts. Resetting it is the same reasoning as the emergency parser's
   * latches.
   */
  struct FreshStream {
    FreshStream()  { reset(); }
    ~FreshStream() { reset(); }
    static void reset() {
      MYSERIAL1.receive_buffer.clear();
      binaryStream[card.transfer_port_index.index].reset();
    }
  };

}

/**
 * A sync packet is answered with the stream's terms.
 *
 * `ss` is how a sender discovers what it is talking to — the sync number to start from, the
 * buffer size it must not exceed, and the protocol version. Everything else in the conversation
 * depends on those three, so a sender that cannot get them cannot begin.
 *
 * `SYNC` is deliberately the one packet whose sequence number is *not* checked, because a sender
 * that has lost track has no way to guess the right one; the whole purpose of the packet is to be
 * answerable from an unknown state. Sending it with a wildly wrong sync is therefore the case
 * worth asserting, not an aside.
 */
MARLIN_TEST(binary_stream, a_sync_packet_is_answered_from_any_state) {
  FreshStream stream;

  const std::string reply = send(packet(0, PROTOCOL_CONTROL, CONTROL_SYNC));
  TEST_ASSERT_TRUE_MESSAGE(said(reply, "ss"),
    "a sync packet should be acknowledged with the stream's terms");

  const std::string out_of_step = send(packet(200, PROTOCOL_CONTROL, CONTROL_SYNC));
  TEST_ASSERT_TRUE_MESSAGE(said(out_of_step, "ss"),
    "and answered whatever sequence number it carries - a sender that has lost track cannot "
    "guess the right one, which is what the packet is for");
}

/**
 * A packet whose header is damaged is refused, and named.
 *
 * The header carries the payload length, so a corrupted one is not merely a lost packet: acting
 * on it would read the wrong number of bytes and swallow the packets after it. The reader
 * therefore checks the header on its own, before any payload arrives, and says which sequence
 * number it could not read.
 *
 * The corruption is exactly one flipped checksum rather than random noise, so this asserts the
 * check rather than the reader's behaviour on garbage.
 */
MARLIN_TEST(binary_stream, a_packet_with_a_damaged_header_is_refused) {
  FreshStream stream;

  const std::string reply = send(packet(0, PROTOCOL_CONTROL, CONTROL_SYNC, "", /*corrupt_header=*/true));

  TEST_ASSERT_TRUE_MESSAGE(said(reply, "corrupt"),
    "a header that fails its checksum should be refused and said to be corrupt");
  TEST_ASSERT_FALSE_MESSAGE(said(reply, "ss"),
    "and must not be acted on - the header carries the length, so acting on a damaged one "
    "would consume the packets behind it");
}

/**
 * A packet whose payload is damaged is refused too, and refused differently.
 *
 * Two checksums guard two different things, and the reader distinguishes them in what it says:
 * a bad header means it does not know how much is coming, a bad payload means it knows and what
 * arrived is wrong. A sender uses the difference to decide what to resend, so collapsing the two
 * messages would be a protocol change rather than a cosmetic one.
 *
 * This is also the guarantee that matters for the firmware upload the mode exists to carry.
 */
MARLIN_TEST(binary_stream, a_packet_with_a_damaged_payload_is_refused_and_said_so) {
  FreshStream stream;

  // Get in step first: a payload packet is only examined once its sequence number is accepted.
  send(packet(0, PROTOCOL_CONTROL, CONTROL_SYNC));

  const std::string reply =
    send(packet(0, PROTOCOL_FILE, 0, "hello", /*corrupt_header=*/false, /*corrupt_payload=*/true));

  TEST_ASSERT_TRUE_MESSAGE(said(reply, "payload corrupt"),
    "a payload that fails its checksum should be refused, and named as the payload rather than "
    "the header - a sender uses the difference to decide what to resend");
}

#endif // BINARY_FILE_TRANSFER
