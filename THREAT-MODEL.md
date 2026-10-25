# lean-ftl Threat Model

Oct 22, 2026 · @Sebastien Riou

lean-ftl is assessed against two objectives, one for each way it can be deployed:

1. **On-chip or otherwise trusted NVM (Case 1).** The attacker can only send messages to the application and cut its power. lean-ftl must keep committed data atomic and fresh, must not let logical actions brick the device, and must never give access to unrelated RAM or flash, or code execution (objectives O1 to O4).
2. **External NVM (Case 2).** The attacker can also read and write the NVM at any time, and control its bus. lean-ftl does not protect the data then, but no NVM content or bus response may give access to the internal address space, or code execution (objective O5).

At commit 426fcb1, lean-ftl meets both objectives for regular and EWLF areas, within the assumptions below. NVM content only picks among fixed slot or record positions inside an area, API arguments are range-checked without overflow, an error aborts the transaction it interrupts, bad configurations are refused at registration, EWLF writes only program erased cells, and the scan never erases the previous version of an area before a complete newer one exists. What remains is by design or up to the integrator: `lftl_memread()` and write sources accept any pointer (T1.7), messages can drive wear-out (T1.5), `lftl_format()` is not anti-tearing (T1.6), flash with ECC needs an NMI handler that lets a read of a torn word complete (T1.13), and external NVM needs a careful driver and careful parsing (T2.3 to T2.8).

No finding is open. As in ISSUES.md, findings are removed from this document once fixed; git history keeps them, including LF-1 to LF-12, which some test comments still name. Open work items are tracked in ISSUES.md.

## Scope and system overview

