# Boards through lean-hwbridge

The NUCLEO-U5A5ZJ-Q and NUCLEO-L552ZE-Q boards on the bench are reached through
[lean-hwbridge](https://github.com/sebastien-riou/lean-hwbridge): a daemon on the bench machine
gives power, serial, flash and reset to clients that cannot see the USB devices, such as a
sandboxed Claude Code session or another machine. Nothing else: no debugger. Every request is
printed in the daemon's terminal and kept in `~/.local/state/lean-hwbridge/audit.log`.

## Start the bridge

Outside any sandbox, in a terminal you keep open:

```
usbman serve &                      # unless it already runs as a service
usbman +all && usbman snapshot      # so that an off board can be switched on by name
hwbridge serve --dir ~/repos/lean-hwbridge/run                   # for sandboxed sessions
hwbridge serve --dir ~/repos/lean-hwbridge/run --confirm-flash   # same, asks before every flash
```

`hwbridge` here is the pipx install of a reviewed commit (lean-hwbridge's README, "Set up the
bench"), never the checkout under `~/repos`. The banner prints the pid: `kill -USR1 <pid>` cancels
the hardware operation in progress, the way out of a flash waiting for a board that never comes
back; `kill -USR2 <pid>` agrees to the audit log rotation when nobody answers in the terminal.

## Use it from a sandboxed session

The client needs nothing installed. Shell state does not survive between the commands of a
session, so prefix every command:

```
PYTHONPATH=/home/sru/repos/lean-hwbridge HWBRIDGE_DIR=/home/sru/repos/lean-hwbridge/run python3 -m hwbridge status
```

Options go after the subcommand (`status --dir ...`, not `--dir ... status`). Exit status 3 means
the bridge is not running. Boards are named by their usbman nickname: `nucleo-u5a5` and
`nucleo-l552` for lean-ftl; `nucleo-f207`, `numaker-m5531` and `pico2` are on the same bench.

## Run the test suite with lean-bsp

This repository is also a [lean-bsp](https://github.com/sebastien-riou/lean-bsp) application
(`app.toml`, `lean-bsp/lftl_lean_bsp.c`): it builds the library and the test suite from source,
with `HAS_PRINTF`, for any lean-bsp BSP offering an NVM region (contract 1.2), today
[`lean-bsp-nucleo-u5a5`](https://github.com/sebastien-riou/lean-bsp-nucleo-u5a5),
[`lean-bsp-nucleo-l552`](https://github.com/sebastien-riou/lean-bsp-nucleo-l552),
[`lean-bsp-pico2`](https://github.com/sebastien-riou/lean-bsp-pico2) (both targets) and
[`lean-bsp-numaker-m5531`](https://github.com/sebastien-riou/lean-bsp-numaker-m5531). The tests run
in the BSP's region 0: flash bank 2 on the Nucleo boards, the upper 2 MB of the QSPI flash on the
Pico 2, APROM bank 1 on the M5531 (on these two, 8-byte write units and no zero overwrite: the
no-pinning variant). With the BSPs cloned next to this repository, from it:

```
lbsp build-load-run --bsp nucleo-u5a5 --until PASSED --timeout 600
lbsp build-load-run --bsp nucleo-l552 --until PASSED --timeout 600
lbsp build-load-run --bsp pico2 --target cortex-m33 --until PASSED --timeout 900
lbsp build-load-run --bsp pico2 --target rv32imcb --until PASSED --timeout 900
lbsp build-load-run --bsp numaker-m5531 --until PASSED --timeout 600
```

From a sandboxed session, run `lbsp` as `python3 -m leanbsp` with lean-bsp and lean-hwbridge on
`PYTHONPATH` and `HWBRIDGE="python3 -m hwbridge"`. Each prints `Callbacks test PASS`, then
`Tests 1 to 45 PASSED`: in about 30 s on the U5A5 (lean-bsp runs it at 160 MHz), 1 minute on the
L552 and the M5531, and about 3 minutes on the Pico 2, on either core.
The flash accessors a board runs with are those of its BSP (`src/bsp_nvm.c`), the only copy there is.

## Flash and read by hand

`lbsp build-load-run` does these steps itself; by hand, with the prefix above and the ELF of a
lean-bsp build:

```
hwbridge serial open nucleo-u5a5 --baud 115200
hwbridge flash nucleo-u5a5 <the ELF> --timeout 300
hwbridge serial cat nucleo-u5a5 --from GEN:0 --until PASSED --timeout 600
```

- Open the serial port before flashing, so that the boot output is captured from its first byte.
  `status` then shows the port `open ttyACMn`; `waiting` means the bridge sees no tty for the
  board (see "When a flash hangs").
- `--timeout` on `flash` cancels the flash on the daemon when it expires. A flash takes well under
  a minute, so 300 seconds only stops a hung one. From a session, run it in the foreground with a
  tool timeout above that: the session's auto mode refused it as a background command.
- `flash` prints the `serial cat ... --from GEN:OFFSET` that starts at the boot; the same text is
  in `/home/sru/repos/lean-hwbridge/run/serial/<board>/<GEN>.log`.
- The firmware runs the suite once, in the pinning variant its accessors declare, without a
  command line, and prints `Tests 1 to N PASSED` at the end.
- A board run has no tearing simulation. It checks the layout, the pins, the tombstones and the
  record copies on real flash, through callbacks that check the flash's error flags; a torn
  program needs a power cut during one.
- On the NUCLEO-L552ZE-Q, whose probe is an ST-LINK/V2-1, a block of about 2 KB printed in the
  first half second after the core starts is lost, the same bytes at every flash or reset;
  probably the programmer detaching. The test suite prints slowly enough at that point; a
  firmware that prints at once should wait two seconds first.
- `flash` and `reset` take `--cat --until TEXT` to print the boot output in the same command.

## The measurement programs and the retired testbenches

Until 2026-11-24 the boards were also run through two testbench repositories,
[lean-ftl-stm32u5](https://github.com/sebastien-riou/lean-ftl-stm32u5/) and
[lean-ftl-stm32l5](https://github.com/sebastien-riou/lean-ftl-stm32l5/), which linked the
`stm32u5` and `stm32l5` libraries of a release. Those libraries no longer exist: the accessors
they held (`target/stm32`) are in the BSPs only, and the route is retired. Its steps are in this
file as of commit df94e3a (`git show df94e3a:test/HWBRIDGE.md`).

The three measurement programs below were built on that route, each pasted into the `main.c` of a
copy of a testbench. They drive the flash controller themselves and need nothing of lean-ftl,
only `printf`, so running one again means building it as a lean-bsp application on `nucleo-u5a5` or
`nucleo-l552`, as the timing firmware of the M5531 was (FLASH.md, 2.3). The build steps quoted
below are those of the testbench route, kept as the record of how the figures were obtained.

## Measure zeros over zeros

[zeros-over-zeros.c](boards/zeros-over-zeros.c) measures whether the flash accepts all zeros
programmed over a write unit that already reads all zeros (FLASH.md, section 4, point 2). It was built
as the printing test firmware of the testbench route, with two more changes to the copy's
`Core/Src/main.c`: paste the file just before `/* USER CODE END 0 */`, and replace the lines
`test_callbacks();` and `test_main(0,NULL,NULL);` with `op2_measure();`. Then:

```
hwbridge flash nucleo-u5a5 build/boards/op2-u5a5.elf --cat --until "OP2 DONE" --timeout 300
```

It erases the last two pages of bank 2 before measuring, so `hwbridge reset nucleo-u5a5 --cat
--until "OP2 DONE"` runs it again. Its last lines are a summary per kind of program.

## Measure an interrupted erase

[torn-erase.c](boards/torn-erase.c) measures what a page erase interrupted by a system reset
leaves on an STM32U5A5, on a programmed page and on a page that was already erased (EWLF.md,
"Physics the design relies on"). It was built as the printing test firmware of the testbench
route, with three more changes to the copy of the testbench: paste the file into
`Core/Src/main.c` just before `/* USER CODE END 0 */`; replace the lines `test_callbacks();` and
`test_main(0,NULL,NULL);` with `te_measure();`; and in `Core/Src/stm32u5xx_it.c` replace the
endless loop of `NMI_Handler` with `void te_nmi(void); te_nmi();`, since a read of a weak cell may
raise the ECC NMI. Then:

```
hwbridge serial open nucleo-u5a5 --baud 115200
hwbridge flash nucleo-u5a5 build/boards/torn-erase-u5a5.elf --cat --until "TORN-ERASE DONE" --timeout 600
```

The run resets the board about 160 times by itself and takes under two minutes. It prints one
line per trial, a table (`TE ROW`), a summary and a verdict (`TE VERDICT`), then `TORN-ERASE
DONE`. It uses the last two pages of bank 2, about 330 erases of the last one per run. The
flash erases the whole chip, which is what starts a fresh run: once a run is complete a reset
only prints its summary again.

[erase-time.c](boards/erase-time.c) times a page erase against what the page holds. It is built
the same way, without the change to the NMI handler, with `et_measure();` as the call, and run
with `--until "ERASE-TIME DONE"`. It uses about 85 erases of the last page of bank 2 and runs
again at every reset.

## When a flash hangs

The STM32 flash power-cycles the board and then waits, without a timeout, for the board's serial
port to come back before it runs the programmer. A board whose port never appears hangs the flash
with no output, and `status` shows the port `waiting` with no tty. Cancel it with `hwbridge cancel
<request id>`, the id the daemon's terminal prints, or `kill -USR1 <pid>`.

The usual cause is the host's `cdc_acm` driver. In a plain terminal:

```
lsmod | grep cdc_acm
ls -l /dev/ttyACM*
sudo modprobe cdc_acm
```

`modprobe: ERROR: could not insert 'cdc_acm': Exec format error` means the kernel modules on disk
were replaced while the machine was running, typically by a release upgrade, and the running
kernel refuses any module it has not loaded already. Only a reboot fixes it; after it, start
`usbman serve` and `hwbridge serve` again.

## Board runs

Test suite runs are recorded in [THREAT-MODEL.md](../THREAT-MODEL.md), section "Verification
plan", and measurements in [FLASH.md](../FLASH.md), section 4:

| Date | Sources | Result |
|---|---|---|
| 2026-10-31 | v0.2.0 | tests 1 to 37 pass on both boards |
| 2026-11-03 | EWLF format of v0.3.0, commit 760da0c | tests 1 to 40 pass on both boards |
| 2026-11-03 | `boards/zeros-over-zeros.c` | all zeros over all zeros refused with PROGERR on both boards, 48 of 48 |
| 2026-11-14 | `boards/torn-erase.c` | NUCLEO-U5A5ZJ-Q: an erased page stays erased through an interrupted erase, 40 of 40 |
| 2026-11-14 | `boards/erase-time.c` | NUCLEO-U5A5ZJ-Q: an erase takes 881 us plus 0 to 4 steps of 226 us, set by the content, not by how much of the page is programmed |
| 2026-11-15 | EWLF format of v0.4.0, branch `ewlf-no-rollback`, commit 006f085 and the sources before it | tests 1 to 40 pass on the NUCLEO-U5A5ZJ-Q and on the NUCLEO-L552ZE-Q |
| 2026-11-22 | large areas and the table form of the CRC-64, branch `chunked-area`, commit cd3d0fa | tests 1 to 45 pass on the NUCLEO-U5A5ZJ-Q, in 6 min 47 s, and on the NUCLEO-L552ZE-Q, in about 1 min |

Tests 35, 39, 40 and 45 need the tearing simulator and are empty on a board.
