# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

Add the next issue to this table, under the next free `LFTL-nnn` (LFTL-011).

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-010](#lftl-010) | The repairs the scan runs at boot erase without any application write; cutting power during one at every boot makes it erase at every boot, the EWLF `prev_erased` repair always the same page, which wears out in about its endurance in boots | Medium | confirmed |

---

<a id="lftl-010"></a>
## LFTL-010 - Boot-time repairs give an attacker with a power switch an erase per boot

**Severity:** Medium (permanent wear, no data loss by itself; the worst case kills one page in about
its endurance in boots, minutes with automated power cycling) · **[confirmed]** in the source ·
found while assessing repeated power cuts at boot (2026-10-23)

**Where.** The repairs of `find_current_slot()` (`move_to_next_slot()`) and `find_current_record()`
(`copy_forward_ewlf()`, the `prev_erased` repair) in `liblean-ftl/source/ftl_internal.h`, at HEAD.

The first access to an area after a reset scans it and repairs what a torn write left. The repairs
erase or write, and a power cut during a repair leaves the state that triggers it, so the next boot
repairs again. An attacker who knows when the application writes creates the torn state with one
cut, then cuts during the repair at every boot:

- a regular slot whose `checksum2` reads all erased costs one erase of that slot per boot, until one
  erase completes (since the LFTL-009 fix; before it, a slot erase and a copy, the slots alternating);
  one whose `checksum2` is partially programmed costs nothing;
- an EWLF record whose `checksum2` reads all erased costs one record write per boot, and one erase per
  `records_per_unit` boots; one whose `checksum2` is partially programmed costs nothing;
- an EWLF rollover record with an unconfirmed `prev_erased` flag costs one erase of the previous
  unit per boot, always the same page, until one erase and the flag write complete. On flash rated
  10k cycles that page is worn out after about 10k boots; with a page erase of some tens of
  milliseconds and an automated power cycle, in minutes. Later erases of that unit fail or its
  cells no longer hold data.

The application cannot rate-limit this: the repairs run before it gets control, and T1.5 of
THREAT-MODEL.md only covers wear driven by messages. Without a torn state to start from, repeated
cuts at boot do nothing permanent: the scan only reads.

**Evidence.** Read in the source: each repair erases before it can complete, and nothing records
that it already ran. Not run on a board.

**Fix.** To be decided. For the `prev_erased` repair: when the previous unit reads fully erased,
write the flag without erasing it again, which removes the per-boot erase but gives up the
protection against a weakly erased unit (reading erased now, flipping later under a record written
a full cycle on). The other repairs now act only when `checksum2` reads all erased, and never erase
more than the torn slot; a bound would need state the library does not keep. THREAT-MODEL.md should name the repeated-cut attacker and a wear
threat for the boot repairs next to T1.5.

---

