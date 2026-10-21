# lean-ftl Threat Model

Oct 22, 2026 · @Sebastien Riou

At commit 5517f9a, lean-ftl meets both objectives for regular and EWLF areas, within the assumptions below: the twelve findings of this model are fixed, each with a regression test in the repository's test suite. NVM content only picks among fixed slot or record positions inside an area, API arguments are range-checked without overflow, an error aborts the transaction it interrupts, bad configurations are refused at registration, and EWLF writes only program erased cells. What remains is by design or up to the integrator: `lftl_memread()` and write sources accept any pointer (T1.7), messages can drive wear-out (T1.5), `lftl_format()` is not anti-tearing (T1.6), and external NVM needs a careful driver and careful parsing (T2.3 to T2.8).

> **Revision.** First written against commit d8f182f, then updated to 501de63, where LF-1 to LF-5 and LF-8 to LF-12 were open. This version covers 5517f9a, where all of them are fixed; the findings tables give each fix commit. ISSUES.md LFTL-001 to LFTL-003, found along the way, are fixed too.

## Scope and system overview

This model covers the core library, `liblean-ftl/source/ftl.c` and `liblean-ftl/source/ftl_internal.h`, at [commit 5517f9a](https://github.com/sebastien-riou/lean-ftl/tree/5517f9a) (2026-10-22, branch `dev/ewlf`), as integrated on a bare-metal MCU. Example code, the STM32 callbacks under `target/` and the test harness are out of scope, except where the integrator contract depends on them.

How lean-ftl works, as far as security is concerned:

- An **area** is split into `n_slots = area_size / slot_size` slots. One slot holds the current data; the next one is erased and rewritten on every update.
- Each slot ends with 3 write units of metadata: `version`, `checksum` and `checksum2`. The checksum is CRC32C over the data plus the version.
- On first access, `find_current_slot()` scans all slots, skips erased versions (`0xFFFFFFFF`) and slots failing the checksum, and picks the highest version. `ctx->data` is set from the **loop index**, not from NVM content. If that slot's `checksum2` is torn, its data is copied into the next slot with fresh metadata.
- An **EWLF area** (`lftl_register_area_ewlf()`) packs many **records** into each erase unit. A record is a magic, a `prev_erased` flag, the same three metadata items, then the data. Writes go downward from the top of the highest erase unit, each to the first record position whose cells all read erased. When a unit is full, the next record goes to the top of the unit below, wrapping from the lowest unit back to the highest; then the vacated unit is erased and the new record's `prev_erased` flag is set. EWLF areas have no transactions.
- On first access to an EWLF area, `find_current_record()` checks only the positions where records can start (the top of each erase unit minus a multiple of the record size), by increasing address. It takes the first valid record, or the top record of the next unit in write order if that one is newer (a wrap interrupted before the lowest unit was erased). `ctx->data` is set from that position: NVM content picks among fixed positions inside the area, but never sets where a record starts.
- The application addresses data through **logical addresses** inside the `nvm` struct (`nvm.a.data0`). `translate_addr()` maps them to the current slot or record after an overflow-safe range check, and the transaction functions run the same check first.
- Transactions (regular areas only) use a caller-owned RAM bitmap (`transaction_tracker`, one bit per write unit) to detect double writes and to know which units to copy at commit.
- `lftl_register_area()` and `lftl_register_area_ewlf()` check the configuration (write unit size, area bounds and alignment, slot count, EWLF record size) and raise `LFTL_ERROR_BAD_CONFIG` before linking a bad area.
- NVM access goes through integrator callbacks (`erase`, `write`, `read`), set once per NVM in `lftl_nvm_props_t`. The exception is `mem_read()`, which copies any address outside every registered NVM with `memcpy`; that is how RAM `src` buffers are read. Errors go to the NVM's `error_handler`, after the transaction of the area the failing operation was for is aborted. The handler must not return; if it does, the library loops forever.

| Asset | Why it matters | Case 1 | Case 2 |
| --- | --- | --- | --- |
| Internal RAM and the CPU's address space | Corruption leads to code execution or secret disclosure | Protect | Protect (sole objective) |
| Firmware and other non-LFTL flash | Erasing or overwriting it bricks the device or plants code | Protect | Protect |
| Integrity of committed LFTL data | Application state, counters, configuration | Protect | Not protected |
| Freshness (no rollback to an older version) | Anti-replay counters, revocation state | Protect | Not protected |
| Availability of the area | Device must keep booting | Protect against logical attacks | Not protected |
| Confidentiality of LFTL data | Secrets stored in NVM | Out of scope (no physical access) | Not protected |
|  |  |  |  |
|  |  |  |  |

## Assumptions and out of scope

Both cases rest on the same baseline: the code, the configuration and the callbacks are trusted; only runtime inputs are hostile.

**Assumed in both cases**

- Firmware, the lean-ftl configuration (`lftl_nvm_props_t`, every `lftl_ctx_t`) and the callbacks are trusted and correct. `write_size`, `erase_size`, `area` and `data_size` are compile-time constants, never derived at runtime from the NVM (for example from SFDP tables). Registration checks that the configuration is consistent, but cannot tell whether it matches the hardware.
- `error_handler` never returns: it resets, or `longjmp`s to a recovery point. The library loops forever if it does return (LF-7), so a returning handler costs availability instead of turning each check into a no-op.
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

The transaction and anti-rollback logic holds against power cuts for both kinds of area, and so does the memory-safety objective. Out-of-range arguments now end in the error handler before any access (LF-1 to LF-3). An error ends the transaction it interrupts (LF-4). Configurations that would overflow buffers or write outside the area are refused (LF-5, LF-11).

The attacker can only send messages to the application. They reach lean-ftl through the arguments the application derives from those messages (`dst_nvm_addr`, `src`, `size`), through the sequence of calls (start, write, commit, abort, duplicates), through the content of data the application stores for them, and through the timing of power cuts. lean-ftl is the last line of defence: its documented checks (`FIRST_NOT_IN_DATA`, `LAST_NOT_IN_DATA`, `TRANSACTION_OVERWRITE`) are expected to stop an application bug that forwards a bad offset or a duplicated record.

### Security objectives

| Objective | Status | Basis |
| --- | --- | --- |
| O1. Atomicity: after any power cut, reads return either the old or the new committed data | Holds | Regular areas: writes go to the erased next slot; metadata is written last, `checksum2` after `version` and `checksum`. A torn slot fails CRC32C and is skipped. A torn `checksum2` is handled at the next scan by copying the data into the next slot with fresh metadata, which only programs erased cells (ISSUES.md LFTL-001, fixed in 2abc78d). EWLF areas: a record is written data first, then magic, metadata and `checksum2`, only into cells that read erased, and its checksum is computed from the data meant to be written. A torn record is skipped by the scan and by the next write, and an interrupted wrap is completed at the next scan (LF-9). Only record positions are checked, so data cannot pass for a record (LF-10). Detection of a torn data write is probabilistic (CRC32C, 2^-32). |
| O2. No rollback below the last committed version | Holds | `version` is incremented on every commit and the highest valid version wins. Wrap at `0xFFFFFFFE` needs about 4.3 billion updates, far beyond endurance (native endurance × wear-leveling factor). EWLF areas take the first valid record by address, or the top record of the next unit in write order if it is newer; an interrupted write can bring back the previous version, never an older one. |
| O3. No bricking through logical actions | Partly | Wear-out and a torn `lftl_format` can leave the device unusable; see threats T1.5 and T1.6. |
| O4. No access to unrelated RAM or flash, no code execution | Holds | Given the integrator contract below, T1.7 in particular. LF-1 to LF-5, LF-8 and LF-11 are fixed. |

### Threats

| ID | Threat | Mitigation in lean-ftl | Residual risk |
| --- | --- | --- | --- |
| T1.1 | Out-of-range destination or size reaches a write | Overflow-safe range check (`check_in_data()`) before the tracker or any NVM access | None (LF-1, LF-2 fixed) |
| T1.2 | Out-of-range source or size reaches a read | Same check in `lftl_read()` and `lftl_transaction_read()` | None (LF-2, LF-3 fixed) |
| T1.3 | Error raised mid-transaction leaves stale state | The error aborts the area's transaction before the handler runs | An error on another area leaves this area's transaction open. If the handler unwinds the frame holding its tracker, the recovery path must abort it (LF-4 fixed). |
| T1.4 | Reading uncommitted data and acting on it | `lftl_read()` returns committed data during a transaction | Application must not base security decisions on `lftl_read_newer()` or `lftl_transaction_read()` results; the transaction may still abort. |
| T1.5 | Wear-out driven by messages | None | Each `lftl_basic_write()` to a regular area erases a full slot, and so does each `lftl_transaction_start()`, even if aborted. A loop of start/abort burns endurance. EWLF areas erase one unit per unit's worth of records. The application must rate-limit or batch writes. |
| T1.6 | Power cut during `lftl_format()` | Documented as not anti-tearing | The next scan raises `NO_VALID_VERSION` on every boot. The application must keep its own "formatted" flag in another area and recover. |
| T1.7 | Arbitrary-pointer gadgets | None, by design | `lftl_memread()` copies from any address, and the `src` of every write may be any address. An attacker-controlled pointer reaching either one reads arbitrary memory, or copies it into NVM to read back later. Only pass pointers the application built itself. |
| T1.8 | Stale data after logical erase | Documented | `lftl_erase_all()` leaves previous versions in other slots, and in EWLF areas, older records stay until their erase unit is recycled. Any raw NVM read (for example `lftl_memread()` outside the area) can still return them. |
| T1.9 | First operation on an area after a reset | `translate_addr()` and `lftl_transaction_start()` locate the current slot on first use | None (LF-8 fixed) |
| T1.10 | Power cut during an EWLF write | Records are written only into erased cells; the scan and the next write skip torn records; an interrupted wrap is completed at the next scan | None (LF-9 fixed) |
| T1.11 | Attacker-chosen data stored in an EWLF area | Only record positions are checked | None (LF-10 fixed) |

### Findings

Every finding was reproduced at 501de63, on a host build with AddressSanitizer and UndefinedBehaviorSanitizer, by `lftl_asan_harness.c`. The harness uses RAM-backed callbacks and an `error_handler` that `longjmp`s, as the documentation recommends; for LF-9 and LF-10 it also cuts power at chosen writes or erases, and models NOR flash or STM32U5-class flash. Since 5517f9a, every harness test ends in the error handler or reads back the newest data, with no sanitizer report. Each finding also has a regression test in `liblean-ftl-test/source/test.c` (test numbers below), which fails without its fix. These tests run on the Linux NVM simulator, whose write callback refuses to program a write unit that is not erased. The issue column describes 501de63, and its line numbers refer to that commit.

| ID | Issue at 501de63 | Trigger | Impact | Fixed in | Regression test |
| --- | --- | --- | --- | --- | --- |
| LF-1 | `lftl_transaction_write()` and `lftl_transaction_write_any()` index the tracker bitmap from `dst_nvm_addr - area` before any range check | `dst_nvm_addr` outside the area during a transaction | Read and bit-set write in RAM at an offset chosen by the caller, before `FIRST_NOT_IN_DATA` is raised (heap-buffer-overflow, `ftl.c:123`) | e99b578: range check before the tracker is touched | 18: guard bytes after the tracker stay untouched |
| LF-2 | `translate_addr_std()` and `translate_addr_ewlf()` check `offset + size > data_size`, which wraps for large `size` | `size` close to `UINTPTR_MAX`, for example `-offset` | Reads of up to the whole address space into `dst`; writes the same size into flash, including firmware | 13f188b: `check_in_data()`, also before writes extend the range to whole write units | 18 |
| LF-3 | `lftl_transaction_read()` has no range check at all; `lftl_read_newer()` and `lftl_memread_newer()` call it during a transaction | Out-of-range `src` or `size` during a transaction | Reads arbitrary addresses relative to the slot base, overflows `dst` (stack-buffer-overflow, via `ftl.c:222`) | bae149a: range check, and unaligned reads return the right bytes | 18, 4 |
| LF-4 | When `error_handler` unwinds a transaction, `ctx->transaction_tracker` still points at the old buffer | A batch message with a duplicated record raises `TRANSACTION_OVERWRITE`; any later `lftl_write()` is routed to `lftl_transaction_write()` | Bit-set writes into a dead stack frame or freed buffer (heap-use-after-free, `ftl.c:123`) | 81cd09d: an error, NVM errors included, aborts the area's transaction before the handler runs | 15, 20 |
| LF-5 | `meta_items_worst_case_t` holds 16-byte write units, but `LFTL_WU_MAX_SIZE` allows 128 bytes, and is checked only in `lftl_format()`. Regular and EWLF metadata both use it. | `write_size` above 16, for example a 32-byte flash word on STM32H7 | Stack overflow on every metadata read and write | a377ea5: buffers sized from the write unit; 2f2b654 checks `LFTL_WU_MAX_SIZE` at registration | 26 |
| LF-6 | `is_in_range()` treated `base + size` as inside | Address exactly at the end of an area or NVM | None alone; it widened LF-2 and misrouted `lftl_memread()` by one byte | ef29ff3 (before 501de63) | — |
| LF-7 | Every check relied on `error_handler` not returning | Integrator handler that logs and returns | Each check became a no-op; for example `NO_VALID_VERSION` then set `ctx->data` to `slot_base(ctx, 0xFFFFFFFF)` | b3ed55b (before 501de63): `raise_nvm_error()` loops forever after the handler | — |
| LF-8 | `lftl_transaction_start()` computes the next slot from `ctx->data` without locating the current slot first. Also present at d8f182f. | `lftl_transaction_start()` is the first call on an area after a reset | `next_slot()` works from `LFTL_INVALID_POINTER` and erases a slot outside the area. On a 32-bit MCU the address wraps into `[0, slot_size)`: the STM32 reference callbacks reject it and leave the tracker set, and the CH32V307 one folds it into flash and erases the start of the firmware. | cf6c304 (ISSUES.md LFTL-003): the current slot is located first, and the tracker is set only once the erase has succeeded | 25, 21 |
| LF-9 | EWLF writes the next record without checking that its cells are erased (ISSUES.md LFTL-002) | Power cut during a record write, or during the wrap from the lowest unit to the highest, before the lowest is erased | NOR flash: the new record holds old AND new data and passes its checksum, which is computed from flash after the write; after an interrupted wrap, `NO_VALID_VERSION` at the next reset. Flash that cannot reprogram a cell: every later write fails, and the area stays read-only. | 5517f9a: writes go to erased cells only, a dirty unit is erased before a rollover, an interrupted wrap is completed at the next scan, and the checksum is computed from the intended data | 29 |
| LF-10 | `find_current_record()` looks for a header at every 8-byte step, including inside a torn record's data | The attacker controls at least a header's worth of stored data (40 bytes with 8-byte write units, 80 with 16) and cuts power between the data write and the magic | Reads return a forged record, attacker-chosen and different from both the old and the new data | c51b414: only record positions are checked | 28 |
| LF-11 | An EWLF record larger than an erase unit is accepted. `LFTL_EWLF_RECORD_SIZE` leaves out the magic, the `prev_erased` flag and the padding, so `LFTL_AREA_EWLF` accepts content that does not fit. | Configuration, for example 192 bytes of data with 16-byte write units on 256-byte pages | `ewlf_unit_top_record()` returns an address below its unit; on the wrap to the lowest unit, the record starts below the area | 2f2b654: the macro matches the layout, `LFTL_AREA_EWLF` refuses such content at compile time, and registration checks it | 27 |

LF-5 and LF-11 were triggered by configuration, not by an attacker. They are listed because they silently corrupted memory on common targets.

## Case 2: external NVM

The library's own handling of hostile NVM content meets the objective for both kinds of area: NVM content only picks among fixed slot or record positions inside the area, and never sets an address, index or size. LF-12, where EWLF content made the scan read past the area, is fixed. So is LF-4, which a bus error inside a transaction could trigger (T2.2). The integrator's driver and application parsing carry the remaining risk.

The attacker reads and writes the external NVM at any time, including between two reads made by the same function, and controls every bus response. Integrity, freshness, confidentiality and availability of LFTL data are explicitly **not** goals. The only objective is:

> **O5.** No sequence of NVM contents or bus responses gives the attacker read or write access to the internal address space, or code execution.

### What the library reads from NVM, and what it does with it

| Value read from NVM | Used for | Can it become an address, index or size? | What the attacker can do with it |
| --- | --- | --- | --- |
| `version` (per slot or record) | Choosing the current slot, or between an EWLF record and the top record of the next unit; `+1` for the next version | No. The slot index or record position comes from the scan loops, so `ctx->data` always points at a slot or record of the configured area | Pick any slot or record; force `VERSION_COLLISION`; force `NO_VALID_VERSION`. Setting `0xFFFFFFFE` makes the next commit write `0xFFFFFFFF`, which the next scan treats as erased |
| `checksum` | Slot or record validity check | No | Forge freely; CRC32C is not keyed |
| `checksum2` | Regular areas: detecting a torn metadata write. EWLF areas: skipping a torn record | No | Force a slot erase and rewrite on every scan (external flash wear) |
| EWLF magic | Recognizing a record at a record position | No. Only record positions are checked, and each lies within its erase unit | Make any record position look valid or not, as with any record content |
| EWLF `prev_erased` flag | Deciding whether to re-erase the previous unit | No. The unit comes from the record's position, modulo the number of units | Force one unit erase per boot (external flash wear) |
| Erased state of EWLF record cells | Choosing where the next record goes, and whether to erase a unit before a rollover | No. The record stays on a record position of its unit | Force an erase on every write (external flash wear) |
| Data bytes | Copied slot to slot at commit, copied to caller buffers | No. Copy sizes come from the configuration or the caller | Any content, changed between reads |
| Callback status codes | Raising `LOW_LEVEL_*` errors | No | Raise an error inside any operation, at a chosen point; it aborts that area's transaction |

The library also re-reads metadata several times during a scan (checksum check, then version, then `checksum2`). A time-of-check/time-of-use (TOCTOU) change between those reads only changes which data is returned. That is a data-integrity effect, which is accepted.

### Threats

| ID | Threat | Status | Required mitigation |
| --- | --- | --- | --- |
| T2.1 | Hostile metadata forces an error path | Availability only | The library loops forever if the handler returns (LF-7), so `NO_VALID_VERSION` cannot produce a wild pointer. The handler must still reset or `longjmp`; one that returns hangs the device. |
| T2.2 | Bus error inside a transaction | Fixed | The error aborts that area's transaction before the handler runs (LF-4). See T1.3 for transactions on other areas. |
| T2.3 | Application parses NVM data as trusted | Integrator | Treat every byte read from an external area as an untrusted packet: bound every length and index against compile-time constants, store no pointers or function pointers, and validate enums and state values. |
| T2.4 | Double fetch by the application | Integrator | Read once into RAM with `lftl_read()`, validate the RAM copy and use only that copy. Never read a length field and then the payload in two calls, and never dereference the `nvm` struct directly. |
| T2.5 | Driver trusts the device | Integrator | Translate each virtual address to a device offset with a bounds check against the window. Never take sizes from the device (SFDP geometry, length fields). Bound every busy-wait loop with a timeout. |
| T2.6 | Write callback dereferences a virtual address | Integrator | lean-ftl copies slot to slot, so the write callback's `src` may lie inside the NVM window. For SPI flash that is not memory-mapped, the callback must read it through the device into a bounce buffer, never as a RAM pointer. |
| T2.7 | Virtual window overlaps real memory | Integrator | `nvm_props.base` and `size` decide whether an address goes to the callback or to `memcpy`. Place the window in unmapped address space so a stray dereference faults, and keep it apart from RAM, peripherals and internal flash. |
| T2.8 | Memory-mapped external flash (XIP, OctoSPI) | Integrator | Content can change between two CPU loads. Mark the region non-executable in the MPU, and make the bus-fault handler reset rather than resume. |
| T2.9 | Forced reformat or wear | Accepted | If the "formatted" flag lives in external NVM, or the application reformats on `NO_VALID_VERSION`, the attacker can wipe the data. `lftl_format()` erases a range taken from the configuration, so this path stays memory-safe. |
| T2.10 | Hostile EWLF content | Fixed | Only record positions are read, each within its erase unit, so the scan never reads past the area (LF-12). |

### Findings

| ID | Issue at 501de63 | Trigger | Impact | Fixed in | Regression test |
| --- | --- | --- | --- | --- | --- |
| LF-12 | `find_current_record()` accepts a magic at any 8-byte boundary up to the last 8 bytes of the area, then reads the metadata and `data_size` bytes from there without checking that the record fits | A magic in the last record's worth of an EWLF area | Reads up to one record past the area: the metadata through the read callback, the data through `mem_read()`, which uses `memcpy` outside every registered NVM. If the CRC also matches, `ctx->data` points at a record that runs past the area, and `lftl_read()` returns those bytes. | c51b414: only record positions are checked | 28: a read callback flags any read outside the area |

LF-1, LF-2, LF-3, LF-5 and LF-11 were driven by API arguments or configuration, not by NVM content, so they applied to Case 2 exactly as in Case 1. LF-9 and LF-10 gave a Case 2 attacker nothing that writing the NVM directly does not.

## Integrator responsibilities and hardening

The library cannot check the contract below itself. Each item is needed in both cases unless marked.

**Contract checklist**

- [ ] `error_handler` never returns: it resets, or `longjmp`s to a recovery point. One that returns hangs the device (LF-7).
- [ ] An error aborts only the transaction of the area it happened on. If the recovery point unwinds a frame holding another area's tracker, it calls `lftl_transaction_abort()` on that area before any further lean-ftl call.
- [ ] `transaction_tracker` lives in static storage or in a frame the handler cannot unwind, and is at least `LFTL_TRANSACTION_TRACKER_SIZE(ctx)` bytes.
- [ ] Before calling lean-ftl, the application checks every offset and size it derived from a message against the field it targets, overflow-safe: `off <= field_size && len <= field_size - off`. lean-ftl raises an error instead of corrupting memory, but with a handler that resets, each bad message would become a reboot.
- [ ] Batch messages are de-duplicated before a transaction, so `TRANSACTION_OVERWRITE`, which aborts the transaction, cannot be triggered from outside.
- [ ] No attacker-influenced pointer reaches `lftl_memread()` or the `src` of any write (T1.7).
- [ ] Message-driven writes are rate-limited and batched against the endurance budget: native endurance × wear-leveling factor per page (T1.5).
- [ ] A "formatted" flag in a separate area lets the application recover from a torn `lftl_format()` (T1.6).
- [ ] Case 2 only: the driver, the address window and the application parsing follow T2.3 to T2.8.

**Defence in depth: bound the callbacks.** The cheapest high-value hardening is to make `erase`, `write` and `read` reject any range outside the LFTL areas (plus, for `write`'s `src`, the RAM and NVM ranges the application uses). This turns any future bug that reaches flash outside an area into an error. On parts with an MPU or TrustZone, also make the LFTL areas non-executable, and make firmware flash write-protected while lean-ftl runs.

**If Case 2 needs integrity or freshness after all.** lean-ftl cannot provide either, but it composes with a small wrapper:

1. Encrypt and authenticate each record with an AEAD (for example AES-GCM or ChaCha20-Poly1305), with a key kept on chip.
2. Bind the associated data to the area identifier and a record version, so records cannot be swapped between areas or fields.
3. Keep the version counter in an **internal** lean-ftl area, which is Case 1 storage, to detect rollback of the external copy.
4. Verify the tag over the RAM copy (T2.4) before parsing anything.

This is the same pattern TF-M Protected Storage uses over its internal trusted storage.

## Verification plan and open questions

The regression tests above run in `testit` with the tearing simulation, on a simulator that refuses to program cells that are not erased. Three fuzzing harnesses on the host build, under AddressSanitizer and UndefinedBehaviorSanitizer, would widen the coverage beyond the known findings, and the tearing sweep has one known gap.

| Harness | Covers | Inputs | Pass criterion |
| --- | --- | --- | --- |
| API sequence fuzzer (libFuzzer) | Case 1, O4 | Random sequences of start, write, write\_any, read, read\_newer, commit, abort, erase\_all, with random offsets and sizes, on regular and EWLF areas, including sequences that begin right after a reset; the handler `longjmp`s and recovery continues | No sanitizer report; every out-of-range call ends in the handler |
| NVM image fuzzer | Case 2, O5 | Mutated images of regular and EWLF areas, then scan, read, write and commit | No sanitizer report; callbacks never see an address outside the area, and nothing reads outside it without them |
| Fault-injecting callbacks | Case 2, T2.2 | The n-th callback returns an error or different data on each read | Same as above, with errors raised inside transactions |
| Existing tearing sweep | Case 1, O1 and O2 | Power cut at every callback boundary. After a tear, the sweep only reads every area back and reformats; writing to every area after recovery would cover what tests 24 and 29 check at chosen points only. A write callback that ANDs bits would also show silent corruption. | After restart, data equals the last or the previous committed version, never older |

Also worth doing: a bounded model check (CBMC or Frama-C) of `check_in_data()`, `is_in_range()`, the tracker indexing and the EWLF record placement in `find_current_record()` and `write_ewlf_core()`, which carry most of the memory-safety burden. In CI, build with `-Wconversion`, since `offset_wu` is a `uint32_t` derived from a `uintptr_t`.

`lftl_asan_harness.c` (about 290 lines) reproduces the findings at 501de63 and checks the fixed behaviour since 5517f9a: an 8-page RAM-backed NVM with 256-byte pages, one area per test, `memcpy` callbacks that report any range outside the area, a `longjmp` handler, switchable NOR and STM32U5 write semantics, and power cuts at a chosen write or erase. Build and run instructions are in its header. It can seed the API fuzzer.

**Open questions for the maintainer**

- Would upstream accept bounds checks against the LFTL areas in the reference callbacks, as defence in depth for future bugs? The CH32V307 one folds any address below `FLASH_BASE` into flash, to support areas addressed through the `0x00000000` alias, which turned LF-8 into an erase of the firmware's first pages; a bound there would have to come from the LFTL areas.
- `LFTL_AREA` counts the metadata of 1-byte and 2-byte write units as 1 and 2 bytes per item, while the run time uses 4. When that crosses a page boundary, an area gets fewer slots than its wear-leveling factor, and registration now refuses it if fewer than 2 remain. Should the macro count 4 bytes, at the cost of moving the layout of deployed areas in those configurations?

## Sources

- [lean-ftl source](https://github.com/sebastien-riou/lean-ftl), core files `liblean-ftl/source/ftl.c` and `liblean-ftl/source/ftl_internal.h` at commit 5517f9a (2026-10-22, branch `dev/ewlf`), Apache-2.0
- `EWLF.md` and `ISSUES.md` in the same repository, at the same commit
- [lean-ftl API reference](https://lean-ftl.readthedocs.io/en/latest/api.html)
- [lean-ftl user guide](https://lean-ftl.readthedocs.io/en/latest/using.html)
- [lean-ftl internals](https://lean-ftl.readthedocs.io/en/latest/internal.html)
- [NLnet project page, LeanFTL](https://nlnet.nl/project/LeanFTL/)
