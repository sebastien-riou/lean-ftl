# lean-ftl - flash memory assumptions

**Status: reference.** This file states what lean-ftl assumes of the flash it runs on and checks
each assumption against the vendor documentation of four targets, read on 2026-10-25. It describes
the hardware as documented; the requirements and assumptions themselves are stated, numbered R, N
and T, on the documentation page
[Flash requirements and assumptions](https://lean-ftl.readthedocs.io/en/latest/flash.html), and
what lean-ftl does with them is in `README.md`, `EWLF.md` and `THREAT-MODEL.md`. Where a document
says nothing, the table says so rather than guessing; the guesses that remain are listed in §4 as
things to measure. The documents themselves are not in the repository (`git_ignore/downloads/`).

## Documents read

| Target | Document | Revision | Parts read |
|---|---|---|---|
| STM32U5 (NUCLEO-U5A5ZJ-Q, STM32U5A5ZJ, 4 MB) | RM0456, STM32U5 series reference manual | Rev 6 | §7 Embedded flash memory: 7.2, 7.3.1, 7.3.2, 7.3.5 to 7.3.9, 7.3.11, 7.3.12, 7.9.12 |
| | ES0553, STM32U59xxx/5Axxx device errata | Rev 4, April 2024 | the flash item, 2.2.12 |
| | PM0264, STM32 Cortex-M33 programming manual | Rev 3 | no flash content; not used |
| | UM2861, STM32U5 Nucleo-144 (MB1549) user manual | Rev 8 | board only; no flash content |
| STM32L5 (NUCLEO-L552ZE-Q, STM32L552ZE, 512 KB) | RM0438, STM32L5 series reference manual | Rev 8 | §6 Embedded flash memory: 6.2, 6.3.1, 6.3.2, 6.3.5 to 6.3.8 |
| M5531 (NuMaker-M5531, 1 or 2 MB APROM) | M5531 series technical reference manual | Rev 1.02 | §6.8 Flash memory controller (FMC): 6.8.2 to 6.8.4, ISP command list (table 6.8-2), 6.8.4.16, 6.8.4.18, FMC_ISPCTL/FMC_ISPSTS |
| Pico 2 (RP2350A + Winbond W25Q32RV, 4 MB) | Raspberry Pi Pico 2 datasheet | release 5 | the flash statements, §5.1 |
| | W25Q32RV_DTR datasheet | Rev H, December 15, 2025 | organization, page program, erases, suspend, reset, power-up, AC characteristics |

Not covered: the CH32V307 (a testbench and `target/ch32v307/` exist, no document in the folder) and
the RP2350's own QSPI/XIP controller (the RP2350 datasheet is not in the folder; on the Pico 2 the
SDK does the programming, see §2.4).

## 1. Summary

"doc" = stated in the document read; "—" = the document read says nothing; "n/a" = no such
feature. Details and quotes in §2, what lean-ftl makes of it in §3.

