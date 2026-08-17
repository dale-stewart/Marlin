# Three files measured at once: what coverage did and did not predict

Part of the rescue log — see [README.md](README.md) for the index and `CLAUDE.md` for the rules
that apply to every session.

Three files were well covered and had **never been mutation tested**: no figure in this log, no
figure anywhere. They were run back to back on 2026-08-17 against `004-sd_powerloss`, one at a
time so the timings stay comparable, and the comparison turned out to be worth more than any of
the three numbers alone.

| file | lines | coverage | raw | by assertion | survivors |
|---|---:|---:|---:|---:|---:|
| `libs/numtostr.cpp` | 270 | 98% | **96.5%** | 94.7% (2085/2201) | 76 |
| `lcd/utf8.cpp` | 81 | 100% | **90.7%** | 77.7% (234/301) | 28 |
| `sd/SdVolume.cpp` | 145 | 75% | **55.1%** | 52.6% (340/646) | 290 |

## The prediction was wrong, and the way it was wrong is the finding

`numtostr.cpp` was picked first because it looked like the most likely embarrassment: 98% covered,
eight open register entries against it, no mutation figure. A high coverage number with nothing
behind it is the exact shape this whole exercise exists to expose.

It came back at **96.5%**, the best figure measured in this tree.

The three files do not differ by how much coverage they have — `utf8.cpp` is at 100% and scores
below `numtostr.cpp` at 98%, and `SdVolume.cpp` at 75% collapses. They differ by **how directly
what the code decides is observed**:

- `numtostr` is pure. A number goes in, a string comes out, and the tests assert the exact string.
  There is nowhere for a wrong decision to hide, and the register entries mean even the strange
  behaviours are pinned.
- `utf8` is a decoder with a little state. Mostly direct, and it sits in the middle.
- `SdVolume` is a stateful FAT allocator and block cache. Nothing observes it directly; everything
  is seen through a file operation several layers up, and most of what it decides never reaches an
  assertion at all.

**So the coverage-to-mutation gap is a property of the code's shape, not of its coverage.** That
refines the rule of thumb this project has been carrying — "expect the mutation score to be far
below the coverage number" — which is true of stateful code and roughly false of pure code with
exact assertions. It is also a scheduling rule: mutation runs are expensive, and they buy the
most on code whose output is observed indirectly.

## `SdVolume`: half the survivors are one missing input

151 of the 290 survivors are in `init()`, and the top lines are all the boot-sector sanity checks —
the partition validity test, `fatCount == 0`, `reservedSectorCount == 0`, and the arithmetic that
derives `dataStartBlock_` from `rootDirEntryCount`.

They survive for one reason: **`SimulatedMedia::format()` lays down exactly one volume, and it is
always valid.** Every test mounts it, so every check runs, and no check has ever been given
something to reject. Coverage sees a validator executing; mutation sees that removing it changes
nothing.

That is the taxonomy's "needs an input, not an assertion" category, and it is cheap to fix — the
fixture already builds the boot sector field by field through the firmware's own `fat_boot_t`, so
producing a malformed one is a parameter, not a new mechanism.

It also matters. A printer handed a corrupt or foreign card has to refuse it rather than act on
whatever geometry it read: `dataStartBlock_` derived from a bad `rootDirEntryCount` is a firmware
reading and writing arbitrary blocks of somebody's card.

The remaining clusters — `allocContiguous` (66), `fatPut` (32), `fatGet` (26) — are the allocator,
and they want the same thing: a card that is nearly full, or fragmented, rather than one with 8095
free clusters and nothing on it.

## Not done

No tests written from any of this. The three figures are the deliverable; the malformed-volume
fixture is the obvious next slice and is recorded here rather than started.
