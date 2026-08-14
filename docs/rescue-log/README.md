# The rescue log

What running the `legacy-rescue` skill on this firmware actually taught, per target.

`CLAUDE.md` is the entry point and holds only what applies to *every* session: what the fork is
for, the cover-first rule, how to run the measurements, and the gotchas that have cost real time.
This directory holds what applies to *one target*, and is meant to be read when that target comes
up rather than loaded every time.

**Read the relevant file before measuring or writing tests against a target it covers.** Several
entries exist precisely because a figure was quoted wrong, a wall was reported that was not
there, or a test was written against a fixture note that had stopped being true.

| File | What is in it |
|---|---|
| [assertion-examples.md](assertion-examples.md) | The worked examples behind the skill's abstract assertion rules |
| [planner-settings.md](planner-settings.md) | The blocked `planner.settings` correction and the state of its consumers |
| [gcode-parser-and-queue.md](gcode-parser-and-queue.md) | `gcode.cpp`, `queue.cpp`, and sizing the blocked `GCodeParser` correction |
| [temperature.md](temperature.md) | `temperature.cpp`, autotune, autotemp, and the thermal configurations |
| [display-drivers.md](display-drivers.md) | The DWIN driver and `marlinui.cpp` |
| [core-and-commands.md](core-and-commands.md) | `MarlinCore.cpp`, `kill()`, and the individual command files |
| [agents.md](agents.md) | What validating each subagent taught |
| [embedded-platforms.md](embedded-platforms.md) | What emulating the embedded targets would cost |
| [survey-2026-08-14.md](survey-2026-08-14.md) | What is left, measured — the evidence base for choosing a target |

## Where a finding goes

Three places, and the split is deliberate:

- **The skill** (`.claude/skills/legacy-rescue/`) gets the transferable principle, stated so that
  it would make sense to someone who has never seen this firmware. It names no G-code command, no
  firmware symbol, and not this project.
- **This log** gets the concrete instance — the file, the line, the number, the mistake.
- **`docs/defect-register.md`** gets behaviour that was found and recorded rather than changed:
  genuine defects, deliberate design limits, and blocked design corrections, each pinned by a
  test.

A finding that only appears in a commit message is a finding that will be learned again.

## Reading a figure from here

Two cautions that apply to every number in this directory:

- **Say which configuration it was measured in.** Several targets are not compiled at all in the
  default build, and a file reading 0% because nothing compiles it looks exactly like a file
  nobody tested. Any figure without a configuration beside it means nothing.
- **A mutation score is comparable only to another over the same covered-line set.** Adding tests
  changes the population, so two runs of the same file are not directly comparable unless the
  covered-line count matches.