| | STM32U5 | STM32L5 | M5531 | Pico 2 (W25Q32RV) |
|---|---|---|---|---|
| Flash | 4 MB on-chip, 2 banks, read-while-write | 512 KB on-chip, 2 banks (DBANK=1), read-while-write | 1 or 2 MB APROM on-chip, 2 banks | 4 MB QSPI NOR, external, XIP |
| Erased value | 0xFF | 0xFF | 0xFF ("all one") | 0xFF |
| Erase unit | 8 KB page | 2 KB page (DBANK=1); 4 KB (DBANK=0) | 8 KB page | 4 KB sector |
| Larger erases | bank, mass | bank, mass | bank (APROM, no XOM), mass | 32 KB and 64 KB block, chip |
| Program unit | 16 B quad-word (+9 ECC bits); burst of 8 quad-words (128 B) | 8 B double word (+8 ECC bits) | 4 B word; also 8 B and multi-word (8 to 256 or 512 B, see §2.3) | 1 to 256 B within a 256 B page |
| Smaller writes | refused (SIZERR, PGAERR) | refused (SIZERR, PGAERR) | — (32 bits is the minimum) | 1 B allowed |
| Re-programming a programmed unit | refused (PROGERR) unless the data is all zeros | same | — | outside the spec: program is defined for "previously erased (FFh)" locations only |
| All zeros over a partially programmed unit | doc: allowed | doc: allowed | — | — |
| ECC | 9 bits per 128 bits: 1-bit correction, 2-bit detection | 8 bits per 64 bits: 1-bit correction, 2-bit detection | none mentioned | none |
| Double-bit error | NMI (ECCD) | NMI (ECCD, ECCD2) | n/a | n/a |
| Erased line and ECC | 2-bit detection not supported on an erased line | same | n/a | n/a |
| Hardware "is erased" | no | no | yes: Flash all one verification (ISP 0x28, 0x08) | no |
| Margin read, or another check that a programmed word is not weak | no margin read (RM0456 §7); ECC correction flag ECCC with ADDR_ECC (§7.9.11), one-sided: a hit means a cell has crossed, no hit proves nothing; FLASH_OPSR is cleared by a brownout reset (§11.3.1), so useless after a power cut | no margin read (RM0438 §6); ECC correction flag ECCC with ADDR_ECC (§6.3.2), one-sided | none documented; the all one verification only checks that a range reads blank after an erase (§6.8.4.18) | none: no ECC, no failure status bit, BUSY only says the operation ended |
| Interrupted operation | reset: content "not guaranteed", torn location "may no longer be writable", page erase mandatory; FLASH_OPSR records the operation across a system reset, not a power-on | reset: content "not guaranteed" | ISPFF on "Erase or Program command at brown-out detected" | reset or power-off while suspended: "data corruption may happen" |
| Write inhibit at power-up | — | — | — | below VWI (1.0 to 2.0 V) and for tPUW (5 ms) after power-up |
| Suspend/resume | n/a | n/a | n/a | yes (75h, 7Ah), tSUS 20 µs |
| Endurance | 10 kcycles; 100 kcycles on at most 32 pages (256 KB) per bank | — (datasheet) | — (datasheet) | min. 100 K program-erase cycles |
| Retention | — | — | — | more than 20 years |
| Program time | — | — | — | tPP 0.25 ms typ., 2 ms max (page) |
| Erase time | — | — | — | tSE 30 ms typ., 240 ms max (4 KB sector) |
| Errata read | ES0553 2.2.12: programming stuck after a sequence error while WDW is set | none in the folder | none in the folder | none |
| lean-ftl today | `write_size` 16, `erase_size` 8 KB, `target/stm32/stm32u5-accessors.c` | `write_size` 8, `erase_size` 2 KB, `target/stm32/stm32l5-accessors.c` | no accessor yet | no accessor yet |

## 2. Per target

### 2.1 STM32U5 (RM0456 Rev 6, §7)

- **Organization (7.3.1).** Two banks of up to 2 MB, 256 pages of 8 KB each; "137 bits wide data
  read and write (128 effective bits plus 9 ECC bits)"; read-while-write between banks (7.3.10),
  a read of the bank being programmed stalls the bus until the operation ends (7.3.5); bank
  swap by option byte (the accessor reads `SWAP_BANK` to address the right page). 512 bytes of OTP
  (32 quad-words), "cannot be erased and can be written only once".
- **Programming (7.3.7).** "The flash memory is programmed 137 bits at a time (128-bit data +
  9 bits ECC). Programming in a previously programmed address is not allowed except if the data
  to write is full zero, and any attempt sets PROGERR flag". Quad-word only: a byte or half-word
  write sets SIZERR, a misaligned quad-word PGAERR. The four 32-bit words go through a write buffer
  (WDW flag); programming starts when the fourth arrives, so "the software must ensure that the
  four words in the same quad-word are all written" (7.3.5); to program less, "the quad-word must be
  completed with the erase value 0xFFFF FFFF". "ECC is calculated from the quad-word to program."
  Burst mode programs 8 quad-words (128 bytes) at an address multiple of 0x80. "No option bytes
  modification nor erase request is allowed when WDW bit is set."
