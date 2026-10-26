# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

Add the next issue to this table, under the next free `LFTL-nnn` (LFTL-014).

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-010](#lftl-010) | The repairs the scan runs at boot erase without any application write; cutting power during one at every boot makes it erase at every boot, the EWLF `prev_erased` repair always the same page, which wears out in about its endurance in boots | Medium | confirmed |
| [LFTL-011](#lftl-011) | The scan reads a weak `checksum2` word twice: a slot or record kept because it read partially programmed at one boot, and exposed, can read all erased at a later boot and is then erased or skipped, a rollback by one version | Medium | confirmed |
| [LFTL-013](#lftl-013) | An EWLF write programs over cells that merely read erased: the weak first word of a torn record, once it reads all erased, is reused without an erase, within a unit and, at a rollover, for the whole unit; under T4 the record written there can lose bits later, and if it is the rollover record no valid record remains once the previous unit is erased | High | inferred |
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

**Fix.** Proposed in `READ-ONLY-SCAN.md`: a scan that reads and chooses but never modifies the NVM,
every erase moved into the application write that reuses the cells. Short of that, for the
`prev_erased` repair: when the previous unit reads fully erased,
write the flag without erasing it again, which removes the per-boot erase but gives up the
protection against a weakly erased unit (reading erased now, flipping later under a record written
a full cycle on; T4 of the [flash page](https://lean-ftl.readthedocs.io/en/latest/flash.html#flash-tearing)).
The other repairs now act only when `checksum2` reads all erased, and never erase
more than the torn slot; a bound would need state the library does not keep. THREAT-MODEL.md should name the repeated-cut attacker and a wear
threat for the boot repairs next to T1.5.

---

<a id="lftl-011"></a>
## LFTL-011 - A weak `checksum2` is read twice: kept at one boot, erased or skipped at a later one

**Severity:** Medium (a rollback by one version of data the application may have acted on; never a
loss, the previous version is complete) · **[confirmed]** in the source and from how weakly
programmed cells behave; not run · found while reviewing READ-ONLY-SCAN.md (2026-10-25)

**Where.** `find_current_slot()` and `find_current_record()` in `liblean-ftl/source/ftl_internal.h`,
since 426fcb1 (LFTL-009) and 14b3532 (LFTL-008).

A write programs `checksum2` last. A power cut during it leaves the word weak: its cells got about
the same partial charge, so the word sits near the read threshold as a whole, reads complete,
partially programmed or all erased depending on the moment, and drifts toward all erased as the
charge leaks (T2 of the [flash page](https://lean-ftl.readthedocs.io/en/latest/flash.html#flash-tearing)).
The current rules decide from how it reads at each boot:

- `checksum2` partially programmed: the slot or record is kept (its checksum is complete) and the
  application sees its data;
- `checksum2` all erased: the slot is erased (regular areas) or the record copied forward then
  ignored (EWLF areas), and the previous version is current.

A word that reads partially programmed at one boot and all erased at a later one goes from the
first rule to the second: the slot the application saw is erased, or the record it saw is skipped,
and the previous version comes back. Nothing is lost, B is complete, but O2's "no rollback below the
last committed version" does not hold for that version. The assumption behind the rules, that a word
with some programmed bits keeps at least one, was the wrong way round: leakage makes "partial, then
all erased" the expected evolution of a marginal word.

**Fix.** Read the word once and pin the decision by programming it to zeros, which needs the
all-zero overwrite of N3 on the [flash page](https://lean-ftl.readthedocs.io/en/latest/flash.html#flash-not-relied-on):
documented on the STM32U5 and STM32L5, to be established on the M5531, outside the W25Q32RV's
specification (FLASH.md); the Linux simulator accepts it:
`checksum2` partially programmed, pin the whole item to zeros before returning, so it never reads
all erased later; `checksum2` all erased, tombstone the torn slot or record by pinning its version
word instead of erasing it, so it is invalid for good and cannot come back. Both are word programs,
no erase: fixing this also removes the regular-area erase of LFTL-010. On today's regular layout
this needs no format change; READ-ONLY-SCAN.md has the design, with the one timing residual that
remains (a marker reading complete at every boot until it reads all erased). A regression test can
build the partially programmed state as test 24 does, reset, then make the item read erased (on
Linux, where the NVM is RAM) and reset again: the same data must be read.

---

<a id="lftl-013"></a>
## LFTL-013 - An EWLF write programs over a torn record that drifted to all erased, without an erase

**Severity:** High (the area's only valid record can go invalid, `LFTL_ERROR_NO_VALID_VERSION`, a
loss; the within-unit case is a rollback by one version) · **[inferred]** from the source and the
tearing model (T2 and T4 of the documentation's flash page), not observed · found while clarifying
T4 (2026-10-25)

**Where.** `write_ewlf_core()` in `liblean-ftl/source/ftl_internal.h`: the free-position search,
`found = is_erased(dst_ctx, candidate_addr, record_size)` at line 1517, and the rollover,
`if(!is_erased(dst_ctx, new_unit_base, page_size(dst_ctx))) nvm_erase(...)` at line 1533, at HEAD.

An EWLF write programs its record at the first position below the current record whose cells all
read erased and, when the unit has no room, rolls over to the next unit, erasing it only if it does
not read all erased. "Reads all erased" is taken for "is erased". A record write cut during its
first write unit leaves that unit weak (T2) and everything after it untouched; the position, or the
whole unit at a rollover, reads partially programmed for a while and all erased once the charge
has leaked, or at once if the cut came early in the pulse. If it reads all erased at the next
write, the next record is programmed over it with no erase in between: the cells the torn write
charged and the new data leaves at 1 keep their residual charge, and under T4 (program disturb
from the words programmed around them, read disturb) they can cross to 0 later, inside a record
whose checksum was complete when written.

Two outcomes:

- within a unit: the record written over the reused position goes invalid later; the record above
  it, current when the torn write started, is in the same unit and still there, so the area rolls
  back by one version (O2);
- at a rollover: the write enters a unit whose top position holds the drifted torn record (the
  previous attempt at the same rollover, cut in its first unit), skips the erase, programs the
  rollover record there, then erases the vacated unit and sets `prev_erased`. If that record goes
  invalid before another is written, no valid record remains and `find_current_record()` raises
  `LFTL_ERROR_NO_VALID_VERSION` (`ftl_internal.h:1172`): a loss (O1).

Both need the torn unit to read all erased at the very next write: positions above the current
record are never revisited, and the rollover's check runs once per attempt. Regular areas are not
exposed: every write erases its slot first (`write_core()`, `erase()`), whatever it reads.

**Evidence.** Read in the source, the sequence above. Not run: the Linux simulator models neither
the drift nor T4; a test can only build the states by hand (a partial first unit, then all erased
in RAM, then a flipped bit in the record written over it) and check the outcome.

**Fix.** Unit case: at a rollover, erase the entered unit unconditionally, even when it reads
erased: the lazy erase of `READ-ONLY-SCAN.md` (§4b), which drops the erase of the vacated unit and
the `prev_erased` flag so that each unit is still erased once per cycle; keeping today's flag and
adding the unconditional erase costs a second erase per unit per cycle. Within-unit case: a
position that held a torn record cannot be told from a free one once it reads erased; the cure is
to make it recognisable while it still reads partial, by pinning its first word to zeros when the
scan or the write sees it (the all-zero overwrite of N3; the start marker of `READ-ONLY-SCAN.md`
§4 does exactly that), or to accept the residual, a rollback, until that design is taken up. A
regression test for the unit case: a torn rollover record whose first unit is then made to read
all erased, then a write; the erase counter of the entered unit must increment.

---
