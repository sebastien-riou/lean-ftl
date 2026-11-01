# lean-ftl Threat Model

Nov 1, 2026 · @Sebastien Riou

lean-ftl is assessed against two objectives, one for each way it can be deployed:

1. **On-chip or otherwise trusted NVM (Case 1).** The attacker can only send messages to the application and cut its power. lean-ftl must keep committed data atomic and fresh, must not let logical actions brick the device, and must never give access to unrelated RAM or flash, or code execution (objectives O1 to O4).
2. **External NVM (Case 2).** The attacker can also read and write the NVM at any time, and control its bus. lean-ftl does not protect the data then, but no NVM content or bus response may give access to the internal address space, or code execution (objective O5).

At v0.2.0 (commit 020bf75, 2026-10-31), lean-ftl meets both objectives for regular and EWLF areas, within the assumptions below. NVM content only picks among fixed slot or record positions inside an area, API arguments are range-checked without overflow, an error aborts the transaction it interrupts, bad configurations are refused at registration, writes only program erased cells, the scan never erases and programs at most two words, both all zeros and only on a flash declared with `zero_overwrite`, and no version is ever erased before a complete newer one exists. What remains is by design or up to the integrator: `lftl_memread()` and write sources accept any pointer (T1.7), messages can drive wear-out (T1.5), `lftl_format()` is not anti-tearing (T1.6), flash with ECC needs an NMI handler that lets a read of a torn word complete (T1.13), and external NVM needs a careful driver and careful parsing (T2.3 to T2.8).