This model covers the core library, `liblean-ftl/source/ftl.c`, `liblean-ftl/source/ftl_internal.h` and `liblean-ftl/include/lean-ftl.h`, at [commit 426fcb1](https://github.com/sebastien-riou/lean-ftl/tree/426fcb1) (2026-10-22, branch `main`), as integrated on a bare-metal MCU. Example code, the reference callbacks under `target/` and the test harnesses are out of scope, except where the integrator contract depends on them.

How lean-ftl works, as far as security is concerned:

- An **area** is split into `n_slots = area_size / slot_size` slots. One slot holds the current data; the next one is erased and rewritten on every update.
- Each slot ends with 3 write units of metadata: `version`, `checksum` and `checksum2`. The checksum is CRC32C over the data plus the version.
- On first access, `find_current_slot()` scans all slots, skips erased versions (`0xFFFFFFFF`) and slots failing the checksum, and picks the highest version. `ctx->data` is set from the **loop index**, not from NVM content. If that slot's `checksum2` mismatches, the whole `checksum2` write unit decides: all erased means the cut came during the checksum word at the latest, which may be weakly programmed, and the slot is erased, the previous version becoming current; partially programmed means the checksum is complete, and the slot is kept. The scan never copies a slot, so it never erases the previous version on the strength of a word that may read differently later.
- An **EWLF area** (`lftl_register_area_ewlf()`) packs many **records** into each erase unit. A record is a magic, a `prev_erased` flag, the same three metadata items, then the data. Writes go downward from the top of the highest erase unit, each to the first record position whose cells all read erased. When a unit is full, the next record goes to the top of the unit below, wrapping from the lowest unit back to the highest; then the vacated unit is erased and the new record's `prev_erased` flag is set. EWLF areas have no transactions.
- On first access to an EWLF area, `find_current_record()` checks only the positions where records can start (the top of each erase unit minus a multiple of the record size), by increasing address. It takes the first record with a valid magic and checksum, or the newest such record of the next unit in write order if that one is newer (a wrap interrupted before the lowest unit was erased). A record whose `checksum2` mismatches is copied forward first, with complete metadata, as a torn slot is, and only then is the previous unit of a rollover erased. `ctx->data` is set from that position: NVM content picks among fixed positions inside the area, but never sets where a record starts.
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
| O1. Atomicity: after any power cut, reads return either the old or the new committed data | Holds | Regular areas: writes go to the erased next slot; metadata is written last, `checksum2` after `version` and `checksum`. A torn slot fails CRC32C and is skipped. A slot whose `checksum2` is missing is erased at the next scan when `checksum2` reads all erased (its checksum may be weakly programmed, and no boot ever returned it), and kept when `checksum2` is partially programmed (its checksum is complete); the previous version is never erased before a complete slot exists, with 2 slots as with more. EWLF areas: a record is written data first, then magic, version and checksum, then `checksum2`, only into cells that read erased, and its checksum is computed from the data meant to be written. A record torn before its checksum is complete fails its magic or checksum and is skipped by the scan and by the next write. One torn at its checksum or `checksum2` is taken when it reads valid and copied forward with complete metadata before anything is erased, so a weakly programmed word cannot change the outcome at a later boot. An interrupted wrap is completed at the next scan. Only record positions are checked, so data cannot pass for a record. Detection of a torn data write is probabilistic (CRC32C, 2^-32). |
| O2. No rollback below the last committed version | Holds | `version` is incremented on every commit and the highest valid version wins. Wrap at `0xFFFFFFFE` needs about 4.3 billion updates, far beyond endurance (native endurance × wear-leveling factor). EWLF areas take the first valid record by address, or the top record of the next unit in write order if it is newer; an interrupted write can bring back the previous version, never an older one. |
| O3. No bricking through logical actions | Partly | Wear-out, a torn `lftl_format` and, on flash with ECC, a torn write without the NMI handling of T1.13 can leave the device unusable; see threats T1.5, T1.6 and T1.13. |
| O4. No access to unrelated RAM or flash, no code execution | Holds | Given the integrator contract below, T1.7 in particular: arguments are range-checked before any access, metadata buffers are sized from the write unit, an aborted transaction leaves no tracker behind, and every slot or record the library touches lies inside its area. |

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
| T1.10 | Power cut during an EWLF write | Records are written only into erased cells; the scan and the next write skip a record torn before its checksum; one torn at its checksum or `checksum2` is copied forward before the previous unit of a rollover is erased; an interrupted wrap is completed at the next scan | None |
| T1.11 | Attacker-chosen data stored in an EWLF area | Only record positions are checked, so data cannot pass for a record | None |
| T1.12 | Configuration that does not fit the run-time layout | Registration checks the configuration; `LFTL_AREA_EWLF` refuses an oversized record at compile time | None for what registration can see; whether the configuration matches the hardware stays an assumption |
| T1.13 | Torn program or erase on flash with ECC | None: the scan and EWLF writes must read a word before they can skip it | On STM32L5 and STM32U5 a double-bit ECC error raises an NMI on read, and a torn program or erase can leave such a word. The NMI handler must clear the error and return, so the read completes and lean-ftl rejects the content; one that loops or resets hangs the device at every boot. The torn word is never in committed data. A slot or record kept with a partially programmed `checksum2` keeps that word until a later write erases it: one NMI per boot while it is current. See the integrator section. |

## Case 2: external NVM

The library's own handling of hostile NVM content meets the objective for both kinds of area: NVM content only picks among fixed slot or record positions inside the area, and never sets an address, index or size. A bus error inside a transaction aborts that transaction (T2.2). The integrator's driver and application parsing carry the remaining risk.

The attacker reads and writes the external NVM at any time, including between two reads made by the same function, and controls every bus response. Integrity, freshness, confidentiality and availability of LFTL data are explicitly **not** goals. The only objective is:

> **O5.** No sequence of NVM contents or bus responses gives the attacker read or write access to the internal address space, or code execution.

### What the library reads from NVM, and what it does with it

| Value read from NVM | Used for | Can it become an address, index or size? | What the attacker can do with it |
| --- | --- | --- | --- |
| `version` (per slot or record) | Choosing the current slot, or between an EWLF record and the top record of the next unit; `+1` for the next version | No. The slot index or record position comes from the scan loops, so `ctx->data` always points at a slot or record of the configured area | Pick any slot or record; force `VERSION_COLLISION`; force `NO_VALID_VERSION`. Setting `0xFFFFFFFE` makes the next commit write `0xFFFFFFFF`, which the next scan treats as erased |
| `checksum` | Slot or record validity check | No | Forge freely; CRC32C is not keyed |
| `checksum2` | Detecting a torn metadata write, in slots and records | No | With `checksum2` erased on a valid slot or record: force the erase of that slot, or a record write, at every boot (external flash wear) |
| EWLF magic | Recognizing a record at a record position | No. Only record positions are checked, and each lies within its erase unit | Make any record position look valid or not, as with any record content |
| EWLF `prev_erased` flag | Deciding whether to re-erase the previous unit | No. The unit comes from the record's position, modulo the number of units | Force one unit erase per boot (external flash wear) |
| Erased state of EWLF record cells | Choosing where the next record goes, and whether to erase a unit before a rollover | No. The record stays on a record position of its unit | Force an erase on every write (external flash wear) |
| Data bytes | Copied slot to slot at commit, copied to caller buffers | No. Copy sizes come from the configuration or the caller | Any content, changed between reads |
| Callback status codes | Raising `LOW_LEVEL_*` errors | No | Raise an error inside any operation, at a chosen point; it aborts that area's transaction |

The library also re-reads metadata several times during a scan (checksum check, then version, then `checksum2`). A time-of-check/time-of-use (TOCTOU) change between those reads only changes which data is returned. That is a data-integrity effect, which is accepted.

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
- [ ] A "formatted" flag in a separate area lets the application recover from a torn `lftl_format()` (T1.6).
- [ ] Flash with ECC (STM32L5, STM32U5 and similar): the NMI handler clears the double-error flag and returns, so a word left by a torn program or erase reads as content lean-ftl then rejects, instead of hanging or resetting the device at every boot (T1.13). The write unit is at least the ECC codeword, so that each metadata item is its own codeword (16 bytes on the STM32U5, 8 on the L5).
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

`testit` runs the test suite on the Linux NVM simulator, whose write callback refuses to program a write unit that is not erased, like STM32U5-class flash. Most tests run under the tearing simulation, which cuts power at every callback boundary and checks that the data reads back as the last or the previous committed version. After each tear and that check, the sweep writes new data to every area it checks and checks again after another simulated reset, so the first write after a power cut is covered too. The simulator never leaves a partly erased page, so torn erases are covered by hand-built states only: tests 24, 29, 32, 33 and 34 build specific torn states by hand, regular and EWLF: a slot whose `checksum2` is erased or partially programmed (test 24), a next slot whose version word reads as the current version after a torn erase (test 32), an EWLF record whose `checksum2` is erased or partially programmed, within a unit and as a rollover record (test 33), and a power cut right after the first erase of the scan's repair, which must have hit the torn slot and not the previous version (test 34). A word that reads differently at each boot cannot be built on flash; on the simulator, test 33 makes the torn record's checksum read erased after the first boot.

`testit` also runs `test/asan/build-and-run`, which builds `test/asan/asan_harness.c` with AddressSanitizer and UndefinedBehaviorSanitizer. The harness covers hand-built scenarios: out-of-range arguments, an error inside a transaction, write units larger than 16 bytes, a transaction started first after a reset, power cuts in EWLF writes on NOR and STM32U5-like flash, hostile EWLF content, and an oversized EWLF record. Each test must end without a sanitizer report or a callback access outside the area, and with its expected result.

Not covered: the ECC behaviour of real flash (T1.13). The simulator returns the raw content of a torn word; the NMI handling is the integrator's, and a power cut during a program or erase on an STM32L5 or STM32U5 board would be the test for it.

Three fuzzing harnesses would widen the coverage beyond these scenarios:

| Harness | Covers | Inputs | Pass criterion |
| --- | --- | --- | --- |
| API sequence fuzzer (libFuzzer) | Case 1, O4 | Random sequences of start, write, write\_any, read, read\_newer, commit, abort, erase\_all, with random offsets and sizes, on regular and EWLF areas, including sequences that begin right after a reset; the handler `longjmp`s and recovery continues | No sanitizer report; every out-of-range call ends in the handler |
| NVM image fuzzer | Case 2, O5 | Mutated images of regular and EWLF areas, then scan, read, write and commit | No sanitizer report; callbacks never see an address outside the area, and nothing reads outside it without them |
| Fault-injecting callbacks | Case 2, T2.2 | The n-th callback returns an error or different data on each read | Same as above, with errors raised inside transactions |

Also worth doing: a bounded model check (CBMC or Frama-C) of `check_in_data()`, `is_in_range()`, the tracker indexing and the EWLF record placement in `find_current_record()` and `write_ewlf_core()`, which carry most of the memory-safety burden. In CI, build with `-Wconversion`, since `offset_wu` is a `uint32_t` derived from a `uintptr_t`. The ASan harness can seed the API fuzzer.

## Sources

- [lean-ftl source](https://github.com/sebastien-riou/lean-ftl), `liblean-ftl/` at commit 426fcb1 (2026-10-22, branch `main`), Apache-2.0
- `EWLF.md` and `ISSUES.md` in the same repository, at the same commit
- [lean-ftl API reference](https://lean-ftl.readthedocs.io/en/latest/api.html)
- [lean-ftl user guide](https://lean-ftl.readthedocs.io/en/latest/using.html)
- [lean-ftl internals](https://lean-ftl.readthedocs.io/en/latest/internal.html)
- [NLnet project page, LeanFTL](https://nlnet.nl/project/LeanFTL/)
