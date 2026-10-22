# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

Add the next issue to this table, under the next free `LFTL-nnn` (LFTL-009).

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-008](#lftl-008) | An EWLF record whose `checksum2` program was torn can read valid at one boot and not at the next; the scan rejects it on `checksum2` and erases the previous unit as soon as a rollover record reads valid, so the second boot can be left with no valid record, or with the previous version after the torn one was exposed | High | demonstrated |

---

<a id="lftl-008"></a>
## LFTL-008 - A torn EWLF `checksum2` can lose the area, or roll it back across boots

**Severity:** High (the area and its data are lost until `lftl_format()`, from a power cut during
an ordinary write; the smaller variant is a rollback across boots) · **[demonstrated]** on the Linux
simulator · found while challenging THREAT-MODEL.md (2026-10-22)

**Where.** `ewlf_record_is_valid()` and `find_current_record()` in `liblean-ftl/source/ftl_internal.h`,
at HEAD and since EWLF areas were added (501de63).

Regular areas take a slot whose data and checksum are valid whatever its `checksum2`, and copy its
data forward with fresh meta-data when `checksum2` mismatches, because a word whose program was torn
"may be weakly programmed" (`find_current_slot()`). EWLF areas do the opposite: `ewlf_record_is_valid()`
rejects a record whose `checksum2` mismatches, and `find_current_record()` erases the previous erase
unit as soon as a rollover record (the top record of its unit, `prev_erased` flag unconfirmed) reads
valid.

`checksum2` is the last word a record write programs, so a power cut during a write leaves exactly
that word weak: it can read valid at one boot and erased at the next. Then:

- within a unit, boot 1 takes the torn record and the application sees its data; boot 2 rejects it
  and returns the previous version: a rollback after exposure, which O2 exists to prevent;
- on a rollover, boot 1 takes the record and erases the previous unit, which held every older record;
  boot 2 rejects it, and the area has no valid record left: `NO_VALID_VERSION` on every boot until
  the area is formatted.

THREAT-MODEL.md claims the opposite: "A torn record is skipped by the scan and by the next write"
(O1), O2 holds for EWLF areas, and T1.10 has no residual risk. The tearing simulation cannot see it:
a simulated word never reads two ways.

**Evidence.** A program on the Linux simulator, 2 units of 102 records. The rollover record is
written by hand up to `checksum2` included, as the write does, and the "power cut" stops it before
the erase of unit 1; the flip is the word poked to erased between boots:

````
boot 1 (checksum2 reads valid): read OK, data 101 (expected 101)
    : current record at unit 0, position 0 from its top, version 103, prev_erased flag set
  unit 1 erased by the scan: yes
boot 2 (checksum2 reads erased): error 0x02 (NO_VALID_VERSION), on every boot from now on

within a unit, boot 1 (checksum2 of version 2 reads valid): read OK, data 1 (expected 1)
within a unit, boot 2 (checksum2 reads erased): read OK, data 255 (expected 1)
````

**Fix.** Give EWLF areas the regular-area semantics: a record with valid magic and checksum is taken
whatever its `checksum2`, and a mismatching `checksum2` makes the scan copy it forward (a fresh record
below it, with full meta-data) before anything irreversible. The rollover repair then keys on the top
record of the unit holding the current record, and the scan's check of the next unit in write order
must take that unit's newest valid record, not only its top one, since a copy forward can leave two
records there. An EWLF area needs at least 2 records per erase unit for the copy to stay in the
record's unit.

---
