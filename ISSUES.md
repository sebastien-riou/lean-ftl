# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

Add the next issue to this table, under the next free `LFTL-nnn` (LFTL-008).

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-007](#lftl-007) | A power cut during the erase of the next slot can leave its version word equal to the current version; the scan then raises `VERSION_COLLISION` without checking that slot's integrity, on every access until the area is formatted | High | demonstrated |

---

<a id="lftl-007"></a>
## LFTL-007 - A torn erase can lose a regular area through `VERSION_COLLISION`

**Severity:** High (the area and its data are lost until `lftl_format()`, from a power cut during
normal operation) · **[demonstrated]** on the Linux simulator · found while challenging
THREAT-MODEL.md (2026-10-22)

**Where.** `find_current_slot()` in `liblean-ftl/source/ftl_internal.h`, at HEAD and since the
collision check was added.

The scan raises `LFTL_ERROR_VERSION_COLLISION` as soon as a slot's version word equals the best
version seen so far, before checking that slot's integrity. A slot with garbage content whose
version word happens to equal the current version is therefore not skipped: it stops the scan, and
since `ctx->data` is never set, every later access runs the same scan and fails again. The only way
out is `lftl_format()`, which loses the data.

Every update first erases the next slot, which still holds the previous version. A power cut during
that erase leaves cells partly erased, so bits flip from 0 to 1 at random. With 2 slots, slot 0 holds
the odd versions and slot 1 the even ones: whenever the current version is odd, the next slot holds
`current - 1`, and that word reads `current` once its bit 0 alone is erased. Slot 1 is scanned after
slot 0, so the valid slot sets `max_version` first and the garbage slot collides with it. A torn
program cannot do this (programming only clears bits), a torn erase can.

The check is also order-dependent: the same garbage slot is harmless when it is scanned before the
valid one, since the integrity check then rejects it.

THREAT-MODEL.md claims the opposite: "A torn slot fails CRC32C and is skipped" (O1), no residual
risk for the first access after a reset (T1.9), and only wear-out and a torn `lftl_format()` can lose
the device (O3). The tearing simulation cannot see it: it tears at callback boundaries and never
leaves a partly erased page.

**Evidence.** A program on the Linux simulator, planting the state a torn erase leaves (one bit set
in the next slot's version word, nothing else):

````
2 slots: after format and 2 writes, current slot 0 holds version 3, slot 1 holds version 2
  torn erase of slot 1: its version word now reads 3, its checksum still covers the old version
  after reset: error 0x01 (VERSION_COLLISION), on every access from now on
3 slots: after format and 5 writes, current slot 2 holds version 6, slot 0 holds version 4
  torn erase of slot 0: its version word now reads 6, its checksum still covers the old version
  after reset: read OK, current slot 2, data 4 (expected 4)
````

**Fix.** Compare versions only between slots that pass their integrity check: move
`slot_integrity_check_ok()` ahead of the collision test in `find_current_slot()`. Two valid slots with
the same version still raise `VERSION_COLLISION` (test 21). A regression test can build the state
above with an erase and programming of erased cells only, as real flash can hold.

---
