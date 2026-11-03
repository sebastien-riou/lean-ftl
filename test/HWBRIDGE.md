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

## Run the test firmware on a board

With the prefix above:

```
hwbridge serial open nucleo-u5a5 --baud 115200
hwbridge flash nucleo-u5a5 build/boards/lean-ftl-u5a5.elf
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
  command line, and prints `Tests 1 to N PASSED` at the end. About 5 minutes on the U5.
- Without `HAS_PRINTF` the firmware prints nothing: the green LED blinks at about 1 Hz on a pass
  and fast on an error. Build with `HAS_PRINTF` as below for a readable run.
- A board run has no tearing simulation. It checks the layout, the pins, the tombstones and the
  record copies on real flash, through callbacks that check the flash's error flags; a torn
  program needs a power cut during one.
- On the NUCLEO-L552ZE-Q, whose probe is an ST-LINK/V2-1, a block of about 2 KB printed in the
  first half second after the core starts is lost, the same bytes at every flash or reset;
  probably the programmer detaching. The test suite prints slowly enough at that point; a
  firmware that prints at once should wait two seconds first.
- `flash` and `reset` take `--cat --until TEXT` to print the boot output in the same command.

## Build a test firmware that prints

The testbenches [lean-ftl-stm32u5](https://github.com/sebastien-riou/lean-ftl-stm32u5/) and
[lean-ftl-stm32l5](https://github.com/sebastien-riou/lean-ftl-stm32l5/), cloned beside this
repository, link the prebuilt `liblean-ftl.a` and `liblean-ftl-test.a` of a release from their
untracked `lean-ftl-dist/` (`./get-local-lean-ftl` fills it from `../lean-ftl`). Release builds
have no `HAS_PRINTF`, so `test.c` is silent on serial. For a readable run:

1. Build the two libraries with the define, from this repository. `buildit` names the build
   directory after the toolchain file, so the file must not sit directly in `build/`:

   ```
   mkdir -p build/boards
   printf 'include(%s/on/stm32u5)\nadd_definitions(-DHAS_PRINTF)\n' "$PWD" > build/boards/stm32u5-printf
   printf 'include(%s/on/stm32l5)\nadd_definitions(-DHAS_PRINTF)\n' "$PWD" > build/boards/stm32l5-printf
   export PATH=/opt/toolchains/arm-none-eabi-gcc/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin:$PATH
   ./buildit $PWD/build/boards/stm32u5-printf minSizeRel
   ./buildit $PWD/build/boards/stm32l5-printf minSizeRel
   ```

   The libraries are `build/stm32u5-printf/liblean-ftl/liblean-ftl.a` and
   `build/stm32u5-printf/liblean-ftl-test/liblean-ftl-test.a`, the same under `stm32l5-printf`.

2. Put the headers and libraries where the testbench's makefile looks, build the ELF and stage it
   under `build/boards/` (ignored by git, visible to the bridge and to a session):

   ```
   TB=../lean-ftl-stm32u5; T=stm32u5; B=build/stm32u5-printf; ELF=lean-ftl-u5a5.elf
   D=$TB/lean-ftl-dist
   mkdir -p $D/minSizeRel/include $D/liblean-ftl-test/include $D/minSizeRel/build/$T/liblean-ftl $D/minSizeRel/build/$T/liblean-ftl-test
   cp liblean-ftl/include/lean-ftl.h $D/minSizeRel/include/
   cp liblean-ftl-test/include/*.h $D/liblean-ftl-test/include/
   cp $B/liblean-ftl/liblean-ftl.a $D/minSizeRel/build/$T/liblean-ftl/
   cp $B/liblean-ftl-test/liblean-ftl-test.a $D/minSizeRel/build/$T/liblean-ftl-test/
   make -C $TB/minSizeRel clean all
   cp $TB/minSizeRel/$ELF build/boards/
   ```

   For the L552: `TB=../lean-ftl-stm32l5; T=stm32l5; B=build/stm32l5-printf; ELF=lean-ftl-l552.elf`,
   after step 3.
   From a session, which stays inside this repository, copy the testbench's tracked files
   (`git -C $TB ls-files`) to the scratchpad and build that copy; only the ELF goes to
   `build/boards/`.

3. L552 only: the serial port the bridge sees is the ST-LINK virtual COM port, which on this board
   is USART3 on PD8/PD9 (solder bridges moved), not the LPUART1 the project initialises at 209700
   baud; LPUART1 output never reaches the bridge. In `Core/Src/main.c`, which has CRLF line
   endings, insert this after `/* USER CODE BEGIN 0 */` and its `#include <stdbool.h>`, and call
   `MX_USART3_UART_Init();` on the line after `MX_LPUART1_UART_Init();`. The weak `_write` in
   `Core/Src/syscalls.c` calls `__io_putchar`. The U5 project already routes `printf` to COM1
   (`USE_COM_LOG`).

   ```c
   /* ST-LINK VCP of this board: USART3 on PD8/PD9 (solder bridges), 115200 8N1 */
   static UART_HandleTypeDef huart3;
   static void MX_USART3_UART_Init(void)
   {
     __HAL_RCC_USART3_CLK_ENABLE();
     __HAL_RCC_GPIOD_CLK_ENABLE();
     GPIO_InitTypeDef pins = {0};
     pins.Pin = GPIO_PIN_8 | GPIO_PIN_9;
     pins.Mode = GPIO_MODE_AF_PP;
     pins.Pull = GPIO_NOPULL;
     pins.Speed = GPIO_SPEED_FREQ_LOW;
     pins.Alternate = GPIO_AF7_USART3;
     HAL_GPIO_Init(GPIOD, &pins);
     huart3.Instance = USART3;
     huart3.Init.BaudRate = 115200;
     huart3.Init.WordLength = UART_WORDLENGTH_8B;
     huart3.Init.StopBits = UART_STOPBITS_1;
     huart3.Init.Parity = UART_PARITY_NONE;
     huart3.Init.Mode = UART_MODE_TX_RX;
     huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
     huart3.Init.OverSampling = UART_OVERSAMPLING_16;
     huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
     huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
     huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
     if (HAL_UART_Init(&huart3) != HAL_OK) Error_Handler();
   }
   int __io_putchar(int ch)
   {
     uint8_t c = (uint8_t)ch;
     HAL_UART_Transmit(&huart3, &c, 1, HAL_MAX_DELAY);
     return ch;
   }
   ```

## Measure zeros over zeros

[zeros-over-zeros.c](boards/zeros-over-zeros.c) measures whether the flash accepts all zeros
programmed over a write unit that already reads all zeros (FLASH.md, section 4, point 2). Build it
as the printing test firmware above, steps 2 and 3, with two more changes to the copy's
`Core/Src/main.c`: paste the file just before `/* USER CODE END 0 */`, and replace the lines
`test_callbacks();` and `test_main(0,NULL,NULL);` with `op2_measure();`. Then:

```
hwbridge flash nucleo-u5a5 build/boards/op2-u5a5.elf --cat --until "OP2 DONE" --timeout 300
```

It erases the last two pages of bank 2 before measuring, so `hwbridge reset nucleo-u5a5 --cat
--until "OP2 DONE"` runs it again. Its last lines are a summary per kind of program.

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

Tests 35, 39 and 40 need the tearing simulator and are empty on a board.
