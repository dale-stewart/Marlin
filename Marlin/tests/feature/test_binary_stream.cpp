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
#include <string.h>

namespace {

  // Fletcher-16, written from the algorithm rather than copied from the reader: the low byte
  // accumulates the data, the high byte accumulates the low byte, both modulo 255.
  //
  // Takes a running value because the reader's does. See `packet()` for why that matters.
  uint16_t fletcher16(const std::vector<uint8_t> &bytes, const uint16_t running = 0) {
    uint16_t low = running & 0xFF, high = (running >> 8) & 0xFF;
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
      /**
       * The payload checksum continues the header's, it does not start again.
       *
       * `packet.checksum` is a single running value across the whole packet. The header
       * checksum is a *snapshot* of it taken two bytes early — which is what lets that field
       * avoid covering itself — but the running value carries on over the checksum bytes and
       * then over the payload, and that is what the footer is compared against.
       *
       * So the footer covers the six header bytes after the token, the two header-checksum
       * bytes, and the payload. Computing it over the payload alone produces a packet the
       * reader always calls corrupt, which is indistinguishable from the reader working.
       */
      uint16_t running = fletcher16({ uint8_t(hcs & 0xFF), uint8_t(hcs >> 8) }, fletcher16(covered));

      std::vector<uint8_t> data(payload.begin(), payload.end());
      out.insert(out.end(), data.begin(), data.end());
      uint16_t pcs = fletcher16(data, running);
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

  // The port spins forever on a full transmit buffer when it believes a host is listening, and
  // the media layer reports freely. See CLAUDE.md.
  struct NoHostAttached {
    bool was;
    NoHostAttached() { was = MYSERIAL1.host_connected; MYSERIAL1.host_connected = false; }
    ~NoHostAttached() { MYSERIAL1.host_connected = was; }
  };

  enum : uint8_t { FILE_QUERY = 0, FILE_OPEN = 1, FILE_CLOSE = 2, FILE_WRITE = 3, FILE_ABORT = 4 };

  /**
   * The payload of an OPEN packet: two flag bytes, then a NUL-terminated name.
   *
   * `dummy` asks the printer to accept the transfer and throw it away — the sender's way of
   * measuring the link without writing to the card — and `compression` selects heatshrink. Both
   * are read as bit zero only, so the rest of each byte is spare.
   */
  std::string open_payload(const char * const name, const bool dummy = false, const bool compressed = false) {
    std::string p;
    p += char(dummy ? 1 : 0);
    p += char(compressed ? 1 : 0);
    p += name;
    p += '\0';
    return p;
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

  /**
   * Let the reader's time slice actually expire.
   *
   * `receive()` loops until `rx_timeslice` has passed, and only the wait-for-a-token state
   * returns early — every other state loops back on starvation. With a clock that moves only on
   * request, a stream that stops mid-packet therefore spins for ever, which is the one shape of
   * firmware this harness cannot otherwise run.
   *
   * Charging an empty poll against the clock ends the loop for the same reason it ends on a
   * board. A microsecond a poll closes the 20 ms slice in twenty thousand iterations, which is
   * instant in real time. The teardown puts it back to free, because the failure path skips
   * destructors.
   */
  struct PollingCostsTime {
    PollingCostsTime(const uint64_t ns = 1000) { HAL_test_set_idle_poll_nanos(ns); }
    ~PollingCostsTime() { HAL_test_set_idle_poll_nanos(0); }
  };

  void feed_text(const char * const line) {
    for (const char *p = line; *p; ++p) MYSERIAL1.receive_buffer.write(uint8_t(*p));
  }

  /**
   * The stream is a process-wide object with a sync counter, so a test that advances it changes
   * where every later test starts. Resetting it is the same reasoning as the emergency parser's
   * latches.
   */
  struct FreshStream {
    /**
     * Every test here runs with an empty poll costing time, not only the one that needs it.
     *
     * A well-formed packet never reaches the spin — the wait-for-a-token state returns as soon
     * as the data runs out — so this changes nothing about what the tests below do. What it
     * changes is what happens when something is *wrong*: a reader stranded mid-packet returns
     * when its time slice expires, exactly as it would on a board, instead of hanging the
     * process.
     *
     * That matters most under mutation. With a frozen clock every mutant that strands the state
     * machine scores as a timeout, timeouts count as detected, and the suite gets credit for
     * detections that belong to the clock. With this on they run to completion and are judged on
     * what they actually did.
     */
    PollingCostsTime spinning;

    FreshStream()  { reset(); }
    ~FreshStream() { reset(); }
    static void reset() {
      MYSERIAL1.receive_buffer.clear();
      binaryStream[card.transfer_port_index.index].reset();
      card.flag.binary_mode = false;
    }
  };

  /**
   * A stream ready for a file transfer, with any transfer left running by an earlier test ended.
   *
   * `SDFileTransferProtocol` keeps `transfer_active` in a static, and only a close or an abort
   * clears it. A test that fails part way through a transfer therefore leaves the next one
   * answering `PFT:busy` to a perfectly good open — and the state lives inside the `.cpp`, so
   * neither a destructor nor the harness's teardown can reach it.
   *
   * So this clears it on the way *in* rather than on the way out, which is the one place the
   * failure path cannot skip.
   *
   * It sends CLOSE rather than ABORT, and the difference matters: `CLOSE` is guarded on
   * `transfer_active` and answers `PFT:invalid` when nothing is open, while `transfer_abort()`
   * has no such guard and deletes `card.filename` regardless — which, with no transfer running,
   * is whatever file the card last touched. See register #64. Cleaning up with the unguarded one
   * would have this fixture quietly deleting other tests' files.
   */
  struct FreshTransfer : FreshStream {
    uint8_t next_sync = 0;

    FreshTransfer() {
      SerialCapture quiet;   // it answers, and nobody is listening for it
      send(packet(0, PROTOCOL_FILE, FILE_CLOSE));
      FreshStream::reset();
    }

    // Each accepted packet advances the stream's sequence number, so a transfer has to count.
    std::string send_file_packet(const uint8_t type, const std::string &payload = "") {
      return send(packet(next_sync++, PROTOCOL_FILE, type, payload));
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
 * A packet carrying an intact payload is accepted, and its payload is not called corrupt.
 *
 * The companion the corrupt-payload test was missing. On its own, that test is satisfied by a
 * reader that rejects *every* payload — which is exactly what a mutant that miscounts the footer,
 * or ends the data phase one byte early, produces. Nothing here sent a valid non-empty payload
 * at all, so the whole data and footer path was covered without being pinned.
 *
 * The payload rides on a `CLOSE` packet because control packets ignore their buffer, which keeps
 * this a test of the framing rather than of what any particular payload means. Leaving binary
 * mode is what says the packet was acted on rather than merely acknowledged.
 */
MARLIN_TEST(binary_stream, a_packet_with_an_intact_payload_is_accepted) {
  FreshStream stream;
  card.flag.binary_mode = true;

  const std::string reply = send(packet(0, PROTOCOL_CONTROL, CONTROL_CLOSE, "hello"));

  TEST_ASSERT_TRUE_MESSAGE(said(reply, "ok0"),
    "a packet whose payload matches its checksum should be accepted");
  TEST_ASSERT_FALSE_MESSAGE(said(reply, "corrupt"),
    "and must not be called corrupt - a reader that rejected every payload would satisfy the "
    "corrupt-payload test on its own");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.binary_mode,
    "and it should be acted on, so the payload was read to its end rather than abandoned");
}

/**
 * Once the stream is back in step, a stray packet is reported again rather than dropped.
 *
 * The silence after a resend request is deliberate, but it has to end: it exists to swallow the
 * packets that were already in flight, not to make the stream permanently mute. A successful
 * packet is what says the sender has caught up, and it clears the retry count.
 *
 * Without that clearing, the first desynchronisation of a transfer would silence every later one
 * for the rest of the connection — the printer would drop stray packets without a word, and a
 * host waiting for a diagnostic would get nothing. That is a worse failure than the noise the
 * silence was introduced to prevent, and nothing was checking for it.
 */
MARLIN_TEST(binary_stream, a_stream_back_in_step_reports_strays_again) {
  FreshStream stream;

  send(packet(7, PROTOCOL_CONTROL, CONTROL_CLOSE));   // out of order: reported, and starts counting
  send(packet(9, PROTOCOL_CONTROL, CONTROL_CLOSE));   // silent, as the test above requires

  const std::string recovered = send(packet(0, PROTOCOL_CONTROL, CONTROL_CLOSE));
  TEST_ASSERT_TRUE_MESSAGE(said(recovered, "ok0"),
    "the packet the stream was waiting for should be accepted");

  const std::string stray = send(packet(9, PROTOCOL_CONTROL, CONTROL_CLOSE));
  TEST_ASSERT_TRUE_MESSAGE(said(stray, "out of order"),
    "and a stray after that should be reported again - the silence is for packets already in "
    "flight, not a permanent state the first desync leaves the connection in");
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
/**
 * A packet that stops half way is given up on, and a resend asked for.
 *
 * A sender that dies mid-packet — unplugged, crashed, or a link that dropped — leaves the reader
 * part way through a payload it has been told the length of. Without a timeout the printer waits
 * for the rest for ever, and the host sees a machine that has stopped answering rather than one
 * asking it to try again. `packet_max_wait` is what bounds that, and nothing was reaching it.
 *
 * Two things had to become true to write this at all. The reader has to *return* from a starved
 * packet, which needs an empty poll to cost simulated time — see `PollingCostsTime`. And the
 * stall has to outlast the reader's patience, which is what the deliberate jump in the clock
 * between the two halves is for.
 *
 * Both messages are asserted, because they say different things: one is why the reader gave up,
 * the other is what it wants the sender to do about it.
 */
MARLIN_TEST(binary_stream, a_packet_that_stops_half_way_times_out_and_asks_again) {
  FreshStream stream;

  // The header of a packet promising five bytes, and then nothing.
  std::vector<uint8_t> truncated = packet(0, PROTOCOL_CONTROL, CONTROL_CLOSE, "hello");
  truncated.resize(8);

  const std::string during = send(truncated);
  TEST_ASSERT_FALSE_MESSAGE(said(during, "timeout"),
    "a packet still arriving should not be given up on - the reader returns when its time slice "
    "expires, and that is not the same as the sender having stopped");

  HAL_test_advance_micros(600000);   // longer than packet_max_wait, so the sender has gone quiet

  const std::string after = send({});
  TEST_ASSERT_TRUE_MESSAGE(said(after, "Datastream timeout"),
    "a sender that stops mid-packet should be given up on rather than waited for indefinitely");
  TEST_ASSERT_TRUE_MESSAGE(said(after, "rs0"),
    "and the reader should ask for the packet again, naming where it wants to restart - "
    "otherwise the host sees a machine that has stopped answering");
}

/**
 * A file sent as packets arrives on the card byte for byte.
 *
 * The reason the whole protocol exists, and the first test here that goes all the way through:
 * open, write, close, then read the file back off the card and compare. Everything above this
 * asserts on what the printer *said*; this asserts on what it *kept*.
 *
 * That distinction is the point for the firmware upload the mode carries. A transfer that
 * acknowledges every packet and writes the wrong bytes is indistinguishable, on the wire, from
 * one that worked — and the failure only appears when the board is asked to run what it stored.
 *
 * The comparison is against the exact bytes sent rather than a length or a checksum, because a
 * length would pass on reordered content and a checksum computed here would agree with any
 * convention the writing side happened to use.
 */
MARLIN_TEST(binary_stream, a_file_sent_as_packets_arrives_on_the_card_byte_for_byte) {
  FreshTransfer transfer;
  const char * const contents = "G28\nG1 X10 Y10 F3000\nM104 S0\n";

  const std::string opened = transfer.send_file_packet(FILE_OPEN, open_payload("upload.gco"));
  TEST_ASSERT_TRUE_MESSAGE(said(opened, "PFT:success"), "the transfer should open");

  const std::string written = transfer.send_file_packet(FILE_WRITE, contents);
  TEST_ASSERT_FALSE_MESSAGE(said(written, "PFT:"),
    "a write that succeeds should say nothing - the packet acknowledgement is the answer, and a "
    "per-write reply would double the traffic the binary mode exists to save");

  const std::string closed = transfer.send_file_packet(FILE_CLOSE);
  TEST_ASSERT_TRUE_MESSAGE(said(closed, "PFT:success"), "the transfer should close cleanly");

  /**
   * Closing releases the card, so reading it back starts by picking it up again — and quietly.
   *
   * `openFileRead()` reports the name and size on the serial port, and outside a capture there
   * is nothing draining it: the port busy-waits for room in a 128-byte transmit buffer that only
   * the simulator's UI empties. Marking the port as having no host attached makes those writes
   * return immediately, which is the honest model of a test with no host listening.
   */
  NoHostAttached quiet;
  card.mount();
  card.openFileRead("upload.gco");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "the uploaded file should exist on the card");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(strlen(contents), card.getFileSize(),
    "and be exactly as long as what was sent");

  char back[64] = { 0 };
  const int16_t got = card.read(back, strlen(contents));
  card.closefile();

  TEST_ASSERT_EQUAL_INT16(int16_t(strlen(contents)), got);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(contents, back,
    "the file on the card should be byte for byte what was sent - a transfer that acknowledges "
    "every packet and stores the wrong bytes looks identical on the wire");
}

/**
 * A write with no transfer open is refused rather than acted on.
 *
 * `transfer_active` is what stands between an arriving payload and `card.write()`. Without the
 * guard, a WRITE that arrives after a close — or from a sender that never opened anything —
 * would be handed to whatever file the card happens to have open, which during a print is the
 * job being read. The refusal is named so a sender can tell it from an I/O failure and knows to
 * open rather than retry.
 */
MARLIN_TEST(binary_stream, a_write_with_no_transfer_open_is_refused) {
  FreshTransfer transfer;

  const std::string reply = transfer.send_file_packet(FILE_WRITE, "stray data");

  TEST_ASSERT_TRUE_MESSAGE(said(reply, "PFT:invalid"),
    "a write with nothing open should be refused as invalid, not attempted");
}

/**
 * A second transfer while one is running is refused, and does not disturb the first.
 *
 * One file at a time is the design — there is one `transfer_active` flag and one card handle —
 * so the interesting part is not that the second open fails but that the first survives it. If
 * the busy check came after the open, the second request would have closed the first file and
 * started a new one, and the sender of the first would go on writing into it.
 */
MARLIN_TEST(binary_stream, a_second_transfer_while_one_is_running_is_refused) {
  FreshTransfer transfer;

  transfer.send_file_packet(FILE_OPEN, open_payload("first.gco"));
  const std::string second = transfer.send_file_packet(FILE_OPEN, open_payload("second.gco"));

  TEST_ASSERT_TRUE_MESSAGE(said(second, "PFT:busy"),
    "a second open while a transfer is running should be refused as busy");

  const std::string written = transfer.send_file_packet(FILE_WRITE, "still the first file\n");
  TEST_ASSERT_FALSE_MESSAGE(said(written, "PFT:invalid"),
    "and the first transfer should still be open - a busy check that ran after the open would "
    "have closed the first file and left its sender writing into the second");

  transfer.send_file_packet(FILE_CLOSE);
}

/**
 * An abandoned transfer takes its partial file with it.
 *
 * A transfer that stops half way has written real blocks to a real directory entry, and what is
 * there is a fragment: a G-code file that ends mid-move, or worse, a firmware image that is
 * complete enough to be selected and short enough to brick the board. `transfer_abort()` removes
 * it rather than leaving it to be found.
 *
 * Asserted on the card rather than on the reply, because "PFT:success" is what an abort says
 * whether or not it removed anything.
 */
MARLIN_TEST(binary_stream, an_abandoned_transfer_removes_its_partial_file) {
  FreshTransfer transfer;

  transfer.send_file_packet(FILE_OPEN, open_payload("partial.gco"));
  transfer.send_file_packet(FILE_WRITE, "G28\nG1 X10");

  const std::string aborted = transfer.send_file_packet(FILE_ABORT);
  TEST_ASSERT_TRUE_MESSAGE(said(aborted, "PFT:success"), "an abort should be acknowledged");

  NoHostAttached quiet;
  card.mount();
  card.openFileRead("partial.gco");
  TEST_ASSERT_FALSE_MESSAGE(card.isFileOpen(),
    "an abandoned transfer should leave no file - a fragment that is complete enough to select "
    "and short enough to be wrong is worse than nothing");
}

/**
 * A sender asks what it is talking to before it commits to a transfer.
 *
 * QUERY is how the two ends agree on compression: the printer names its version and whether it
 * can decompress, and a sender that assumed either would corrupt the file rather than fail to
 * send it. The window and lookahead bits are part of the answer because heatshrink cannot decode
 * a stream packed with different ones.
 */
MARLIN_TEST(binary_stream, the_file_protocol_states_its_version_and_what_it_can_decompress) {
  FreshTransfer transfer;

  const std::string reply = transfer.send_file_packet(FILE_QUERY);

  TEST_ASSERT_TRUE_MESSAGE(said(reply, "PFT:version:0.1.0"),
    "a query should name the protocol version the sender must speak");
  TEST_ASSERT_TRUE_MESSAGE(said(reply, "compression:heatshrink"),
    "and whether it can decompress - a sender that guessed would corrupt the file rather than "
    "fail to send it");
}

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