- **PROGERR (7.3.9)** is set when the word to program points to an address "not previously
  erased", "already fully programmed to 0", or "already partially programmed (contains 0 and 1)
  and the new value to program is not full zero". Read with 7.3.7: an erased quad-word takes any
  value, a partially programmed one takes all zeros only. The second bullet names a fully zero
  quad-word as an error without the "unless full zero" exception; whether a second all-zero
  program of an all-zero quad-word is accepted or flagged is therefore not settled by the manual
  (§4, point 2). An error aborts the operation and leaves the flags set; the next operation must
  clear them first or gets PGSERR.
- **ECC (7.3.2).** One corrected bit sets ECCC (interrupt if ECCCIE); two detected bits set ECCD
  "In this case, an NMI is generated." "Caution: When the ECCC flag is set, a further two-errors
  detection is not able to generate the NMI or break signal to timers. It is therefore recommended to
  clear the ECCC flag as soon as a correction is operated". After an ECCD "the software must clean
  the cache in the NMI handler". "For an erased flash line, one error is detected and corrected but
  two errors detection is not supported." "When an ECC error is reported, a new read at the failing
  address may not generate an ECC error if the data is still present in the current buffer". Two
  system-flash words carry a 1-bit and a 2-bit error for testing (0x0BFA1F00, 0x0BFA1F80).
- **Erase (7.3.6).** Page (PER, PNB, BKER, STRT), bank (MER1, MER2), mass; none while WDW is set.
- **Interrupted operation (7.3.11, 7.3.12).** "The contents of the flash memory currently being
  accessed are not guaranteed if a power-down occurs during a flash memory program or erase
  operation." Same for a reset, and after a *system* reset FLASH_OPSR (7.9.12) tells which
  operation was interrupted (CODE_OP: single write, burst write, page, bank or mass erase, option
  change), in which bank and at which address; table 56 gives the corrective action, and its note:
  "For single and burst write, it is mandatory to perform a page erase because the current flash
  memory locations may no longer be writable." FLASH_OPSR reads 0 after a power-on reset, so it
  records nothing across a power cut; it covers NRST, watchdog and software resets.
- **Endurance (7.2, 7.3.8).** "10 kcycles endurance on all flash memory. 100 kcycles on up to
  256 Kbytes per bank": "Any flash page can be chosen to be cycled up to 100 kcycles. As soon as a
  page is above 10 kcycles, it is considered as high cycling page", and "the application must take
  care not to exceed 32 pages cycled more than 10 000 times" (on STM32U535/545 the whole flash is
  100 kcycle capable). Retention and timings are in the datasheet, not in RM0456.
- **Errata (ES0553 Rev 4, 2.2.12).** "If an operation is started while the write buffer is
  waiting for the next data (STRT or OPTSTRT is set while WDW is already set), the PGSERR flag is
  set and the programming sequence is expected to be aborted. However, the WDW bit remains set and
  the flash programming operation remains stuck, waiting for the second word to be written."
  Workaround: write the next word. The lean-ftl accessor writes complete quad-words with interrupts
  masked and never starts an erase while a program is pending, so it does not reach this state; a
  second flash driver running from an interrupt could.
- **lean-ftl.** `target/stm32/stm32u5-accessors.c` with `LFTL_WU_SIZE` 16 and 8 KB pages
  (`liblean-ftl-test/include/type.h`): `nvm_ll_write` feeds the write buffer 32-bit word by word
  and waits for BSY, interrupts disabled, nonsecure registers, then verifies by reading back
  (`memcmp`); `nvm_erase` erases page by page.

### 2.2 STM32L5 (RM0438 Rev 8, §6)

- **Organization (6.3.1).** DBANK=1: two banks of 128 pages of 2 KB, "72 bits wide data read
  (64 bits plus 8 ECC bits)", bank and mass erase, read-while-write (6.3.9); DBANK=0: one bank of
  128 pages of 4 KB, 144-bit reads (2 x (64 + 8)), mass erase only, no read-while-write. The
  accessor hard-codes 2 KB pages, so lean-ftl on this part requires DBANK=1. OTP: "If only one bit
  is at 0, the entire double word cannot be written anymore, even with the value 0x0000 0000 0000
  0000", the opposite of the main memory rule below.