Findings are removed from this document once fixed, as in ISSUES.md; git history keeps them, including LF-1 to LF-12, which some test comments still name, and LFTL-001 to LFTL-013, the last three closed by the read-only scan described on the [anti-tearing page](https://lean-ftl.readthedocs.io/en/latest/design.html) of the documentation. No finding is open.

## Scope and system overview

This model covers the core library, `liblean-ftl/source/ftl.c`, `liblean-ftl/source/ftl_internal.h` and `liblean-ftl/include/lean-ftl.h`, at [v0.2.0](https://github.com/sebastien-riou/lean-ftl/tree/v0.2.0) (commit 020bf75, 2026-10-31), as integrated on a bare-metal MCU. Example code, the reference callbacks under `target/` and the test harnesses are out of scope, except where the integrator contract depends on them.

How lean-ftl works, as far as security is concerned:

- An **area** is split into `n_slots = area_size / slot_size` slots. One slot holds the current data; the next one is erased and rewritten on every update.
- Each slot ends with 3 write units of metadata: `version`, `checksum` and `checksum2`. The checksum is CRC32C over the data plus the version.
- On first access, `find_current_slot()` scans all slots, skips erased versions (`0xFFFFFFFF`) and slots failing the checksum, and picks the highest version. `ctx->data` is set from the **loop index**, not from NVM content. If that slot's `checksum2` mismatches, the whole `checksum2` write unit decides, on one reading: partially programmed means the checksum is complete, and the slot is taken, its `checksum2` unit pinned to zeros first on a flash declared with `zero_overwrite`; all erased means the cut came during the checksum word at the latest, which may be weakly programmed, and the previous version is current: the scan runs again without the torn slot, which is tombstoned first (its version item pinned to zeros) on a flash with `zero_overwrite` and left as it is otherwise. The scan never erases and never copies a slot; the next write erases the torn slot, as every write erases its target.
- An **EWLF area** (`lftl_register_area_ewlf()`) packs many **records** into each erase unit. A record is a magic, a start marker (an all-zero write unit, programmed first), `version`, `checksum`, an end marker (an all-zero write unit, programmed last), then the data. Writes go downward from the top of the highest erase unit, each to the first record position whose start marker reads all erased, stepping over started positions (torn or tombstoned records) and numbering the new record above any valid checksum among them. When a unit is full, the next record goes to the top of the unit below, wrapping from the lowest unit back to the highest; that unit is erased first, whatever it reads, and the unit left behind is not erased until the write order returns to it. EWLF areas have no transactions.
- On first access to an EWLF area, `find_current_record()` checks only the positions where records can start (the top of each erase unit minus a multiple of the record size). A candidate is a record with a valid magic and checksum whose end marker is not all erased; the current record is the candidate with the highest version over all units, the lowest candidate position being the newest inside a unit. The current unit is found by halving on the units' keys (the version of their top record), the newest record inside it by halving on start markers on a flash with `zero_overwrite`; the result is checked against the two neighbouring units, with the linear pass over every unit as the fallback, and the Linux test build runs both at every scan. A partially programmed end marker is pinned, a torn record newer than the one taken is tombstoned, on a flash with `zero_overwrite`; nothing is erased. `ctx->data` is set from the position found: NVM content picks among fixed positions inside the area, but never sets where a record starts.
- The application addresses data through **logical addresses** inside the `nvm` struct (`nvm.data0`). `translate_addr()` maps them to the current slot or record after an overflow-safe range check, and the transaction functions run the same check first.
- Transactions (regular areas only) use a caller-owned RAM bitmap (`transaction_tracker`, one bit per write unit) to detect double writes and to know which units to copy at commit.
- `lftl_register_area()` and `lftl_register_area_ewlf()` check the configuration (write unit size, area bounds and alignment, slot count, EWLF record size) and raise `LFTL_ERROR_BAD_CONFIG` before linking a bad area. `LFTL_AREA` and `LFTL_AREA_EWLF` reserve the pages the run-time layout needs, and `LFTL_AREA_EWLF` refuses at compile time a record that does not fit in a page.
- NVM access goes through integrator callbacks (`erase`, `write`, `read`), set once per NVM in `lftl_nvm_props_t`. The exception is `mem_read()`, which copies any address outside every registered NVM with `memcpy`; that is how RAM `src` buffers are read. Errors go to the NVM's `error_handler`, after the transaction of the area the failing operation was for is aborted. The handler must not return; if it does, the library loops forever.

| Asset | Why it matters | Case 1 | Case 2 |
| --- | --- | --- | --- |
| Internal RAM and the CPU's address space | Corruption leads to code execution or secret disclosure | Protect | Protect (sole objective) |
| Firmware and other non-LFTL flash | Erasing or overwriting it bricks the device or plants code | Protect | Protect |
| Integrity of committed LFTL data | Application state, counters, configuration | Protect | Not protected |
| Freshness (no rollback to an older version) | Anti-replay counters, revocation state | Protect | Not protected |
| Availability of the area | Device must keep booting | Protect against logical attacks | Not protected |
| Confidentiality of LFTL data | Secrets stored in NVM | Out of scope (no physical access) | Not protected |

## Assumptions and out of scope

Both cases rest on the same baseline: the code, the configuration and the callbacks are trusted; only runtime inputs are hostile.

**Assumed in both cases**

- Firmware, the lean-ftl configuration (`lftl_nvm_props_t`, every `lftl_ctx_t`) and the callbacks are trusted and correct. `write_size`, `erase_size`, `area` and `data_size` are compile-time constants, never derived at runtime from the NVM (for example from SFDP tables). Registration checks that the configuration is consistent, but cannot tell whether it matches the hardware.
- The flash behaves as stated on the [Flash requirements and assumptions](https://lean-ftl.readthedocs.io/en/latest/flash.html) page: requirements R1 to R8, and the tearing model T1 to T5 the anti-tearing argument below rests on (a cut affects only the unit being written; a weak word may read anything, and may read all erased later).
- `error_handler` never returns: it resets, or `longjmp`s to a recovery point. The library loops forever if it does return, so a returning handler costs availability instead of turning each check into a no-op.
- An error aborts the transaction of the area it happened on, and only that one.
- lean-ftl is called from one execution context per area. It is not thread-safe or reentrant, and its registration lists (`first_area`, `first_area_ewlf`, `first_nvm`) are global.
- Power loss or reset at any instant is **in scope**. It is the normal operating condition the anti-tearing logic is built for, not an attack. Deliberate glitching that corrupts a single instruction or flash cell is fault injection and stays out of scope.

**Out of scope in both cases**

- Fault injection: voltage, clock, EM or laser glitches.
- Side channels: power, EM, timing, cache.
- Physical readout of internal flash or RAM (decapping, probing, debug port).
- Bugs in application code that do not go through the lean-ftl API.

**What the attacker controls**

|  | Case 1: on-chip / trusted NVM | Case 2: external NVM |
| --- | --- | --- |
| Messages to the application | Yes, arbitrary content and timing | Yes |
| Arguments the application derives from messages (offsets, sizes, sequences of calls) | Yes, insofar as the application lets them through | Yes |
| Power cuts at chosen instants | Yes (functional, not fault injection) | Yes |
| NVM content | No | Yes, read and write at any time, including between two reads |
| NVM bus responses (status, errors, timing) | No | Yes |

## Case 1: on-chip / trusted NVM

The transaction and anti-rollback logic holds against power cuts for both kinds of area, and so does the memory-safety objective. Out-of-range arguments end in the error handler before any access, an error ends the transaction it interrupts, and configurations that would overflow buffers or write outside the area are refused.

The attacker can only send messages to the application. They reach lean-ftl through the arguments the application derives from those messages (`dst_nvm_addr`, `src`, `size`), through the sequence of calls (start, write, commit, abort, duplicates), through the content of data the application stores for them, and through the timing of power cuts. lean-ftl is the last line of defence: its documented checks (`FIRST_NOT_IN_DATA`, `LAST_NOT_IN_DATA`, `TRANSACTION_OVERWRITE`) are expected to stop an application bug that forwards a bad offset or a duplicated record.

### Security objectives

| Objective | Status | Basis |
| --- | --- | --- |
| O1. Atomicity: after any power cut, reads return either the old or the new committed data | Holds | Regular areas: writes go to the erased next slot; metadata is written last, `checksum2` after `version` and `checksum`. A torn slot fails CRC32C and is skipped. A slot whose `checksum2` reads all erased is not taken when another slot is valid (its checksum may be weakly programmed, and no boot ever returned it); one whose `checksum2` is partially programmed is taken (its checksum is complete). The scan never erases; the next write erases the torn slot, with 2 slots as with more. EWLF areas: a record is written start marker first, then data, magic, version and checksum, then the end marker, only into a unit erased right before its first record, and its checksum is computed from the data meant to be written. A record torn before its checksum is complete fails its magic or checksum and is skipped by the scan and by the next write; one torn at its checksum word has an erased end marker and is not taken; one torn at its end marker is taken. A torn rollover leaves its record in a unit the next rollover erases again before programming it (LFTL-013). Only record positions are checked, so data cannot pass for a record. Detection of a torn data write is probabilistic (CRC32C, 2^-32). |
| O2. No rollback below the last committed version | Holds; one residual without `zero_overwrite` | `version` is incremented on every commit and the highest valid version wins, in regular and EWLF areas alike. Wrap at `0xFFFFFFFE` needs about 4.3 billion updates, far beyond endurance (native endurance × wear-leveling factor). An interrupted write can bring back the previous version, never an older one. On a flash declared with `zero_overwrite` the scan pins what it decides about the one word a power cut can leave weak, so no later boot decides otherwise. Without it (the W25Q32RV, the M5531 until measured, the CH32V307) the decision is made again at every boot on the word as it reads then, until the next write: a partially programmed end marker that later reads all erased brings the previous version back, an all-erased one that later reads programmed brings the torn write's version forward, each by one version ([what a power cut can still do](https://lean-ftl.readthedocs.io/en/latest/design.html#design-residual)). |
| O3. No bricking through logical actions | Partly | Wear-out, a torn `lftl_format` and, on flash with ECC, a torn write without the NMI handling of T1.13 can leave the device unusable; see threats T1.5, T1.6 and T1.13. |
| O4. No access to unrelated RAM or flash, no code execution | Holds | Given the integrator contract below, T1.7 in particular: arguments are range-checked before any access, metadata buffers are sized from the write unit, an aborted transaction leaves no tracker behind, and every slot or record the library touches lies inside its area. |
| O5. Bounded time, independent of the NVM state after boot | Holds for regular areas; EWLF areas bounded only | Every loop runs over a configuration constant (`n_slots`, `n_pages`, `records_per_unit`, the two passes of `find_current_slot()`, the halvings of `find_current_record()`), and the callbacks complete or fail (R5 of the flash page), so each operation has a worst case that follows from the configuration alone. After the scan, a regular-area write costs one slot erase plus the data and the meta-data whatever the NVM holds: it erases its slot unconditionally (`write_core()`, `erase()`, transaction start). An EWLF write is bounded but depends on the state in one way: a torn or tombstoned record left below the current one is stepped over by the free-position search of `write_ewlf_core()`, one start-marker read, one checksum when its magic is valid and, with `zero_overwrite`, one word program when its start marker is not all zeros, per such position. A rollover is exactly one erase in every state: the unit entered is erased unconditionally, the unit left never is. The scan is bounded but state-dependent by design: up to two passes with a CRC per slot whose version is at least the running maximum; for an EWLF area a logarithmic number of key reads and start-marker reads, the linear pass as fallback, and at most two word programs; never an erase. No test measures timing. |

### Threats

| ID | Threat | Mitigation in lean-ftl | Residual risk |
| --- | --- | --- | --- |
| T1.1 | Out-of-range destination or size reaches a write | Overflow-safe range check (`check_in_data()`) before the tracker or any NVM access | None |
| T1.2 | Out-of-range source or size reaches a read | Same check in `lftl_read()` and `lftl_transaction_read()` | None |
| T1.3 | Error raised mid-transaction leaves stale state | The error aborts the area's transaction before the handler runs | An error on another area leaves this area's transaction open. If the handler unwinds the frame holding its tracker, the recovery path must abort it. |
| T1.4 | Reading uncommitted data and acting on it | `lftl_read()` returns committed data during a transaction | Application must not base security decisions on `lftl_read_newer()` or `lftl_transaction_read()` results; the transaction may still abort. |
| T1.5 | Wear-out driven by messages | None | Each `lftl_basic_write()` to a regular area erases a full slot, and so does each `lftl_transaction_start()`, even if aborted. A loop of start/abort burns endurance. EWLF areas erase one unit per unit's worth of records. The application must rate-limit or batch writes. |
| T1.6 | Power cut during `lftl_format()` | Documented as not anti-tearing | The next scan raises `NO_VALID_VERSION` on every boot. The application must keep its own "formatted" flag in another area and recover. |
| T1.7 | Arbitrary-pointer gadgets | None, by design | `lftl_memread()` copies from any address, and the `src` of every write may be any address. An attacker-controlled pointer reaching either one reads arbitrary memory, or copies it into NVM to read back later. Only pass pointers the application built itself. |
| T1.8 | Stale data after logical erase | Documented | `lftl_erase_all()` leaves previous versions in other slots, and in EWLF areas, older records stay until their erase unit is recycled. Any raw NVM read (for example `lftl_memread()` outside the area) can still return them. |
| T1.9 | First operation on an area after a reset | `translate_addr()` and `lftl_transaction_start()` locate the current slot on first use | None |
| T1.10 | Power cut during an EWLF write | A unit is erased right before its first record; the scan and the next write skip a record torn before its checksum; one torn at its checksum word has an erased end marker, is not taken and, with `zero_overwrite`, tombstoned; one torn at its end marker is taken and, with `zero_overwrite`, pinned; a torn rollover is redone, erase included, by the next write | Without `zero_overwrite`, the one-version residual of O2 |
| T1.11 | Attacker-chosen data stored in an EWLF area | Only record positions are checked, so data cannot pass for a record | None |
| T1.12 | Configuration that does not fit the run-time layout | Registration checks the configuration; `LFTL_AREA_EWLF` refuses an oversized record at compile time | None for what registration can see; whether the configuration matches the hardware stays an assumption |
| T1.13 | Torn program or erase on flash with ECC | None: the scan and EWLF writes must read a word before they can decide on it; with `zero_overwrite` the pin or tombstone that follows programs a valid codeword over the torn word | On STM32L5 and STM32U5 a double-bit ECC error raises an NMI on read, and a torn program or erase can leave such a word. The NMI handler must clear the error and return, so the read completes and lean-ftl decides on the content; one that loops or resets hangs the device at every boot. The torn word is never in committed data. With `zero_overwrite`, which both parts declare, the word is pinned at the first boot that reads it, one NMI in all, provided the check bits of an all-zero word are all zero: inferred from the documented all-zero overwrite, to be measured (FLASH.md). A cut during the pin itself can leave the data cells at zero and a check cell weak, an NMI per boot until the unit is erased. See the integrator section, and R7 and T5 of the [flash page](https://lean-ftl.readthedocs.io/en/latest/flash.html#flash-tearing). |

## Case 2: external NVM

The library's own handling of hostile NVM content meets the objective for both kinds of area: NVM content only picks among fixed slot or record positions inside the area, and never sets an address, index or size. A bus error inside a transaction aborts that transaction (T2.2). The integrator's driver and application parsing carry the remaining risk.

The attacker reads and writes the external NVM at any time, including between two reads made by the same function, and controls every bus response. Integrity, freshness, confidentiality and availability of LFTL data are explicitly **not** goals. The only objective is:

> **O5.** No sequence of NVM contents or bus responses gives the attacker read or write access to the internal address space, or code execution.

### What the library reads from NVM, and what it does with it

| Value read from NVM | Used for | Can it become an address, index or size? | What the attacker can do with it |
| --- | --- | --- | --- |
| `version` (per slot or record) | Choosing the current slot, or between an EWLF record and the top record of the next unit; `+1` for the next version | No. The slot index or record position comes from the scan loops, so `ctx->data` always points at a slot or record of the configured area | Pick any slot or record; force `VERSION_COLLISION`; force `NO_VALID_VERSION`. Setting `0xFFFFFFFE` makes the next commit write `0xFFFFFFFF`, which the next scan treats as erased |
| `checksum` | Slot or record validity check | No | Forge freely; CRC32C is not keyed |
| `checksum2`, EWLF end marker | Detecting a torn metadata write, in slots and records | No | With it erased or partially programmed on a valid slot or record: force one word program at the next boot on a flash with `zero_overwrite` (a pin or a tombstone, once per crafted word, since a pinned word reads zeros for good); nothing without |
| EWLF magic | Recognizing a record at a record position | No. Only record positions are checked, and each lies within its erase unit | Make any record position look valid or not, as with any record content |
| EWLF start marker | Choosing where the next record goes, and the in-unit search of the scan | No. The write path only steps over positions of the current unit, the scan only reads positions | Force the next write to step over positions and, with `zero_overwrite`, to program their start markers, at most a unit's worth; make the scan's fast search fail its check and fall back to the linear pass |
| Erased state of EWLF record cells | Choosing where the next record goes | No. The record stays on a record position of its unit | Force one rollover, one erase, before the next write lands in a freshly erased unit |
| Data bytes | Copied slot to slot at commit, copied to caller buffers | No. Copy sizes come from the configuration or the caller | Any content, changed between reads |
| Callback status codes | Raising `LOW_LEVEL_*` errors | No | Raise an error inside any operation, at a chosen point; it aborts that area's transaction |

The library may read the same metadata more than once during a scan (a unit's key, then its records; the cross-check build reads everything twice). A time-of-check/time-of-use (TOCTOU) change between those reads only changes which data is returned. That is a data-integrity effect, which is accepted.

### Threats

| ID | Threat | Status | Required mitigation |
| --- | --- | --- | --- |
| T2.1 | Hostile metadata forces an error path | Availability only | The library loops forever if the handler returns, so `NO_VALID_VERSION` cannot produce a wild pointer. The handler must still reset or `longjmp`; one that returns hangs the device. |
| T2.2 | Bus error inside a transaction | Mitigated | The error aborts that area's transaction before the handler runs. See T1.3 for transactions on other areas. |
| T2.3 | Application parses NVM data as trusted | Integrator | Treat every byte read from an external area as an untrusted packet: bound every length and index against compile-time constants, store no pointers or function pointers, and validate enums and state values. |
| T2.4 | Double fetch by the application | Integrator | Read once into RAM with `lftl_read()`, validate the RAM copy and use only that copy. Never read a length field and then the payload in two calls, and never dereference the `nvm` struct directly. |
| T2.5 | Driver trusts the device | Integrator | Translate each virtual address to a device offset with a bounds check against the window. Never take sizes from the device (SFDP geometry, length fields). Bound every busy-wait loop with a timeout. |
| T2.6 | Write callback dereferences a virtual address | Integrator | lean-ftl copies slot to slot, so the write callback's `src` may lie inside the NVM window. For SPI flash that is not memory-mapped, the callback must read it through the device into a bounce buffer, never as a RAM pointer. |
| T2.7 | Virtual window overlaps real memory | Integrator | `nvm_props.base` and `size` decide whether an address goes to the callback or to `memcpy`. Place the window in unmapped address space so a stray dereference faults, and keep it apart from RAM, peripherals and internal flash. |
| T2.8 | Memory-mapped external flash (XIP, OctoSPI) | Integrator | Content can change between two CPU loads. Mark the region non-executable in the MPU, and make the bus-fault handler reset rather than resume. |
| T2.9 | Forced reformat or wear | Accepted | If the "formatted" flag lives in external NVM, or the application reformats on `NO_VALID_VERSION`, the attacker can wipe the data. `lftl_format()` erases a range taken from the configuration, so this path stays memory-safe. |
| T2.10 | Hostile EWLF content | Mitigated | Only record positions are read, each within its erase unit, so the scan never reads past the area. |

## Integrator responsibilities and hardening

The library cannot check the contract below itself. Each item is needed in both cases unless marked.

**Contract checklist**

- [ ] `error_handler` never returns: it resets, or `longjmp`s to a recovery point. One that returns hangs the device.
- [ ] A transaction tracker is at least `LFTL_TRANSACTION_TRACKER_SIZE(ctx)` bytes, and stays valid until the transaction ends with `lftl_transaction_commit()` or `lftl_transaction_abort()`. A tracker in static storage always does. One on the stack needs its transaction ended before that frame returns, or is unwound by an error handler, including for an error on another area: lean-ftl aborts only the transaction of the area an error happened on.
- [ ] Before calling lean-ftl, the application checks every offset and size it derived from a message against the field it targets, overflow-safe: `off <= field_size && len <= field_size - off`. lean-ftl raises an error instead of corrupting memory, but with a handler that resets, each bad message would become a reboot.
- [ ] Batch messages are de-duplicated before a transaction, so `TRANSACTION_OVERWRITE`, which aborts the transaction, cannot be triggered from outside.
- [ ] No attacker-influenced pointer reaches `lftl_memread()` or the `src` of any write (T1.7).
- [ ] Message-driven writes are rate-limited and batched against the endurance budget: native endurance × wear-leveling factor per page (T1.5).
- [ ] `zero_overwrite` is declared only for a flash whose manual documents programming all zeros over a programmed word (STM32L5, STM32U5), and left false otherwise: lean-ftl then never programs a unit that is not erased, and the scan writes nothing (N3 of the flash page).
- [ ] A "formatted" flag in a separate area lets the application recover from a torn `lftl_format()` (T1.6).
- [ ] Flash with ECC (STM32L5, STM32U5 and similar): the NMI handler clears the double-error flag and returns, so a word left by a torn program or erase reads as content lean-ftl then rejects, instead of hanging or resetting the device at every boot (T1.13). The write unit is at least the ECC codeword, so that each metadata item is its own codeword (16 bytes on the STM32U5, 8 on the L5). R7 of the [flash page](https://lean-ftl.readthedocs.io/en/latest/flash.html#flash-required).
- [ ] Case 2 only: the driver, the address window and the application parsing follow T2.3 to T2.8.

**Defence in depth: bound the callbacks.** The callbacks check that each range is a flash address. The reference `erase`, `write` and `read` callbacks do: the STM32 ones refuse anything outside the flash, and the CH32V307 one also accepts the flash's `0x00000000` alias. They cannot check that a range is inside an LFTL area, since they do not know where the areas are. An integrator who knows its layout can bound its own callbacks to the LFTL areas, which turns a future bug that reaches other flash into an error. On parts with an MPU or TrustZone, also make the LFTL areas non-executable, and make firmware flash write-protected while lean-ftl runs.

**Flash with ECC (STM32L5, STM32U5 and similar).** These parts protect each flash word with an ECC: a single-bit error is corrected on read, a double-bit error sets `ECCD` in `FLASH_ECCR`, records the address, and raises an NMI. A program or erase interrupted by a power cut can leave a word whose data and ECC do not match, and lean-ftl must read such words before it can judge them: at the first access after a reset, the scan reads the version word of every slot and the magic of every record position, and an EWLF write reads the cells of the next record position to check that they are erased. The torn word always lies in the slot being erased or written, or in the record being written, never in committed data, so lean-ftl's own checks (version, checksum, magic, erased state) reject it once the read completes, as they do on parts without ECC and on the Linux simulator, where the read returns the raw content directly. The reference STM32 callbacks read with `memcpy` and do nothing about ECC, and the testbenches' generated NMI handlers loop forever: there, a torn write hangs the device at every boot until the area is reformatted from outside. The integrator's NMI handler must check `ECCD`, clear it and return, so that the read completes; the recorded address can be logged. It must not reset, which turns every boot into the same failure, and must not erase anything: the torn EWLF word lies in the erase unit that holds the current record.

**If Case 2 needs integrity or freshness after all.** lean-ftl cannot provide either, but it composes with a small wrapper:

1. Encrypt and authenticate each record with an AEAD (for example AES-GCM or ChaCha20-Poly1305), with a key kept on chip.
2. Bind the associated data to the area identifier and a record version, so records cannot be swapped between areas or fields.
3. Keep the version counter in an **internal** lean-ftl area, which is Case 1 storage, to detect rollback of the external copy.
4. Verify the tag over the RAM copy (T2.4) before parsing anything.

This is the same pattern TF-M Protected Storage uses over its internal trusted storage.

## Verification plan

`testit` runs the test suite on the Linux NVM simulator, whose write callback refuses to program a write unit that is not erased, like STM32U5-class flash. Most tests run under the tearing simulation, which cuts power at every write-unit boundary and checks that the data reads back as the last or the previous committed version; test 35 checks the simulator itself: a cut anywhere in a multi-unit write or erase corrupts exactly the bytes past it. After each tear and that check, the sweep writes new data to every area it checks and checks again after another simulated reset, so the first write after a power cut is covered too. The simulator never leaves a partly erased page, so torn erases are covered by hand-built states only: tests 24, 29, 32, 33, 34 and 36 build specific torn states by hand, regular and EWLF: a slot whose `checksum2` is erased or partially programmed, pinned or tombstoned where the flash allows it (test 24), a next slot whose version word reads as the current version after a torn erase (test 32), an EWLF record whose end marker is erased or partially programmed, within a unit and as a rollover record (test 33), a power cut during the scan's tombstone, which must leave the previous version untouched and erase nothing (test 34), power cuts in EWLF writes and rollovers (test 29), and three cycles of writes over an area of 17 erase units with cuts at every stage of a record write and during a rollover's erase (test 36). Test 37 rebuilds areas formatted and written by lean-ftl v0.1.0 and checks that the scan takes the slot v0.1.0 had current and programs and erases nothing, before the area takes new writes. A word that reads differently at each boot cannot be built on flash; on the simulator, tests 24 and 33 make a torn word read erased after the first boot, and check the one-version residual without `zero_overwrite`. The suite runs twice, with the simulator's all-zero overwrite and `zero_overwrite` on, then off (`--no-pinning`), and the Linux build defines `LFTL_SCAN_CROSS_CHECK`: every scan of an EWLF area runs the fast search and the linear pass and raises `LFTL_INTERNAL_ERROR` if they disagree.

`testit` also runs `test/asan/build-and-run`, which builds `test/asan/asan_harness.c` with AddressSanitizer and UndefinedBehaviorSanitizer. The harness covers hand-built scenarios: out-of-range arguments, an error inside a transaction, write units larger than 16 bytes, a transaction started first after a reset, power cuts in EWLF writes on NOR and STM32U5-like flash, hostile EWLF content, and an oversized EWLF record. Each test must end without a sanitizer report or a callback access outside the area, and with its expected result.

On 2026-10-31 the suite ran on the NUCLEO-U5A5ZJ-Q and NUCLEO-L552ZE-Q boards (through lean-hwbridge, the test firmware built with `HAS_PRINTF`): tests 1 to 37 passed on both, in the pinning variant their builds declare, so the pins and tombstones of the scan have been programmed over partially programmed words on real STM32 flash, through callbacks that check the flash's error flags. A board run does not produce a torn program, which only a power cut during one does.

Not covered: the ECC behaviour of real flash (T1.13). The simulator returns the raw content of a torn word; the NMI handling is the integrator's, and a power cut during a program or erase on an STM32L5 or STM32U5 board would be the test for it.

Three fuzzing harnesses would widen the coverage beyond these scenarios:

| Harness | Covers | Inputs | Pass criterion |
| --- | --- | --- | --- |
| API sequence fuzzer (libFuzzer) | Case 1, O4 | Random sequences of start, write, write\_any, read, read\_newer, commit, abort, erase\_all, with random offsets and sizes, on regular and EWLF areas, including sequences that begin right after a reset; the handler `longjmp`s and recovery continues | No sanitizer report; every out-of-range call ends in the handler |
| NVM image fuzzer | Case 2, O5 | Mutated images of regular and EWLF areas, then scan, read, write and commit | No sanitizer report; callbacks never see an address outside the area, and nothing reads outside it without them |
| Fault-injecting callbacks | Case 2, T2.2 | The n-th callback returns an error or different data on each read | Same as above, with errors raised inside transactions |

Also worth doing: a bounded model check (CBMC or Frama-C) of `check_in_data()`, `is_in_range()`, the tracker indexing and the EWLF record placement in `find_current_record()` and `write_ewlf_core()`, which carry most of the memory-safety burden. In CI, build with `-Wconversion`, since `offset_wu` is a `uint32_t` derived from a `uintptr_t`. The ASan harness can seed the API fuzzer.

## Sources

- [lean-ftl source](https://github.com/sebastien-riou/lean-ftl), `liblean-ftl/` at [v0.2.0](https://github.com/sebastien-riou/lean-ftl/tree/v0.2.0) (commit 020bf75, 2026-10-31), Apache-2.0
- `FLASH.md` and `ISSUES.md` in the same repository, at the same tag
- [lean-ftl API reference](https://lean-ftl.readthedocs.io/en/latest/api.html)
- [lean-ftl user guide](https://lean-ftl.readthedocs.io/en/latest/using.html)
- [lean-ftl anti-tearing and wear leveling](https://lean-ftl.readthedocs.io/en/latest/design.html)
- [lean-ftl flash requirements and assumptions](https://lean-ftl.readthedocs.io/en/latest/flash.html)
- [lean-ftl internals](https://lean-ftl.readthedocs.io/en/latest/internal.html)
- [NLnet project page, LeanFTL](https://nlnet.nl/project/LeanFTL/)
