# Emulating the embedded platforms: what it would actually take

Every wall here that is not a testing problem is the same wall — code that cannot be
compiled or run for a 64-bit host. Staged, with costs, and none of it done.

Part of the rescue log — see [README.md](README.md) for the index and
`CLAUDE.md` for the rules that apply to every session.

### Emulating the embedded platforms: what it would actually take

Every wall here that is *not* a testing problem is the same wall — code that cannot be compiled
or run for a 64-bit host: `sovol_rts`, `creality/dwin`, `mks_ui`, `M485.cpp`, `mmu3.cpp`.

**The important finding is that most of those are not hardware dependencies.** They are
toolchain and type-model differences. `sovol_rts` fails because `int32_t` is `int` on x86-64
and `long` on arm-none-eabi, which turns two distinct overloads into one signature with two
bodies (register #36). `dwin` and `M485` fail on missing Arduino headers. None of that needs a
CPU emulated; it needs the target's *compiler*.

So the work is staged, and the stages are wildly different in cost. Nothing below is installed
on this machine — checked: no `qemu-system-arm`, no `qemu-system-avr`, no `arm-none-eabi-gcc`,
and only the `native` PlatformIO platform.

**Stage 0 — done, and it ended the sovol question rather than advancing it.** Both toolchains
install into user space with no root: `pio pkg install -g -p atmelavr` and
`-t platformio/toolchain-gccarmnoneeabi`. The type-model story is confirmed exactly —
`int32_t` is `long` and `int` is 16-bit on AVR, so `sendData(int)` collides with the
**`int16_t`** overload there, while on x86-64 it collides with the **`int32_t`** one. The set is
well-formed only where `int` is 32-bit and `int32_t` is `long`, which is arm-none-eabi and
nothing else.

But cross-compiling the real `GD32F103RET6_sovol_maple` environment found something that makes
that moot: **`sovol_rts.cpp` references four identifiers that do not exist anywhere in the tree**
— `MARLINVERSION`, `MACVERSION`, `SOFTVERSION`, `CORP_WEBSITE_E` — so it cannot compile in any
configuration on any platform. Register #37. It is dead code, the defect recorded against it in
#33 cannot run on a machine built from this tree, and **no emulator helps**: undefined
identifiers fail at compile time, and QEMU has nothing to run.

The other two were then checked the same way, and the three drivers turn out to be in three
different states:

| driver | cross-compiled for its target | state |
|---|---|---|
| `lcd/sovol_rts` | **cannot, ever** — four undefined identifiers | dead code (#37) |
| `lcd/dwin/creality` | **builds clean** — `STM32F103RE_creality`, flash 25.5%, RAM 11.2% | live, and testable in principle |
| `lcd/extui/mks_ui` | **builds clean** — `mks_robin_nano_v1v2`, flash 56.7%, RAM 74.8% | live; register #34 now confirmed |

`mks_ui` took two goes, and the second one only worked after reading `Conditionals-2-LCD.h`
instead of guessing. Two traps, both worth knowing. **`TFT_LVGL_UI` is not something you set** —
it is auto-defined by the legacy path from `TFT_LVGL_UI_FSMC` or `TFT_LVGL_UI_SPI`, and setting
it directly alongside a panel is what pushes `LCD_ENABLED_COUNT` past one. And
**`TFT_RES_480x320` lives inside Configuration.h's `#if ENABLED(TFT_GENERIC)` block**, which is
evaluated *before* Conditionals auto-defines `TFT_GENERIC` — so the resolution silently does not
apply unless `TFT_GENERIC` is also set explicitly, and the build then insists on a resolution
that is right there in the file.

**Both live drivers had their recorded suspicion confirmed by their own object files**, which is
the cheapest form of confirmation available and needed no emulator at all. #33 for `dwin`: uses
`set_max_feedrate` and `set_max_acceleration`, writes steps-per-millimetre raw, references no
`refresh` symbol. #34 for `mks_ui`: references `refresh_positioning` but no
`refresh_acceleration_rates`, and writes `max_acceleration_mm_per_s2` raw — it remembers the
refresh derived from resolution and forgets the one derived from acceleration.

So the emulator was never what these two needed. A compiler that accepts the file and a symbol
table settled both.

**Stage 1 — `qemu-user`, not `qemu-system`.** A test binary cross-compiled for 32-bit ARM Linux
and run under `qemu-arm` gives a genuine 32-bit type model, real execution, and the existing
test HAL, without modelling a board at all. Far cheaper than system emulation. Caveat worth
checking before betting on it: glibc on ARM may define `int32_t` as `int`, in which case this
stage does *not* reproduce #36 even though it does reproduce pointer width and alignment.

**Stage 2 — `qemu-system-arm` with a Cortex-M machine and semihosting.** The only stage that
exercises the real platform HAL — timers, USART, SPI. Also the most fragile: QEMU's STM32
models implement a subset of peripherals, and Marlin's init touches many, so the likely first
result is a hang in `setup()` rather than a running firmware. A project, not a task.

**And the instrument problem applies to the emulator itself.** An emulator is a measuring
device, and this fork's whole discipline says an unvalidated measurement is a rumour. QEMU's
peripheral models are approximations; a test that passes under emulation and would fail on
hardware is exactly the self-consistent wrong measurement this workflow exists to catch. Any
QEMU result needs the same treatment as any other harness — inject a fault, prove it can fail,
and reproduce a known result before trusting a new one.