- **Programming (6.3.7).** "The flash memory is programmed 72 bits at a time (64 bits + 8 bits
  ECC). Programming in a previously programmed address is not allowed except if the data to write
  is full zero, and any attempt sets the NSPROGERR or SECPROGERR flag". Double word only (SIZERR,
  PGAERR otherwise); two 32-bit writes, the second launches the programming; one word alone is
  completed "with the erase value 0xFFFF FFFF"; "ECC is calculated from the double word to
  program." RM0438 has no burst or fast programming mode.
- **PROGERR (6.3.8).** The same three bullets as the STM32U5, with the same open question on an
  all-zero target.
- **ECC (6.2, 6.3.2).** DBANK=1: per double word, one correction (ECCC) and two detections (ECCD,
  "In this case, a NMI is generated"); DBANK=0: the same per 64-bit half of the 128-bit line,
  with ECCC2 and ECCD2 for the upper half. "For a virgin data: 0xFF FFFF FFFF FFFF FFFF, one error
  is detected and corrected but two errors detection is not supported." The same re-read caveat
  as the STM32U5.
- **Interrupted operation (6.3.5).** "The contents of the flash memory are not guaranteed if a
  device reset occurs during a flash memory program or erase operation." No operation status
  register. Erase and program need voltage scaling range 0 or 1 (range 2 is the reset default).
- **Endurance, retention, timings.** Not in RM0438 (datasheet, not in the folder).
- **lean-ftl.** `target/stm32/stm32l5-accessors.c`, `LFTL_WU_SIZE` 8, 2 KB pages, same structure
  as the STM32U5 accessor.

### 2.3 M5531 (TRM Rev 1.02, §6.8)

- **Organization (6.8.2, 6.8.3, 6.8.4.1).** Dual-bank APROM of 1024 or 2048 KB (512 or 1024 KB per
  bank), 8 KB LDROM, 3 KB OTP, user configuration block, XOM setting pages. "The page erase Flash
  size is 8 Kbytes, and minimum program bit size is 32 bits." "Supports 8 Kbytes page erase for all
  embedded Flash", "Supports bank erase for APROM, except if XOM regions is set".
- **Programming (6.8.4.16, table 6.8-2).** Every operation is an ISP command: FMC_ISPCMD,
  FMC_ISPADDR, FMC_ISPDAT, then ISPGO, polled until done, ISPFF checked. "Flash 32-bit Program"
  (0x21), "Flash 64-bit Program" (0x61), "Flash Multi-Word Program" (0x27): "the maximum programming
  length is up to 512 bytes, and the minimum programming length is 8 bytes (2 words)", while
  figure 6.8-14 says "8 ~ 256 bytes programming (8 bytes align)"; the two disagree on the maximum.
  During multi-word programming the firmware "has to be located in embedded SRAM of chip to avoid
  CPU hold". "When FMC is doing ISP command, user cannot enter any Power-down mode".
- **Erase.** Page erase 0x22 (8 KB aligned, within one bank), bank erase 0x23, mass erase 0x26
  (if MERASE is enabled), CFG word erase 0x24.
- **Verification helpers.** "Run Flash All One Verification" (0x28) then "Read Flash All One
  Result" (0x08, 0xA11F_FFFF means all one) on an 8 KB aligned range within one bank (6.8.4.18);
  CRC32 over an 8 KB aligned range (0x2D, 0x0D; 6.8.4.17).
- **Failure flag.** ISPFF (FMC_ISPCTL[6]) is set for a disabled update (APUEN, LDUEN, CFGUEN at 0),
  "Erase or Program command at brown-out detected", an illegal address, an invalid command, the
  XOM and OTP rules, "ISP conflict error". Nothing about the current content of the target
  address: the TRM neither allows nor refuses programming a word that is already programmed, and
  does not mention an ECC. The OTP has a lock bit after which a word "cannot be programmed again,
  regardless its content is all 1 or not".
- **Endurance, retention, timings.** Not in the TRM (datasheet, not in the folder).
- **lean-ftl.** No accessor yet (`lean-bsp-numaker-m5531` declares no NVM region). A port would use
  `write_size` 4 (or 8 with the 64-bit command) and `erase_size` 8 KB; the all-one verification
  could back `is_erased()`. The re-programming behaviour must be measured first (§4, point 1).

### 2.4 Pico 2: RP2350A and W25Q32RV (Pico 2 datasheet release 5, W25Q32RV Rev H)

