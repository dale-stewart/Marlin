# Sending a file as bytes: `feature/binary_stream.h`

The framed packet protocol behind `M28 B1` and `CUSTOM_FIRMWARE_UPLOAD`. Opened, not closed.

Part of the rescue log — see [README.md](README.md) for the index and `CLAUDE.md` for the rules
that apply to every session.

## First slice (2026-08-14): 0% -> 30%

Third target from the [survey](survey-2026-08-14.md), and the largest untouched compiled file
left: 100 of 331 countable lines, with config `004-sd_powerloss` going 72.1% -> 72.7%.

**Why it is worth testing at all**, since the survey noted "nothing about it is dangerous" and that
was too generous: ordinary G-code upload is ASCII with a checksum per line, and this replaces it
with framed packets to save the encoding overhead. It is also what `CUSTOM_FIRMWARE_UPLOAD` sends a
*firmware image* over. A corrupted G-code line prints one bad layer; a corrupted firmware image
bricks the board. Every guarantee in the protocol exists to stop the second, and none of them had a
test.

**It tests well for the same reason `e_parser.cpp` did: the protocol publishes its own
vocabulary.** `ss` acknowledges a sync, `ok<n>` acknowledges packet *n*, and the failures are
named — `Packet header(n?) corrupt`, `Packet(n) payload corrupt`, `Datastream packet out of order`.
Those are the words a sender parses, so those are what the assertions compare against, rather than
anything the test computed for itself.

## Speaking the protocol

The tests frame packets byte by byte rather than asking the firmware to do it, which is the point:
a test that had the code under test build its own input would agree with any framing at all.

    token   uint16  0xB5AD, little-endian
    sync    uint8   sequence number
    meta    uint8   protocol in the high nibble, packet type in the low
    size    uint16  payload length
    cksum   uint16  Fletcher-16 over the four bytes above it — *not* the token

**That last line is the detail worth having written down**, because it is not obvious from the
struct and is stated nowhere: the running checksum starts at zero in `PACKET_HEADER`, which the
reader only enters *after* the token has matched, and it is snapshotted at
`bytes_received == sizeof(Header) - 2`. So the token contributes nothing and the checksum field
cannot cover itself. Getting this wrong produces packets the reader rejects, which looks exactly
like a defect in the reader.

Fletcher-16 is written in the test from the algorithm rather than copied from the reader. That is
unavoidable — a test cannot construct a valid packet without speaking the protocol — and it is why
the *assertions* are all on the published responses, where the reader has no say in what the test
expects.

## What the three tests say

- **A sync packet is answered from any state.** `SYNC` is deliberately the one packet whose
  sequence number is *not* checked, because a sender that has lost track cannot guess the right
  one — being answerable from an unknown state is the packet's whole purpose. So it is sent with a
  wildly wrong sync as well as a correct one, which is the case worth asserting rather than an
  aside.
- **A packet with a damaged header is refused, and named.** The header carries the payload length,
  so acting on a corrupted one would read the wrong number of bytes and swallow the packets behind
  it. The reader checks the header alone, before any payload arrives.
- **A packet with a damaged payload is refused, and refused *differently*.** Two checksums guard
  two different things and the reader distinguishes them in what it says: a bad header means it
  does not know how much is coming, a bad payload means it knows and what arrived is wrong. A
  sender uses the difference to decide what to resend, so collapsing the two messages would be a
  protocol change rather than a cosmetic one.

Each corruption flips exactly one checksum rather than sending noise, so a test names the guarantee
it exercises. **Verified by injection, and the useful part is that the two probes are
independent**: forcing the header check true fails only the header test, forcing the payload check
true fails only the payload test. Two tests that both merely detected "something was rejected"
would have failed together.

## Not done

Two thirds of the file. The untouched parts are the ones with side effects rather than answers:
`SDFileTransferProtocol` — open, write, close, abort, and the compressed-transfer path — which
needs a card and a file rather than a reply to assert on. Also untested: the resend and timeout
paths, and the `ok<n>` acknowledgement of a correctly received payload packet, which needs the
sequence number advanced deliberately across several packets.

No mutation measurement yet, so 30% is line coverage only and should not be read as more.
