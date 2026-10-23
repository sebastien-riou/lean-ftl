# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

Add the next issue to this table, under the next free `LFTL-nnn` (LFTL-010).

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-009](#lftl-009) | In a 2-slot area, the repair of a slot with a missing `checksum2` erases the other slot, the only stable copy, on the strength of a checksum word that may be weakly programmed; a power cut during that repair, then the word reading erased, leaves no valid slot | High | demonstrated |

---

<a id="lftl-009"></a>
## LFTL-009 - In a 2-slot area, the `checksum2` repair erases the only stable copy

**Severity:** High under the threat model's attacker, who cuts power at chosen instants and can repeat
it at every boot (the area and its data are lost until `lftl_format()`); rare for natural tears, which
must land in two specific windows · **[demonstrated]** on the Linux simulator · found while
assessing repeated power cuts at boot (2026-10-23)

**Where.** `move_to_next_slot()`, called by `find_current_slot()` in
`liblean-ftl/source/ftl_internal.h`, at HEAD and since the `checksum2` repair was added.

A write programs its data, then version and checksum, then `checksum2`. A power cut during the
checksum word leaves it weakly programmed and `checksum2` erased. At the next boot the scan takes
the slot if its checksum reads valid, and repairs it: `move_to_next_slot()` erases the next slot,
copies the data, writes fresh meta-data. With 2 slots the next slot holds the previous version, the
only slot whose meta-data is complete. From its erase until the copy's version and checksum are
written, the torn slot's weak checksum word is the only thing that validates any data in the area.
A power cut in that window, followed by the weak word reading erased at a later boot, leaves no valid
slot: `NO_VALID_VERSION` on every boot. With 3 or more slots the next slot is a stale one and the
previous version survives whatever the weak word reads.

The window is not specific to the repair: with 2 slots, whatever erases the other slot while the
current one has a weak checksum opens it, the next application write included. The repair makes it
happen at boot, where the attacker can cut on cue and repeat at every boot until the weak cell
misreads; temperature and supply voltage are theirs too. This is the regular-area twin of LFTL-008.

THREAT-MODEL.md says O1 and O3 hold for regular areas, and the user guide gives 2 as the minimum
wear-leveling factor.

**Evidence.** A program on the Linux simulator: a write, its `checksum2` word made erased (the cut
during the checksum word), an erase callback that erases then reports the power cut, then the
checksum word made erased (the weak word misreading):

````
2 slots: current slot 1 holds version 2 (data 1), the write before it is in slot 0
  boot 1, repair starts: erase next slot, cut: error 0x17f (the simulated power cut, right after the repair's erase)
  slot erased by the repair: version word of each slot now: 4294967295 2
  boot 2, the torn slot's checksum reads erased: error 0x2 (NO_VALID_VERSION): area lost
3 slots: current slot 2 holds version 3 (data 2), the write before it is in slot 1
  boot 1, repair starts: erase next slot, cut: error 0x17f (the simulated power cut, right after the repair's erase)
  slot erased by the repair: version word of each slot now: 4294967295 2 3
  boot 2, the torn slot's checksum reads erased: read OK, data 1 (expected 1), current slot 1
````

**Fix.** To be decided. Options: require 3 slots (the repair then erases a stale slot), at the cost
of 50 % more flash for the smallest areas; keep 2 slots and state in THREAT-MODEL.md and the user
guide that anti-tearing against an adversary with power control needs 3; or find a 2-slot repair
that does not erase the previous version before the copy is validated, which the slot layout does
not currently allow.

---