- **The board.** The Pico 2 datasheet only names the part: "RP2350A microcontroller with 4 MB
  flash", "External Quad-SPI flash with eXecute In Place (XIP) and 16 kB on-chip cache", "flash
  (Winbond W25Q32RV)"; §5.1 covers reprogramming over SWD or USB mass storage. The RP2350 datasheet
  is not in the folder. With the pico-sdk the programming path is `flash_range_erase` (multiples of
  4 KB) and `flash_range_program` (multiples of 256 bytes), run from RAM with XIP paused; that is
  SDK knowledge, not something the documents read say.
- **Organization.** "16,384 programmable pages of 256-bytes each. Up to 256 bytes can be programmed
  at a time. Pages can be erased in groups of 16 (4KB sector erase), groups of 128 (32KB block
  erase), groups of 256 (64KB block erase) or the entire chip (chip erase)."
- **Page Program (02h), Quad Page Program (32h).** "from one byte to 256 bytes (a page) of data to
  be programmed at previously erased (FFh) memory locations"; the address wraps inside the page; a
  partial page can be programmed "without having any effect on other bytes within the same page".
  Programming a byte that does not read FFh is outside the datasheet: NOR cells only go from 1 to 0,
  but Winbond does not specify the result. Every program and erase needs a Write Enable (WEL),
  which clears itself after the operation and at power-up.
- **Sector Erase (20h).** "sets all memory within a specified sector (4K-bytes) to the erased
  state of all 1s (FFh)"; block erases 52h and D8h, chip erase C7h/60h.
- **Suspend (75h) and resume (7Ah).** tSUS 20 µs. "Unexpected power off during the Erase/Program
  suspend state will reset the device and release the suspend state. [...] The data within the
  page, sector or block that was being suspended may become corrupted."
- **Reset during an operation.** /RESET pin or the 66h/99h sequence: "Any on-going Program/Erase
  operation will be interrupted and data corruption may happen."
- **Power-up and power-down (9.3).** The device is held in reset while VCC is below VWI (1.0 to
  2.0 V); once above it, "all program and erase related Instructions are further disabled for a
  time delay of tPUW" (5 ms max); tVSL 20 µs from VCC(min) to the first /CS; a power cycle must take
  VCC below VPWD (1.2 V) for tPWD (100 µs).
- **AC characteristics.** tPP 0.25 ms typ., 2 ms max; tSE 30 / 240 ms; tBE1 (32 KB) 80 / 800 ms;
  tBE2 (64 KB) 120 / 1200 ms; tCE 6 / 40 s; tW (status register) 1.5 / 15 ms.
- **Endurance and retention.** "Min. 100K Program-Erase cycles", "More than 20-year data
  retention". No ECC.
- **lean-ftl.** No accessor yet (`lean-bsp-pico2` declares no NVM region). A port would use
  `erase_size` 4 KB and a `write_size` between 1 (the datasheet's unit) and 256 (the SDK's unit);
  with the SDK as is, a 4-byte meta-data item costs a 256-byte page program, so a driver doing
  partial page programs is worth having.

### 2.5 The Linux simulator

`target/linux/linux-accessors.c` models the STM32 rule: a write unit is accepted only while it reads
all 0xFF, or when the data is all zeros (lines 121-126, "like STM32U5-class flash"); an erase sets
0xFF; the tearing simulator in `liblean-ftl-test/source/test.c` (`tearing_sim_nvm_write`) refuses
the same writes so that tests only build states a real flash can hold. The geometry comes from
`on/linux` and `testit`, today the CH32V307 values (4 KB pages, 2-byte write units; the STM32 values
are there, commented out). It models no ECC and no weak cell; a simulated cut lands the whole write,
then corrupts the bytes past a write-unit boundary (XOR 0x55) and reports the tear. The partially
programmed words of the tear tests (24, 33, 34) are built by the tests themselves.

## 3. Status per target of the requirements and assumptions

