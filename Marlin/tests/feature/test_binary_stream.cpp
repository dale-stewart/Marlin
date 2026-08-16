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
#include "src/gcode/queue.h"
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
   *
   * **The buffer is `MAX_CMD_SIZE` because that is what the queue hands the reader**, and the
   * length is part of the protocol rather than an implementation detail: it is reported to the
   * sender as the largest packet it may send, and it bounds the overrun check. Picking a
   * comfortable number here would test the reader against a machine that does not exist.
   *
   * It used to matter for a second reason, now designed away: `receive()` was a template on the
   * length, so the size chose which *instantiation* ran, and a test with its own number silently
   * exercised a copy of the reader the firmware never builds. The array form is now a one-line
   * forward to a single out-of-line definition, so there is only one copy to measure.
   */
  std::string send(const std::vector<uint8_t> &bytes) {
    static char line_buffer[MAX_CMD_SIZE];
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
   * Put bytes on the port and let the *queue* decide what to do with them.
   *
   * `send()` above calls the reader directly, which is right for testing the protocol and wrong
   * for testing the handover: it proves the reader answers, not that anything ever gives it the
   * bytes. This goes in at `get_available_commands()`, the entry point the main loop calls, so
   * the branch in `get_serial_commands()` that chooses between the parser and the stream is the
   * thing under test.
   */
  std::string pump(const std::vector<uint8_t> &bytes) {
    SerialCapture reply;
    for (const uint8_t b : bytes) MYSERIAL1.receive_buffer.write(b);
    queue.get_available_commands();
    return reply.finish();
  }

  void feed_text(const char * const line) {
    for (const char *p = line; *p; ++p) MYSERIAL1.receive_buffer.write(uint8_t(*p));
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
      card.flag.binary_mode = false;
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

/**
 * A packet that arrives intact and in step is acknowledged by number, and the stream moves on.
 *
 * `ok<n>` is the only thing that tells a sender the packet is safely in — it is what releases the
 * next one and what a resend is triggered by the absence of. The number matters as much as the
 * word: a sender with several packets outstanding uses it to decide which one was received.
 *
 * The packet used here is `CLOSE`, which leaves binary mode. That gives the acknowledgement a
 * *companion*: the reply says the packet was accepted, and the mode change says it was acted on.
 * Asserting only the reply would pass on a reader that acknowledged everything and did nothing.
 */
MARLIN_TEST(binary_stream, an_accepted_packet_is_acknowledged_by_number_and_acted_on) {
  FreshStream stream;
  card.flag.binary_mode = true;

  const std::string first = send(packet(0, PROTOCOL_CONTROL, CONTROL_CLOSE));
  TEST_ASSERT_TRUE_MESSAGE(said(first, "ok0"),
    "an intact, in-step packet should be acknowledged by its sequence number");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.binary_mode,
    "and acted on - a close packet leaves binary mode, which is what makes the acknowledgement "
    "more than a reply to itself");

  // The stream is now expecting packet 1, so 1 is what the next acknowledgement must name.
  card.flag.binary_mode = true;
  const std::string second = send(packet(1, PROTOCOL_CONTROL, CONTROL_CLOSE));
  TEST_ASSERT_TRUE_MESSAGE(said(second, "ok1"),
    "and the sequence number advances - a sender with packets outstanding uses the number to "
    "tell which one arrived");
}

/**
 * A packet the sender resent because the acknowledgement was lost is acknowledged again, and its
 * payload is dropped.
 *
 * This is the guarantee that makes the protocol safe to lose bytes on. The sender cannot tell a
 * lost packet from a lost acknowledgement, so it resends; if the printer acted on the resend, the
 * data would be written twice. For a firmware image that is a corrupted image from a link that
 * never actually dropped anything.
 *
 * The reader accepts one sequence number behind and answers without dispatching. Observing "not
 * dispatched" needs an effect to look for, so binary mode is put back on between the two: if the
 * resent close were acted on, it would come off again.
 */
MARLIN_TEST(binary_stream, a_resent_packet_is_acknowledged_again_but_not_acted_on_twice) {
  FreshStream stream;
  card.flag.binary_mode = true;

  send(packet(0, PROTOCOL_CONTROL, CONTROL_CLOSE));   // received and acted on; sync is now 1
  card.flag.binary_mode = true;

  const std::string again = send(packet(0, PROTOCOL_CONTROL, CONTROL_CLOSE));

  TEST_ASSERT_TRUE_MESSAGE(said(again, "ok0"),
    "a packet resent because its acknowledgement was lost should be acknowledged again - the "
    "sender cannot tell a lost packet from a lost reply, and needs the same answer either way");
  TEST_ASSERT_TRUE_MESSAGE(card.flag.binary_mode,
    "but must not be acted on twice - a duplicate applied is a duplicate written into the file, "
    "which is how a link that dropped nothing still corrupts an upload");
}

/**
 * A packet from the wrong place in the sequence is refused, named, and a resend asked for.
 *
 * Silently accepting it would splice the file together in the wrong order — every byte intact,
 * every checksum passing, and the result wrong. So the sequence number is a guarantee in its own
 * right, and the reply carries the number the stream actually wants rather than the one that
 * arrived: a sender needs to know where to restart, not merely that it was wrong.
 */
MARLIN_TEST(binary_stream, a_packet_out_of_sequence_is_refused_and_a_resend_requested) {
  FreshStream stream;

  const std::string reply = send(packet(7, PROTOCOL_CONTROL, CONTROL_CLOSE));

  TEST_ASSERT_TRUE_MESSAGE(said(reply, "out of order"),
    "a packet from the wrong place in the sequence should be refused as out of order - accepting "
    "it would assemble the file wrongly with every checksum passing");
  TEST_ASSERT_TRUE_MESSAGE(said(reply, "rs0"),
    "and the resend request should name the sequence number the stream wants, not the one that "
    "arrived - the sender needs to know where to restart");
  TEST_ASSERT_FALSE_MESSAGE(said(reply, "ok"),
    "and it must not also be acknowledged");
}

/**
 * Once a resend has been asked for, further out-of-step packets are dropped without a word.
 *
 * This is deliberate and easy to read as a bug. A flow-controlled link may already have several
 * packets in flight when the resend request goes out; answering each of them would produce a
 * resend request per packet, and the sender would resend the whole run again. So after the first
 * request the reader stays quiet until the sequence it asked for arrives.
 *
 * The cost is that a genuinely desynchronised stream is silent rather than diagnostic, which is
 * why this is worth pinning: it is a design choice, and a test is what stops it being "fixed".
 */
MARLIN_TEST(binary_stream, after_a_resend_request_further_stray_packets_are_dropped_silently) {
  FreshStream stream;

  send(packet(7, PROTOCOL_CONTROL, CONTROL_CLOSE));   // asks for a resend, and starts counting

  const std::string second = send(packet(9, PROTOCOL_CONTROL, CONTROL_CLOSE));

  TEST_ASSERT_FALSE_MESSAGE(said(second, "out of order"),
    "a second stray packet should be dropped without comment - packets already in flight when "
    "the resend request went out would otherwise each provoke another request");
  TEST_ASSERT_FALSE_MESSAGE(said(second, "rs"),
    "and without a second resend request, which the sender would answer by resending the run");
}

/**
 * The stream asks for a resend for ever; it never declares the transfer failed.
 *
 * `max_retries` is 0, and the guard reads `packet_retries < max_retries || max_retries == 0`, so
 * the zero is not "no retries" but "no limit". The `fe` reply and the reset behind it are
 * therefore unreachable in the shipped firmware, and a transfer that cannot resynchronise leaves
 * the printer waiting rather than reporting.
 *
 * Recorded rather than changed: the register entry is what carries the argument, and this test is
 * what pins the behaviour so a change to it is visible.
 *
 * Corrupt headers are what drive the count, because they are refused on their own terms rather
 * than through the sequence check — so unlike a stray packet each one reaches the resend path and
 * increments the retry counter. Sixteen consecutive failures is well past any plausible limit.
 */
MARLIN_TEST(binary_stream, the_stream_never_declares_a_transfer_failed) {
  FreshStream stream;

  std::string last;
  for (int i = 0; i < 16; ++i) {
    last = send(packet(0, PROTOCOL_CONTROL, CONTROL_CLOSE, "", /*corrupt_header=*/true));
    TEST_ASSERT_TRUE_MESSAGE(said(last, "rs0"),
      "every failed packet should draw another resend request, however many have failed already");
  }

  TEST_ASSERT_FALSE_MESSAGE(said(last, "fe"),
    "and the stream should never give up - max_retries is 0, which this guard reads as no limit "
    "rather than no retries, so the failure reply cannot be reached");
}

/**
 * In binary mode the queue hands the port to the stream instead of the parser.
 *
 * Every test above calls the reader directly, which says nothing about whether the firmware ever
 * reaches it. `get_serial_commands()` opens with a branch on `card.flag.binary_mode`: on one side
 * the bytes are accumulated into lines and parsed as G-code, on the other they go to the stream
 * and the function returns without looking at them further. That branch is the entire connection
 * between the protocol and the printer, and nothing was checking it.
 *
 * It is asserted as a *switch* rather than as an effect, by driving both sides through the same
 * entry point. Text arriving in ASCII mode has to become a queued command, or the negative half
 * of the binary case would be satisfied by a port that was simply not being read. The bytes are
 * different in the two arms because they have to be: a packet is not a line of G-code and a line
 * of G-code is not a packet — what is held still is the path they take in.
 */
MARLIN_TEST(binary_stream, binary_mode_hands_the_port_to_the_stream_rather_than_the_parser) {
  FreshStream stream;
  queue.clear();

  // ASCII mode: the queue reads the port itself and a line becomes a command.
  card.flag.binary_mode = false;
  feed_text("G4 P0\n");
  pump({});
  TEST_ASSERT_TRUE_MESSAGE(queue.has_commands_queued(),
    "in ASCII mode the queue should read the port and queue the line - without this the binary "
    "assertion below would also pass on a port nobody was reading");
  queue.clear();

  // Binary mode: the same entry point, and the bytes reach the stream instead.
  card.flag.binary_mode = true;
  const std::string reply = pump(packet(0, PROTOCOL_CONTROL, CONTROL_SYNC));

  TEST_ASSERT_TRUE_MESSAGE(said(reply, "ss"),
    "in binary mode the queue should hand the port to the stream, which answers the sync - this "
    "is the only thing connecting the protocol to the firmware");
  TEST_ASSERT_FALSE_MESSAGE(queue.has_commands_queued(),
    "and must not also parse the packet as text - a packet interpreted as G-code is a printer "
    "acting on the bytes of a firmware image");

  queue.clear();
}

#endif // BINARY_FILE_TRANSFER
