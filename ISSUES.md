# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-002](#lftl-002) | After a power cut, the next EWLF write programs over cells that are not erased: NOR flash keeps a corrupted record that passes its checksum, STM32U5-class flash refuses every later write | High | inferred |

---

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
- **STM32U5-class flash:** RM0456, FLASH_NSSR.PROGERR is *"set by hardware when a nonsecure
  quad-word address to be programmed contains a value different from all 1 before programming,
  except if the data to write is all 0."* The data write fails, `LOW_LEVEL_WRITE` reaches the error
  handler, and so does every later write. The area stays readable but can never be updated again.

**Evidence.** Emulation only: `lftl_asan_harness.c` tests 7 to 10 (2026-10-21) cut power at the two
points above, with a write callback that ANDs (tests 7 and 9) or refuses to program a non-erased
byte (tests 8 and 10). Test 7, 64-byte records of `11`, then `0f` torn before its magic, then `3c`:

```
  after reset : 11 11 11 11 11 11 11 11
  after d3    : 0c 0c 0c 0c 0c 0c 0c 0c
```

Test 9 reads back `5a & a5 = 00` after the write that follows the interrupted wrap, then
`error_handler(0x2)` after a reset; tests 8 and 10 get `error_handler(0x201)` on every write after
the reset. The repository's own tearing tests do not see this. The Linux simulator
(`target/linux/linux-accessors.c`) refuses to program a write unit that is not erased, but after
each simulated tear the tests only read the areas back, then format them, so no write ever lands on
a torn record. Not observed on a board.

**Fix.** Never program a record over cells that are not erased: before writing, check that the
target range reads erased, and if it does not, move below it or roll over. At initialization, when
more than one unit is programmed, keep the unit with the highest valid version and erase the others,
as `EWLF.md` describes. Compute the checksum from the data meant to be written, not from flash after
the write, so a record that did not program as intended fails its check instead of passing. Make
the tearing tests write to each area after recovering from a tear, so the simulator's refusal to
program a non-erased write unit catches this class of bug; a write mode that ANDs would also show
the silent corruption.