The ids are those of the documentation page
[Flash requirements and assumptions](https://lean-ftl.readthedocs.io/en/latest/flash.html)
(R required, N not relied on, T tearing model).

| Id | Statement | Relied on by | STM32U5 | STM32L5 | M5531 | W25Q32RV |
|---|---|---|---|---|---|---|
| R6, N1 | An erase sets the whole erase unit to 0xFF: needed by EWLF areas (R6); regular areas only need a stable erased value (N1) | formatted records read 0xFF; `is_erased()` | doc | doc | doc | doc |
| R2 | Programming works in aligned write units of `write_size`; nothing smaller is needed (N2) | `lftl_nvm_props_t`, the accessors reject other sizes | 16 B | 8 B | 4 B minimum | 1 B minimum |
| R3, N3 | Between two erases every write unit is programmed once; the library never relies on re-programming | slot and record layouts; `LFTL_ERROR_TRANSACTION_OVERWRITE` | by construction | by construction | by construction | by construction |
| N3 (opt-in) | All zeros can be programmed over a partially programmed write unit | only the pinning proposed in `READ-ONLY-SCAN.md` (LFTL-011); the Linux simulator | doc; all-zero target open | doc; same | — to measure | — outside the spec |
| R2, R7 | `write_size` is at least the ECC codeword, so an item is one codeword and a torn program spoils only its own unit | `write_size` doc in `lean-ftl.h`; T1.13 | 16 B = codeword | 8 B = codeword | n/a | n/a |
| R7, T5 | A double-bit ECC error raises an NMI rather than returning a wrong value; a weak word may do so until it is erased or pinned | T1.13; the integrator guide | doc | doc | n/a | n/a |
| T1 | A cut during a program or erase leaves only the unit being written undefined; other units keep their content | anti-tearing, O1 and O2 of `THREAT-MODEL.md` | "not guaranteed" for the accessed content; others not stated | same | not stated | "may become corrupted" for the unit; others not stated |
| T2, T3, T4 | A unit left by a cut may be weak: read differently at different times and drift toward erased; 16 programmed bits once read correctly never read all erased; a weakly erased unit may read all erased, then show programmed bits | LFTL-011, `READ-ONLY-SCAN.md` | not documented by any vendor; a physics assumption | same | same | same |
| R5 | Erase and program are blocking and finish; a reset or cut is the only interruption | the accessors poll BSY or ISPGO | doc | doc | doc | suspend exists but a port would not use it |
| N4 | Endurance used in wear estimates: 10 k cycles per erase unit (a figure the library does not rely on) | T1.5; LFTL-010 | 10 k (100 k on at most 32 pages per bank) | — | — | 100 k |

Two consequences of the ECC rows, now T5 on the page:

- An erased line with two flipped bits is not detected (STM32U5 7.3.2, STM32L5 6.3.2), so a weakly
  erased unit can read as not erased, with no NMI, and `is_erased()` would see a stray value; that
  is a value error, not a fault, and the scan treats it as one more non-erased word.
- On the STM32U5 a pending ECCC masks the next ECCD NMI (7.3.2 caution); the application's ECC
  handling must clear ECCC promptly, or a double-bit error on a weak word passes as a corrected
  read.

## 4. Open points, to measure or to document

1. **M5531 re-programming.** Program a word twice (expected: the AND of the two values, no ISPFF),
   then all zeros over a partially programmed word; and whether a page that was being programmed at
   a reset behaves like the STM32 one ("may no longer be writable"). Needed before the M5531 port,
   and before the pinning of `READ-ONLY-SCAN.md` can claim that target.
2. **STM32: all zeros over all zeros.** 7.3.7/6.3.7 allow it, the PROGERR bullet lists it as an
   error. Measure once on each board (PROGERR set or not; the accessor's read-back passes either
   way). It only matters for a repeated pin.
3. **Endurance and retention of the STM32L5 and M5531.** In their datasheets, not in the folder;
   LFTL-010 and T1.5 use 10 k cycles for every target.
4. **STM32U5 FLASH_OPSR.** After a system reset it names the interrupted operation and its address.
   lean-ftl does not use it (its scan must also cover the power-on case, where it reads 0), but an
   integrator's reset handler could log it, and a test could read it to confirm what a tear left.
5. **Documents to add.** The CH32V307 reference manual and the RP2350 datasheet, so that the two
   remaining targets get their rows.
