# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-001](#lftl-001) | A torn checksum2 is repaired by reprogramming flash that STM32U5-class flash refuses to reprogram, leaving the area unreadable | Medium | inferred |
| [LFTL-002](#lftl-002) | After a power cut, the next EWLF write programs over cells that are not erased: NOR flash keeps a corrupted record that passes its checksum, STM32U5-class flash refuses every later write | High | inferred |
| [LFTL-003](#lftl-003) | A transaction started before the area's first read erases outside the area: the STM32 callbacks reject it and the transaction cannot start, the CH32V307 callback erases the start of the firmware | High | demonstrated |

---

<a id="lftl-001"></a>
## LFTL-001 - A torn checksum2 is repaired by reprogramming flash the STM32U5 refuses to reprogram

**Severity:** Medium · **[inferred]**, needs a bench test

**Where.** `find_current_slot()` and `write_meta_core()`, in `liblean-ftl/source/ftl_internal.h`
at HEAD (501de63), in `liblean-ftl/source/ftl.c` in v0.1.0 and earlier (the same code back to at
least v0.0.19). Regular areas only: EWLF records skip a record whose checksum2 alone fails instead
of repairing it in place (`EWLF.md`), which leads to [LFTL-002](#lftl-002) instead.

Every slot ends in three metadata items, each its own write unit when the write unit is 16 bytes
(the STM32U5 case): version, checksum, checksum2. `write_meta_core()` programs the first two, then
checksum2, so a power loss between them leaves checksum2 erased or half-programmed while the slot's
CRC still checks. On the next access, `find_current_slot()` picks that slot and repairs it - the
`meta.checksum2 != meta.checksum` branch - by calling `write_meta_core()` again, which first
re-programs version and checksum: write units that are already programmed.

RM0456 (STM32U5), FLASH_NSSR.PROGERR: *"set by hardware when a nonsecure quad-word address to be
programmed contains a value different from all 1 before programming, except if the data to write is
all 0."* So the repair's very first write fails. A platform `write` callback that checks the flash
status reports it, `raise_error()` reaches the error handler, and it does so on the first access to
that area at every boot. The area's data stays on the part but is unreachable.

**Evidence.** Emulation only

**Fix.** Program only what is still erased: if checksum2 reads as erased, program it alone. If it
is half-programmed, the slot cannot be repaired in place: rewrite the data into the next slot with
version+1, which is the normal update path and lands a clean metadata block.

<a id="lftl-002"></a>
## LFTL-002 - After a power cut, the next EWLF write programs over cells that are not erased

**Severity:** High · **[inferred]**, needs a bench test · reported from `THREAT-MODEL.md` as LF-9
(2026-10-21)

**Where.** `write_ewlf_core()` and `find_current_record()`, in `liblean-ftl/source/ftl_internal.h`
at HEAD (501de63). EWLF areas only: regular areas erase the next slot before every write.

`write_ewlf_core()` puts the new record directly below the current one, or at the top of the next
unit down when the current unit is full, and programs it without checking that those cells are
erased. Two kinds of power cut leave them programmed:

- **Torn record.** A record is written data first, then magic, metadata and checksum2. A power loss
  before checksum2 leaves a partial record below the current one. `find_current_record()` skips it
  and returns the current record, so the next write targets the partial record's cells again.
- **Interrupted wrap.** When the lowest unit is full, the next record goes to the top of the highest
  unit, then the lowest unit is erased. A power loss before that erase completes leaves both units
  programmed. The scan runs upward from the area base, so it returns the old record in the lowest
  unit and never reaches the new one; the next write wraps to the same top position, on top of it.
  `EWLF.md` says initialization detects two programmed units and erases the stale one. The code only
  repairs when the record it finds is the top record of its unit, which is not the case here.

What happens next depends on the flash:

- **NOR flash, where programming can only clear bits:** the new data is ANDed with the old. The
  checksum is computed from flash after the data write (`meta.checksum = checksum(dst_ctx,
  new_data_addr, ...)`), so it matches the corrupted data and the record is accepted. After an
  interrupted wrap the metadata is ANDed too: at the next reset no record checks, and every access
  raises `NO_VALID_VERSION` until the area is reformatted.
- **STM32U5-class flash** (RM0456 PROGERR, quoted under LFTL-001): the data write fails,
  `LOW_LEVEL_WRITE` reaches the error handler, and so does every later write. The area stays
  readable but can never be updated again.

**Evidence.** Emulation only: `lftl_asan_harness.c` tests 7 to 10 (2026-10-21) cut power at the two
points above, with a write callback that ANDs (tests 7 and 9) or refuses to program a non-erased
byte (tests 8 and 10). Test 7, 64-byte records of `11`, then `0f` torn before its magic, then `3c`:

```
  after reset : 11 11 11 11 11 11 11 11
  after d3    : 0c 0c 0c 0c 0c 0c 0c 0c
```

Test 9 reads back `5a & a5 = 00` after the write that follows the interrupted wrap, then
`error_handler(0x2)` after a reset; tests 8 and 10 get `error_handler(0x201)` on every write after
the reset. The repository's own tearing tests cannot see any of this: the Linux simulator
(`target/linux/linux-accessors.c`) programs with `memcpy` and models a tear by XORing bytes, so
writing over a torn record looks like a clean write. Not observed on a board.

**Fix.** Never program a record over cells that are not erased: before writing, check that the
target range reads erased, and if it does not, move below it or roll over. At initialization, when
more than one unit is programmed, keep the unit with the highest valid version and erase the others,
as `EWLF.md` describes. Compute the checksum from the data meant to be written, not from flash after
the write, so a record that did not program as intended fails its check instead of passing. Give the
Linux simulator a write mode that ANDs and one that refuses non-erased cells, so tearing tests catch
this class of bug (and LFTL-001).

<a id="lftl-003"></a>
## LFTL-003 - A transaction started before the area's first read erases outside the area

**Severity:** High · **[demonstrated]** on the host; the CH32V307 outcome is **[confirmed]** from the
source and a 32-bit model of the arithmetic, not run on a board · reported from `THREAT-MODEL.md` as
LF-8 (2026-10-21)

**Where.** `lftl_transaction_start()` in `liblean-ftl/source/ftl.c` and `next_slot()` in
`liblean-ftl/source/ftl_internal.h` at HEAD (501de63); the same code at d8f182f. Regular areas only:
EWLF areas reject transactions.

Every other operation locates the current slot on first use, through `translate_addr()` and
`find_current_slot()`. `lftl_transaction_start()` does not: it sets `transaction_tracker`, then
erases the slot `next_slot()` picks. After a reset, `ctx->data` is still `LFTL_INVALID_POINTER`, so
`next_slot()` works from `0xFFFFFFFF - area` and `slot_base()` wraps around. On a 32-bit MCU the
result is always page-aligned and below `slot_size`, near address 0. On a 64-bit host it is a wild
address. What happens next depends on the erase callback:

- **STM32U5 and STM32L5** reference callbacks return 1 for an address below `FLASH_BASE_NS`, so the
  error handler gets `0x101` (`LOW_LEVEL_ERASE | 1`). The tracker was set before the erase and stays
  set: retrying `lftl_transaction_start()` raises `TRANSACTION_ONGOING` (`0x7`), and a later
  `lftl_write()` is routed into the dead transaction (`THREAT-MODEL.md` LF-4). An application that
  starts with a transaction after every boot never gets one through.
- **CH32V307** reference callback folds any address below `FLASH_BASE` into flash ("handle flash
  address aliasing", `target/ch32v307/ch32v307-accessors.c`), so it erases a slot's worth of pages
  from `0x08000000`: the start of the firmware, vector table included. The device runs on until the
  next reset, then does not boot.

The repository's tests never reach this: they always format or read an area before starting a
transaction.

**Evidence.** `lftl_asan_harness.c` test 6 (64-bit host): the erase callback is handed an address
hundreds of GB past the area (the exact offset depends on where the loader puts things), and
`memset` faults. With an erase callback that rejects out-of-range
addresses, as the STM32 ones do (2026-10-21):

```
lftl_transaction_start first after reset:
  error_handler(0x101)
  transaction_tracker still set
retry lftl_transaction_start:
  error_handler(0x7)
```

The 32-bit addresses come from `next_slot()`, `get_current_slot_index()` and `slot_base()`
transcribed with a 32-bit `uintptr_t`: for 224 layouts (areas from flash+64 KB to +288 KB on the
CH32V307, slots of 1 to 4 pages of 4 KB), the erase address was always page-aligned and below
`slot_size`. With an area at `0x08040000` and one-page slots it is `0x00000000`, which the CH32V307
callback turns into `0x08000000`. A 32-bit host build of the same scenario could not run in the
sandbox it was written in; not observed on a board.

**Workaround.** After a reset, make the first call on each area a read, for example `lftl_read()` of
one byte. If `lftl_transaction_start()` raised an error, call `lftl_transaction_abort()` before
anything else.

**Fix.** In `lftl_transaction_start()`, call `find_current_slot()` when `ctx->data` is
`LFTL_INVALID_POINTER`, before `next_slot()`, and set `transaction_tracker` only once the erase has
succeeded. Separately, the CH32V307 callback should check its range against the LFTL areas rather
than fold out-of-range addresses into flash; that would have turned this into an error.
