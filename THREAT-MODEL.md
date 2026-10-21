# lean-ftl Threat Model

Oct 21, 2026 · @Sebastien Riou

For regular areas, lean-ftl's design largely meets both objectives, because nothing read from NVM is used as a pointer, index or size. The gaps there are in argument validation, error recovery and configuration: five open memory-safety issues reproduce under AddressSanitizer, none from NVM content. EWLF areas, added since the first version of this model, lack that property: NVM content decides where the current record starts, nothing bounds where it ends, and the write path assumes the space for the next record is erased. That gives an NVM-driven read past the area, a record an attacker can forge with one power cut, and a power-loss bug that silently corrupts an EWLF area or blocks it for good.

> **Revision.** Updated from commit d8f182f to 501de63. LF-6 and LF-7 are fixed. LF-8 (regular areas) and LF-9 to LF-12 (EWLF areas) are new. `lftl_asan_harness.c` was ported to the new callback API and reproduces every open finding. LF-8 has been fixed since, as has the tearing issue tracked as LFTL-001 in ISSUES.md.

## Scope and system overview

This model covers the core library, `liblean-ftl/source/ftl.c` and `liblean-ftl/source/ftl_internal.h`, at [commit 501de63](https://github.com/sebastien-riou/lean-ftl/tree/501de63) (2026-08-31, branch `dev/ewlf`), as integrated on a bare-metal MCU. Example code, the STM32 callbacks under `target/` and the test harness are out of scope, except where the integrator contract depends on them.

How lean-ftl works, as far as security is concerned:

- An **area** is split into `n_slots = area_size / slot_size` slots. One slot holds the current data; the next one is erased and rewritten on every update.
- Each slot ends with 3 write units of metadata: `version`, `checksum` and `checksum2`. The checksum is CRC32C over the data plus the version.
- On first access, `find_current_slot()` scans all slots, skips erased versions (`0xFFFFFFFF`) and slots failing the checksum, and picks the highest version. `ctx->data` is set from the **loop index**, not from NVM content.
- An **EWLF area** (`lftl_register_area_ewlf()`) packs many **records** into each erase unit. A record is a magic, a `prev_erased` flag, the same three metadata items, then the data. Writes go downward from the top of the highest erase unit. When a unit is full, the next record goes to the top of the unit below, wrapping from the lowest unit back to the highest; then the vacated unit is erased and the new record's `prev_erased` flag is set. EWLF areas have no transactions.
- On first access to an EWLF area, `find_current_record()` scans upward from the area base in 8-byte steps and takes the first record whose magic and checksum match. `ctx->data` is set from the **scan position**, so NVM content decides where the record starts.
- The application addresses data through **logical addresses** inside the `nvm` struct (`nvm.a.data0`). `translate_addr()` maps them to the current slot or record after a range check.
- Transactions (regular areas only) use a caller-owned RAM bitmap (`transaction_tracker`, one bit per write unit) to detect double writes and to know which units to copy at commit.
- NVM access goes through integrator callbacks (`erase`, `write`, `read`), set once per NVM in `lftl_nvm_props_t`. The exception is `mem_read()`, which copies any address outside every registered NVM with `memcpy`; that is how RAM `src` buffers are read, and checksums go through it too. Errors go to the NVM's `error_handler`, which must not return; if it does, the library loops forever.

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

- Firmware, the lean-ftl configuration (`lftl_nvm_props_t`, every `lftl_ctx_t`) and the callbacks are trusted and correct. `write_size`, `erase_size`, `area` and `data_size` are compile-time constants, never derived at runtime from the NVM (for example from SFDP tables).
- `error_handler` never returns: it resets, or `longjmp`s to a recovery point. Since b3ed55b the library loops forever if it does return (LF-7), so a returning handler costs availability instead of turning each check into a no-op.
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

For regular areas, the transaction and anti-rollback logic holds against power cuts. For EWLF areas it does not: one power cut can corrupt the area or block it for good (LF-9), and an attacker who also controls stored data can forge a record (LF-10). The memory-safety objective holds for neither kind of area, because API gaps (LF-1 to LF-4) let a caller corrupt RAM or flash before or instead of the error handler firing.

The attacker can only send messages to the application. They reach lean-ftl through the arguments the application derives from those messages (`dst_nvm_addr`, `src`, `size`), through the sequence of calls (start, write, commit, abort, duplicates), through the content of data the application stores for them, and through the timing of power cuts. lean-ftl is the last line of defence: its documented checks (`FIRST_NOT_IN_DATA`, `LAST_NOT_IN_DATA`, `TRANSACTION_OVERWRITE`) are expected to stop an application bug that forwards a bad offset or a duplicated record.

### Security objectives

| Objective | Status | Basis |
| --- | --- | --- |
| O1. Atomicity: after any power cut, reads return either the old or the new committed data | Holds for regular areas; **does not hold for EWLF areas** | Regular areas: writes go to the erased next slot; metadata is written last, `checksum2` after `version` and `checksum`. A torn slot fails CRC32C and is skipped. A torn `checksum2` is handled at the next scan by copying the data into the next slot with fresh metadata. At 501de63 it was reprogrammed in place instead, which STM32U5-class flash refuses (ISSUES.md LFTL-001, fixed since). Detection of a torn data write is probabilistic (CRC32C, 2^-32). EWLF areas: the write after a torn write lands on the torn cells (LF-9), and a torn record's data can pass for a record (LF-10). |
| O2. No rollback below the last committed version | Holds | `version` is incremented on every commit and the highest valid version wins. Wrap at `0xFFFFFFFE` needs about 4.3 billion updates, far beyond endurance (native endurance × wear-leveling factor). EWLF areas take the lowest-address valid record instead of the highest version; an interrupted write can bring back the previous version, never an older one. |
| O3. No bricking through logical actions | Partly | Wear-out and a torn `lftl_format` can leave the device unusable; see threats T1.5 and T1.6. On flash that cannot reprogram a cell, one power cut leaves an EWLF area read-only for good; on NOR flash it can end in `NO_VALID_VERSION` (LF-9). |
| O4. No access to unrelated RAM or flash, no code execution | **Does not hold** | LF-1 to LF-5 and LF-11 below. |

### Threats

| ID | Threat | Mitigation in lean-ftl | Residual risk |
| --- | --- | --- | --- |
| T1.1 | Out-of-range destination or size reaches a write | Range check in `translate_addr()` | LF-1, LF-2 |
| T1.2 | Out-of-range source or size reaches a read | Range check in `lftl_read()` | LF-2, LF-3 |
| T1.3 | Error raised mid-transaction leaves stale state | None | LF-4 |
| T1.4 | Reading uncommitted data and acting on it | `lftl_read()` returns committed data during a transaction | Application must not base security decisions on `lftl_read_newer()` or `lftl_transaction_read()` results; the transaction may still abort. |
| T1.5 | Wear-out driven by messages | None | Each `lftl_basic_write()` to a regular area erases a full slot, and so does each `lftl_transaction_start()`, even if aborted. A loop of start/abort burns endurance. EWLF areas erase one unit per unit's worth of records, but see LF-9. The application must rate-limit or batch writes. |
| T1.6 | Power cut during `lftl_format()` | Documented as not anti-tearing | The next scan raises `NO_VALID_VERSION` on every boot. The application must keep its own "formatted" flag in another area and recover. |
| T1.7 | Arbitrary-pointer gadgets | None, by design | `lftl_memread()` copies from any address, and the `src` of every write may be any address. An attacker-controlled pointer reaching either one reads arbitrary memory, or copies it into NVM to read back later. Only pass pointers the application built itself. |
| T1.8 | Stale data after logical erase | Documented | `lftl_erase_all()` leaves previous versions in other slots, and in EWLF areas, older records stay until their erase unit is recycled. Any raw NVM read (for example `lftl_memread()` outside the area) can still return them. |
| T1.9 | First operation on an area after a reset | `translate_addr()` locates the current slot on first use, and so does `lftl_transaction_start()` | None. Until LF-8 was fixed, `lftl_transaction_start()` skipped it. |
| T1.10 | Power cut during an EWLF write | Data is written first, then magic, metadata and `checksum2`; the scan skips incomplete records | LF-9: the next write targets the same cells without checking that they are erased. |
| T1.11 | Attacker-chosen data stored in an EWLF area | None | LF-10: with one power cut, the data can pass for a record. Until it is fixed, keep attacker-influenced data out of EWLF areas, or treat what is read back as untrusted (T2.3). |

### Findings in the current code

All open findings reproduce on a host build with AddressSanitizer and UndefinedBehaviorSanitizer, in `lftl_asan_harness.c` (test numbers below). The harness uses RAM-backed callbacks and an `error_handler` that `longjmp`s, as the documentation recommends. For LF-9 and LF-10 it also cuts power by dropping a chosen write or erase, and for LF-9 its write callback models NOR flash (programming ANDs bits) or STM32U5-class flash (a non-erased cell cannot be programmed).

| ID | Issue | Trigger | Impact | Status (test) | Suggested fix |
| --- | --- | --- | --- | --- | --- |
| LF-1 | `lftl_transaction_write()` and `lftl_transaction_write_any()` index the tracker bitmap from `dst_nvm_addr - area` before any range check | `dst_nvm_addr` outside the area during a transaction | Read and bit-set write in RAM at an offset chosen by the caller, before `FIRST_NOT_IN_DATA` is raised | Open (T1): heap-buffer-overflow, `ftl.c:123` | Validate `dst` and `size` against the area, overflow-safe, before touching the tracker |
| LF-2 | `translate_addr_std()` and `translate_addr_ewlf()` check `offset + size > data_size`, which wraps for large `size` | `size` close to `UINTPTR_MAX`, for example `-offset` | Reads of up to the whole address space into `dst`; writes the same size into flash, including firmware | Open (T2): memcpy with size -16 | `if (size > data_size - offset)` |
| LF-3 | `lftl_transaction_read()` has no range check at all; `lftl_read_newer()` and `lftl_memread_newer()` call it during a transaction | Out-of-range `src` or `size` during a transaction | Reads arbitrary addresses relative to the slot base, overflows `dst` | Open (T3): stack-buffer-overflow in memcpy, via `ftl.c:222` | Same check as `lftl_read()` |
| LF-4 | When `error_handler` unwinds a transaction, `ctx->transaction_tracker` still points at the old buffer | A batch message with a duplicated record raises `TRANSACTION_OVERWRITE`; any later `lftl_write()` is routed to `lftl_transaction_write()` | Bit-set writes into a dead stack frame or freed buffer | Open (T5): heap-use-after-free, `ftl.c:123` | Clear `transaction_tracker` before calling the handler on transaction errors; integrators call `lftl_transaction_abort()` on every area in their recovery path |
| LF-5 | `meta_items_worst_case_t` holds 16-byte write units, but `LFTL_WU_MAX_SIZE` allows 128 bytes, and is checked only in `lftl_format()`. Regular and EWLF metadata both use it. | `write_size` above 16, for example a 32-byte flash word on STM32H7 | Stack overflow on every metadata read and write | Open (T4): stack-buffer-overflow, `write_meta_core` | Size the buffer from `LFTL_WU_MAX_SIZE`, or reject `write_size > 16`, and check it at `lftl_register_area()` and `lftl_register_area_ewlf()` |
| LF-6 | `is_in_range()` treated `base + size` as inside | Address exactly at the end of an area or NVM | None alone; it widened LF-2 and misrouted `lftl_memread()` by one byte | **Fixed** in ef29ff3 | — |
| LF-7 | Every check relied on `error_handler` not returning | Integrator handler that logs and returns | Each check became a no-op; for example `NO_VALID_VERSION` then set `ctx->data` to `slot_base(ctx, 0xFFFFFFFF)` | **Fixed** in b3ed55b: `raise_nvm_error()` loops forever after the handler | — |
| LF-8 | `lftl_transaction_start()` computed the next slot from `ctx->data` without locating the current slot first. Also present at d8f182f. | `lftl_transaction_start()` is the first call on an area after a reset | `next_slot()` worked from `LFTL_INVALID_POINTER` and erased a slot outside the area. On a 32-bit MCU the address wrapped into `[0, slot_size)`: the STM32 reference callbacks rejected it and left the tracker set, and the CH32V307 one folded it into flash and erased the start of the firmware. | **Fixed** after 501de63 (ISSUES.md LFTL-003): the current slot is located first, and the tracker is set only once the erase has succeeded | — |
| LF-9 | EWLF writes the next record without checking that its cells are erased (ISSUES.md LFTL-002) | Power cut during a record write, or during the wrap from the lowest unit to the highest, before the lowest is erased | NOR flash: the new record holds old AND new data and passes its checksum, which is computed from flash after the write; after an interrupted wrap, `NO_VALID_VERSION` at the next reset. Flash that cannot reprogram a cell: every later write fails, and the area stays read-only. | Open (T7 to T10) | Check that the target reads erased and move past programmed cells. At initialization, keep the unit with the highest valid version and erase any other programmed unit, as `EWLF.md` describes. Compute the checksum from the intended data. |
| LF-10 | `find_current_record()` looks for a header at every 8-byte step, including inside a torn record's data | The attacker controls at least a header's worth of stored data (40 bytes with 8-byte write units, 80 with 16) and cuts power between the data write and the magic | Reads return a forged record, attacker-chosen and different from both the old and the new data. Its data can extend into the record or unit above, but not past the area. | Open (T11) | Accept headers only on the record grid (top of a unit minus a multiple of the record size). Data then never covers a candidate header. |
| LF-11 | An EWLF record larger than an erase unit is accepted. `LFTL_EWLF_RECORD_SIZE` leaves out the magic, the `prev_erased` flag and the padding, so `LFTL_AREA_EWLF` accepts content that does not fit. | Configuration, for example 192 bytes of data with 16-byte write units on 256-byte pages | `ewlf_unit_top_record()` returns an address below its unit; on the wrap to the lowest unit, the record starts below the area | Open (T12): 16 bytes written below the area | Reject a record larger than `erase_size` at `lftl_register_area_ewlf()`, and count every header field in `LFTL_EWLF_RECORD_SIZE` |

LF-5 and LF-11 are triggered by configuration, not by an attacker. They are listed because they silently corrupt memory on common targets.

## Case 2: external NVM

For regular areas, the library's own handling of hostile NVM content meets the objective: no NVM-derived value becomes an address, index or size. EWLF areas do not meet it: a magic planted near the end of the area makes the scan read past it, partly with a `memcpy` that bypasses the callbacks (LF-12). One Case 1 issue also becomes attacker-reachable through the NVM alone: LF-4 (a stale tracker after a read error). LF-7, the other one in the first version of this model, is fixed. The integrator's driver and application parsing carry most of the remaining risk.

The attacker reads and writes the external NVM at any time, including between two reads made by the same function, and controls every bus response. Integrity, freshness, confidentiality and availability of LFTL data are explicitly **not** goals. The only objective is:

> **O5.** No sequence of NVM contents or bus responses gives the attacker read or write access to the internal address space, or code execution.

### What the library reads from NVM, and what it does with it

| Value read from NVM | Used for | Can it become an address, index or size? | What the attacker can do with it |
| --- | --- | --- | --- |
| `version` (per slot or record) | Regular areas: choosing the current slot, and `+1` for the next version. EWLF areas: `+1` only | No. The slot index comes from the scan loop, so `ctx->data` always points at a slot of the configured area | Pick any slot; force `VERSION_COLLISION`; force `NO_VALID_VERSION`. Setting `0xFFFFFFFE` makes the next commit write `0xFFFFFFFF`, which the next scan treats as erased |
| `checksum` | Slot or record validity check | No | Forge freely; CRC32C is not keyed |
| `checksum2` | Regular areas: detecting a torn metadata write. EWLF areas: skipping a torn record | No | Force a slot erase and rewrite on every scan (external flash wear) |
| EWLF magic | Finding records during the scan | **Yes.** The 8-byte boundary where it matches becomes the record's start, with no check that the record fits in the area | Make the scan read up to one record past the area (LF-12); place the current record at any 8-byte boundary |
| EWLF `prev_erased` flag | Deciding whether to re-erase the previous unit | No. The unit comes from the record's position, modulo the number of units | Force one unit erase per boot (external flash wear) |
| Data bytes | Copied slot to slot at commit, copied to caller buffers | No. Copy sizes come from the configuration or the caller | Any content, changed between reads |
| Callback status codes | Raising `LOW_LEVEL_*` errors | No | Raise an error inside any operation, at a chosen point |

The library also re-reads metadata several times during a scan (checksum check, then version, then `checksum2`). A time-of-check/time-of-use (TOCTOU) change between those reads only changes which data is returned. That is a data-integrity effect, which is accepted.

### Threats

| ID | Threat | Status | Required mitigation |
| --- | --- | --- | --- |
| T2.1 | Hostile metadata forces an error path | Availability only | Since b3ed55b the library loops forever if the handler returns (LF-7, fixed), so `NO_VALID_VERSION` can no longer produce a wild pointer. The handler must still reset or `longjmp`; one that returns hangs the device. |
| T2.2 | Bus error inside a transaction | **Reachable** | A failed read during `lftl_transaction_commit()` unwinds with the tracker still set; the next write uses the dead buffer (LF-4). Fix LF-4, or abort every area in the recovery path. |
| T2.3 | Application parses NVM data as trusted | Integrator | Treat every byte read from an external area as an untrusted packet: bound every length and index against compile-time constants, store no pointers or function pointers, and validate enums and state values. |
| T2.4 | Double fetch by the application | Integrator | Read once into RAM with `lftl_read()`, validate the RAM copy and use only that copy. Never read a length field and then the payload in two calls, and never dereference the `nvm` struct directly. |
| T2.5 | Driver trusts the device | Integrator | Translate each virtual address to a device offset with a bounds check against the window. Never take sizes from the device (SFDP geometry, length fields). Bound every busy-wait loop with a timeout. |
| T2.6 | Write callback dereferences a virtual address | Integrator | lean-ftl copies slot to slot, so the write callback's `src` may lie inside the NVM window. For SPI flash that is not memory-mapped, the callback must read it through the device into a bounce buffer, never as a RAM pointer. |
| T2.7 | Virtual window overlaps real memory | Integrator | `nvm_props.base` and `size` decide whether an address goes to the callback or to `memcpy`. Place the window in unmapped address space so a stray dereference faults, and keep it apart from RAM, peripherals and internal flash. LF-12 reads up to one EWLF record past an area with `memcpy`, so leave at least that much unmapped space after the window. |
| T2.8 | Memory-mapped external flash (XIP, OctoSPI) | Integrator | Content can change between two CPU loads. Mark the region non-executable in the MPU, and make the bus-fault handler reset rather than resume. |
| T2.9 | Forced reformat or wear | Accepted | If the "formatted" flag lives in external NVM, or the application reformats on `NO_VALID_VERSION`, the attacker can wipe the data. `lftl_format()` erases a range taken from the configuration, so this path stays memory-safe. |
| T2.10 | Hostile EWLF content | **Reachable** | LF-12. Bounds checks in the read callback do not stop it, because the data part is read with `memcpy` when it lies outside every registered NVM. Until it is fixed, do not use EWLF areas on external NVM. |

### Findings in the current code

| ID | Issue | Trigger | Impact | Status (test) | Suggested fix |
| --- | --- | --- | --- | --- | --- |
| LF-12 | `find_current_record()` accepts a magic at any 8-byte boundary up to the last 8 bytes of the area, then reads the metadata and `data_size` bytes from there without checking that the record fits | A magic in the last record's worth of an EWLF area | Reads up to one record past the area: the metadata through the read callback, the data through `mem_read()`, which uses `memcpy` outside every registered NVM. If the CRC also matches, `ctx->data` points at a record that runs past the area, and `lftl_read()` returns those bytes. | Open (T13): global-buffer-overflow in `memcpy` from `checksum()` | Accept a record only if it fits in its erase unit, which the record grid of LF-10's fix guarantees. Read area data through `nvm_read()` rather than `mem_read()`. |

LF-1, LF-2, LF-3, LF-5 and LF-11 are driven by API arguments or configuration, not by NVM content. They therefore apply to Case 2 exactly as in Case 1. LF-9 and LF-10 give a Case 2 attacker nothing that writing the NVM directly does not.

## Integrator responsibilities and hardening

Until the open findings are fixed upstream, the integrator closes them with bounds-checked callbacks, a safe recovery path, strict argument validation and limits on EWLF areas. Each item below is needed in both cases unless marked.

**Contract checklist**

- [ ] `error_handler` never returns: it resets, or `longjmp`s to a recovery point. One that returns now hangs the device (LF-7).
- [ ] The recovery point calls `lftl_transaction_abort()` on every registered area before any further lean-ftl call (LF-4).
- [ ] `transaction_tracker` lives in static storage or in a frame the handler cannot unwind, and is at least `LFTL_TRANSACTION_TRACKER_SIZE(ctx)` bytes.
- [ ] Before calling lean-ftl, the application checks every offset and size it derived from a message against the field it targets, overflow-safe: `off <= field_size && len <= field_size - off` (LF-1, LF-2, LF-3).
- [ ] Batch messages are de-duplicated before a transaction, so `TRANSACTION_OVERWRITE` cannot be triggered from outside.
- [ ] No attacker-influenced pointer reaches `lftl_memread()` or the `src` of any write (T1.7).
- [ ] `write_size` is 16 bytes or less until LF-5 is fixed, and the check runs at boot, not only in `lftl_format()`.
- [ ] Until LF-9 to LF-12 are fixed, EWLF areas hold only data the application generates itself and can afford to lose, on internal NVM. Each record fits in one erase unit by the runtime layout, `max(8, WU) + max(1, WU) + 3 × max(4, WU)` plus the data rounded up to `WU`, then to 8 bytes, not by `LFTL_EWLF_RECORD_SIZE` (LF-11).
- [ ] Message-driven writes are rate-limited and batched against the endurance budget: native endurance × wear-leveling factor per page (T1.5).
- [ ] A "formatted" flag in a separate area lets the application recover from a torn `lftl_format()` (T1.6).
- [ ] Case 2 only: the driver, the address window and the application parsing follow T2.3 to T2.8 and T2.10.

**Defence in depth: bound the callbacks.** The cheapest high-value hardening is to make `erase`, `write` and `read` reject any range outside the LFTL areas (plus, for `write`'s `src`, the RAM and NVM ranges the application uses). This turns LF-2's "overwrite firmware" into an error, whatever bug reaches it. It does not cover LF-12, whose reads past an EWLF area go around the callbacks. On parts with an MPU or TrustZone, also make the LFTL areas non-executable, and make firmware flash write-protected while lean-ftl runs.

**If Case 2 needs integrity or freshness after all.** lean-ftl cannot provide either, but it composes with a small wrapper:

1. Encrypt and authenticate each record with an AEAD (for example AES-GCM or ChaCha20-Poly1305), with a key kept on chip.
2. Bind the associated data to the area identifier and a record version, so records cannot be swapped between areas or fields.
3. Keep the version counter in an **internal** lean-ftl area, which is Case 1 storage, to detect rollback of the external copy.
4. Verify the tag over the RAM copy (T2.4) before parsing anything.

This is the same pattern TF-M Protected Storage uses over its internal trusted storage.

## Verification plan and open questions

Three fuzzing harnesses on the host build, under AddressSanitizer and UndefinedBehaviorSanitizer, would cover both cases and catch regressions of the memory-safety findings. The existing tearing emulator needs new flash semantics to catch LF-9.

| Harness | Covers | Inputs | Pass criterion |
| --- | --- | --- | --- |
| API sequence fuzzer (libFuzzer) | Case 1, O4 | Random sequences of start, write, write\_any, read, read\_newer, commit, abort, erase\_all, with random offsets and sizes, on regular and EWLF areas, including sequences that begin right after a reset (LF-8); the handler `longjmp`s and recovery continues | No sanitizer report; every out-of-range call ends in the handler |
| NVM image fuzzer | Case 2, O5 | Mutated images of regular and EWLF areas, then scan, read, write and commit | No sanitizer report; callbacks never see an address outside the area, and nothing reads outside it without them |
| Fault-injecting callbacks | Case 2, T2.2 | The n-th callback returns an error or different data on each read | Same as above, with errors raised inside transactions |
| Existing tearing emulator | Case 1, O1 and O2 | Power cut at every callback boundary, followed by writes to every area after recovery. Its write callback refuses to program a write unit that is not erased, which catches LFTL-001; today the tests only read and reformat after a tear, which hides LF-9. A write callback that ANDs bits would also show silent corruption. | After restart, data equals the last or the previous committed version, never older |

Also worth doing: a bounded model check (CBMC or Frama-C) of `translate_addr()`, `is_in_range()`, the tracker indexing and the EWLF record placement in `find_current_record()` and `write_ewlf_core()`, which carry most of the memory-safety burden. In CI, build with `-Wconversion`, since `offset_wu` is a `uint32_t` derived from a `uintptr_t`.

The reproductions use `lftl_asan_harness.c` (about 290 lines): an 8-page RAM-backed NVM with 256-byte pages, one area per test, `memcpy` callbacks that report any range outside the area, a `longjmp` handler, switchable NOR and STM32U5 write semantics, and power cuts at a chosen write or erase. Build and run instructions are in its header. It can seed the API fuzzer.

**Open questions for the maintainer**

- Is `lftl_transaction_read()` meant to rely on caller validation, or is the missing range check an oversight (LF-3)?
- Should the library clear `transaction_tracker` itself before raising a transaction error (LF-4)?
- Is the 16-byte limit on metadata write units intended, and should `LFTL_WU_MAX_SIZE` be 16 (LF-5)?
- Does `find_current_record()` need the 8-byte scan step, for example to find records written with a different data size? If not, scanning only the record grid fixes LF-10 and LF-12.
- `EWLF.md` says initialization erases a second programmed unit after a power cut. Is that still planned (LF-9)?
- Would upstream accept bounds checks against the LFTL areas in the reference callbacks, as defence in depth for future bugs? The CH32V307 one folds any address below `FLASH_BASE` into flash, to support areas addressed through the `0x00000000` alias, which turned LF-8 into an erase of the firmware's first pages; a bound there would have to come from the LFTL areas.

## Sources

- [lean-ftl source](https://github.com/sebastien-riou/lean-ftl), core files `liblean-ftl/source/ftl.c` and `liblean-ftl/source/ftl_internal.h` at commit 501de63 (2026-08-31, branch `dev/ewlf`), Apache-2.0
- `EWLF.md` and `ISSUES.md` in the same repository, at the same commit and in the working tree respectively
- [lean-ftl API reference](https://lean-ftl.readthedocs.io/en/latest/api.html)
- [lean-ftl user guide](https://lean-ftl.readthedocs.io/en/latest/using.html)
- [lean-ftl internals](https://lean-ftl.readthedocs.io/en/latest/internal.html)
- [NLnet project page, LeanFTL](https://nlnet.nl/project/LeanFTL/)
