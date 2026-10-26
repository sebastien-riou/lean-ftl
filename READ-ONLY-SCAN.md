# A read-only scan

**Status:** proposal; nothing below is implemented. It describes how an FTL designed from scratch
would close ISSUES.md LFTL-010 (boot-time repairs give an attacker with a power switch an erase per
boot) and LFTL-011 (a weak `checksum2` read twice), and what it would take to get lean-ftl there.
It assumes the reader knows EWLF.md and THREAT-MODEL.md.

## Notation

- The **scan** is what the first access to an area after a reset does: locate the current slot or
  record. A **repair** is any erase or write the scan makes on its own.
- A **write** is an update the application asks for. For a regular area it erases the next slot,
  programs the data, then `version` and `checksum`, then `checksum2`. For an EWLF area it programs
  a record at the next free position (data, magic, `version` and `checksum`, `checksum2`), rolling
  over to the next erase unit in write order when the current unit is full.
- **B** is the slot or record that was current when a write started; **T** is the one that write
  was filling when a power cut interrupted it.
- A **weak word** is a write unit whose programming a power cut interrupted. Its cells got about the
  same partial charge, so the word sits near the read threshold as a whole; it may read complete,
  partially programmed or all erased, differently from one read to the next, and since charge only
  leaks it drifts toward reading erased over time. Only the word being programmed at the instant of
  the cut can be weak: the words before it are complete, the words after it erased. This is T2 of
  the documentation's [Flash requirements and assumptions](https://lean-ftl.readthedocs.io/en/latest/flash.html#flash-tearing)
  page; T3 there bounds it: a word written with at least 16 programmed bits that once read
  correctly never reads all erased later.
- A slot or record is **stable** when its `checksum` word is complete: its validity can then not
  change until it is erased.
- "Reads all erased" means every byte of a write unit reads `0xFF`, not only its 4 value bytes: the
  library programs the bytes next to a meta-data value to zero along with it.
- **Pinning** a word is programming its whole write unit to zeros. It needs a flash that accepts
  all zeros over a partially programmed unit: documented for the STM32U5 and STM32L5, unknown for
  the M5531, outside the specification of the W25Q32RV (FLASH.md), so it is an opt-in capability
  per NVM (N3 of the [flash page](https://lean-ftl.readthedocs.io/en/latest/flash.html#flash-not-relied-on)),
  not something lean-ftl assumes; the Linux simulator accepts it (a write over programmed cells is
  refused unless it writes zeros). It gives the cells a full pulse, so a pinned word reads zeros
  for good, and on ECC flash it replaces a codeword a torn program left inconsistent by a valid one.
- Diagrams: `U1` is the unit at the higher addresses, `U0` the lower one; `p0` is the top position
  of a unit (highest address), `p3` its bottom. Writes fill a unit from `p0` down.

## 1. Why boot repairs are a wear channel

A repair's completion is only recorded after it: the flag after the erase, the fresh meta-data
after the copy, the rescan after the erased slot. A power cut during the repair therefore leaves
exactly the state that triggered it, and the next boot repeats it, in full. The application cannot
rate-limit this: the repair runs before the application gets control, and the attacker needs no
message, only a power switch and the knowledge of when the scan erases. Today's repairs:

| Repair | Created by | Per boot, if cut during it every time |
|---|---|---|
| Regular slot whose `checksum2` reads all erased: erase it (`find_current_slot()`) | A cut during a write's checksum word | One erase of that slot, until one completes |
| EWLF record whose `checksum2` reads all erased: copy it forward (`copy_forward_ewlf()`) | Same, in an EWLF area | One record write; one erase per `records_per_unit` boots |
| EWLF rollover record whose `prev_erased` flag is unconfirmed: erase the previous unit, write the flag (`find_current_record()`) | A cut between a rollover's `checksum2` and its flag | One erase of the same page, until one erase and the flag write complete |

The third one is the bad one: on flash rated 10k cycles, 10k boots kill that page, and an automated
power cycle of some tens of milliseconds does that in minutes.

## 2. The principle

**The scan reads and chooses; it never erases. Only writes erase, and only the application calls
writes. When the scan must act on a word that may be weak, it reads it once and pins its decision.**

What a power cut can do is then bounded by what the application's own writes cost, which
THREAT-MODEL.md T1.5 already asks the application to budget, and hostile NVM content (Case 2) can
make the scan program a word of zeros, never erase.

The decision is about the last word of a write, the end marker, `checksum2` today. The word before
it, `checksum`, is complete as soon as the marker's programming started, and may be weak otherwise.
At the scan, the marker reads one of three ways:

- **Programmed in some bits, not all:** its programming had started, so the slot or record is
  stable. Take it, and pin the marker before returning to the application. It then reads zeros for
  good: the weak word cannot later read all erased and make the next boot think otherwise.
- **All erased:** either the marker was never programmed, and the checksum may be weak, or it was
  marginal and has drifted. The two cannot be told apart and need not be: do not take the slot or
  record, B is current, and **tombstone** T by pinning its version word. Its checksum can no longer
  verify, so it is invalid for good and cannot come back at a later boot as a late "new" once its
  marker jitters back. B is stable: it was current when T's write began.
- **Complete:** take it. This is every boot in normal operation.

Pinning and tombstoning are word programs, idempotent and solidifying; a cut during one leaves the
word more programmed than before, and the next boot decides again on a word that is closer to
settled. The attacker of LFTL-010 gets one word program per boot and never an erase.

One case still depends on timing: a marginal marker that reads complete at every boot that looks at
it, then all erased before the next one. That needs all its programmed bits to cross the threshold
between two consecutive boots, with no boot in between to see the partial state and pin it; the
spread between the first and the last cell to cross is what gives the pin its chance. The
consequence is a rollback by one version of data the application may have acted on, never a loss,
since B is complete. A design that wants that gone too pins the current record's marker at every
boot, one word program per boot and per area; an option, not the default here.

## 3. Regular areas

The scan already takes the highest version among valid slots, and reads the whole `checksum2` unit
since 426fcb1. Today's layout allows both pins, so this part needs no format change:

- `checksum2` partially programmed: pin the whole item to zeros. Its value becomes 0, a stable
  mismatch that the existing rule reads as "keep"; it can never read all erased again. (Today the
  slot is kept without the pin: LFTL-011.)
- `checksum2` all erased, another valid slot exists: tombstone T by pinning its version item,
  instead of `erase_slot()`. T becomes invalid for good; the next write erases it anyway, as every
  write erases its target. Nothing is erased at boot. (Today: the erase, the regular-area part of
  LFTL-010.)
- `checksum2` all erased, no other valid slot: keep T, as today. Nothing in normal operation
  produces that state, and taking it beats having nothing.

Because the marker only ever has to answer "started or not", `checksum2` need not duplicate the
checksum: one write unit holding zeros, written last, does the same job. That is a layout change for
deployed areas and buys nothing but clarity, so it is noted, not proposed.

## 4. EWLF areas

The EWLF layout can change, so the two markers take their natural form, and three changes together
remove the `prev_erased` flag, the copy forward and the next-unit rule.

### 4a. A zero end marker, no copy forward

The end marker is an all-zero write unit programmed last (the `checksum2` field, with that meaning):
a complete write leaves it solid. At the scan, the rules of section 2: partially programmed, pin and
take; all erased, tombstone the version word and take the previous record; complete, take. A
tombstoned record is skipped by the scan (its checksum cannot verify) and by the write path (its
start marker, below, is set). The copy forward of 14b3532 was chosen to survive a marker that reads
complete, then all erased; pinning answers the same case without a write of the data, and without
LFTL-008's rollover problem, because nothing is erased at boot.

### 4b. A start marker, and lazy unit erase

The `prev_erased` flag's write unit, right after the magic, becomes the **start marker**: an
all-zero word programmed first, before any data. A position is started iff its start marker has any
bit at 0. The write path takes the first position below the current record whose start marker reads
all erased, and pins the start marker of every started position it skips on the way (a torn record,
a tombstoned one), so a skipped position stays solidly started. A start marker that was marginal and
reads erased is pinned again by the write that uses the position: harmless, nothing after a weak
start marker was ever programmed. The magic cannot play this role: it is written after the data, and
a weak magic programmed over would stay weak, since its 1 bits cannot be refreshed.

A unit is erased exactly once per cycle: right before its first record is programmed, at the
rollover into it, unconditionally. It is not erased when the write order leaves it, and never at
boot. Wear is unchanged, one erase per unit per cycle, and a torn erase is always redone before
anything is programmed over it, because the next attempt to use the unit erases it again. Today the
rollover only erases a unit that `is_erased()` reports dirty, and a weakly erased unit reads erased
until the record programmed over it starts to flip; the `prev_erased` flag and its boot repair exist
to close that gap. With the erase immediately followed by the programming, the gap is gone and so is
the flag.

Steady state, three units, write order U2 then U1 then U0 then U2 again, four records per unit; the
current unit is U2 and the others hold the two previous cycles:

```
       U2 (current)             U1 (oldest)               U0
      +------------------+      +------------------+      +------------------+
  p0  | v+9              |      | v+1              |      | v+5              |
  p1  | v+10             |      | v+2              |      | v+6              |
  p2  | v+11  current    |      | v+3              |      | v+7              |
  p3  | erased           |      | v+4              |      | v+8              |
      +------------------+      +------------------+      +------------------+
```

The write of v+13 rolls over, since v+12 fills U2: it erases U1, the next unit in write order and
always the oldest, then programs v+13 at its top.

```
       U2                       U1                        U0
      +------------------+      +------------------+      +------------------+
  p0  | v+9              |      | v+13  current    |      | v+5              |
  p1  | v+10             |      | erased           |      | v+6              |
  p2  | v+11             |      | erased           |      | v+7              |
  p3  | v+12             |      | erased           |      | v+8              |
      +------------------+      +------------------+      +------------------+
```

A cut during the erase of U1 leaves U1 partly erased and nothing else changed: the scan finds v+12
in U2, and the application's next write erases U1 again before programming. A cut during the
programming of v+13 leaves a torn record: its end marker reads all erased, so it is tombstoned and
v+12 stays current; U2 is full, so the next write erases U1 again and starts over. In both cases
the erase is paid for by a write the application asked for.

### 4c. Highest version over all units

With stale units the normal state, "the first valid record by increasing address" no longer
identifies the newest record, and the rule that patched it, "unless the next unit in write order
holds a newer one", goes too. The scan does what the regular-area scan does: the highest version
among candidates, with two valid candidates sharing a version an error. Inside a unit the newest
record is still the lowest candidate position, so each unit costs one pass from its bottom up to its
first candidate: one or two positions for a full stale unit, up to a unit's worth of positions for
the current one. Versions are unique among candidates: a tombstoned record can share its version with
the record written after it, a candidate cannot. Section 6 shows how to find the current unit and
its newest record without visiting every unit or every position.

## 5. Costs and residuals

- **Boot time.** Visited unit by unit, every unit contributes one candidate, so a boot would cost
  one pass and one checksum per unit, and an area with many erase units would pay for all of them.
  Section 6 brings that down to a number of reads logarithmic in the unit and record counts, which
  is below what today's scan costs.
- **Boot-time programming.** A pin or a tombstone is one word program, only after a torn write, and
  once per torn word, since the pinned word reads zeros for good. Under the repeated-cut attacker
  that is one word program per boot, no erase.
- **Stale data lives longer** (T1.8): a unit's old records stay until the write order returns to
  it, instead of being erased at the rollover that leaves it. Nothing reads them through the API.
- **ECC flash** (T1.13): a weak word is an inconsistent codeword, and its first read at the next boot
  raises the NMI; the pin or tombstone that follows programs a valid codeword over it, so the NMI
  does not come back. That is better than today, where a kept slot keeps its weak word until a
  later write erases it.
- **The timing residual** of section 2: a marker that reads complete at every boot and all erased
  before the next, a rollback by one version after exposure, never a loss.
- **The application's own boot-time writes** stay its own: a counter incremented at every boot costs
  a slot erase per boot in a regular area and an erase per `records_per_unit` boots in an EWLF
  area. That is T1.5's budget, and the reason to keep such counters in EWLF areas.
- **Memory layout.** Nothing changes for regular areas. EWLF records keep their size, but the
  `prev_erased` field becomes the start marker and `checksum2` an all-zero end marker: existing
  EWLF areas would need a format.

## 6. Boot time with many units

Lazy erasing orders the units, and that order can be searched instead of walked. Nothing in the
layout changes for this; the information is in the records already.

### 6a. The order lazy erasing creates

Call a unit's **key** the version of its top record, `p0`, the first record written into the unit
after its erase. Every record of a newer unit has a higher version than every record of an older
one, so keys order the units by age. Four units, four records per unit, write order U3 then U2
then U1 then U0 then U3 again, record versions counted from format; the first cycle filled U3 to
U0 with versions 1 to 16, the second cycle has reached U1:

```
       U3                       U2                       U1 (current)             U0
      +------------------+      +------------------+      +------------------+      +------------------+
  p0  | 17               |      | 21               |      | 25               |      | 13               |
  p1  | 18               |      | 22               |      | 26  current      |      | 14               |
  p2  | 19               |      | 23               |      | erased           |      | 15               |
  p3  | 20               |      | 24               |      | erased           |      | 16               |
      +------------------+      +------------------+      +------------------+      +------------------+
  key   17                        21                        25                        13
```

Read in write order from the current unit, U1, U2, U3, U0, the keys only decrease: 25, 21, 17, 13.
In address order, U0 to U3, that is 13, 25, 21, 17: a decreasing sequence with a single jump up, at
the current unit. Units that cannot serve as keys get the lowest key of all: an erased unit (first
cycle), a unit whose top record is torn or tombstoned (the abandoned target of a torn rollover,
always the next unit in write order), or a unit left partly erased by a torn erase. They form one
run at the old end of the cycle, so the shape holds.

A key is valid only if its top record is a candidate in full, checksum included. The magic and the
marker are not enough: erasing only sets bits to 1, so the top record of a unit whose erase was cut
early can keep a valid magic and a programmed marker while a few cells of its *version* word have
flipped, which reads as a larger number, possibly larger than the current unit's key. The checksum
covers the version and rejects it; the unit then takes the lowest key, where it belongs. A partly
erased unit may also read differently from one probe to the next, so a unit's key is read once per
scan and cached: whether it reads as its old version or as no key, it sits at the bottom of the
order either way.

### 6b. Finding the current unit

A cyclic sequence that decreases from its maximum is searched by halving. Keep a range of units that
contains the jump, probe its middle unit, and compare its key with the key of the range's last unit:
higher means the jump is at the middle or before it, otherwise it is after. Each probe is one header
read and one checksum, and the number of probes is the number of times the unit count can be
halved, plus one for the first last-unit read. Eight units, 8 records each, the second cycle has
reached U2:

```
       U7         U6         U5         U4         U3         U2         U1         U0
      +--------+ +--------+ +--------+ +--------+ +--------+ +--------+ +--------+ +--------+
  p0  | 65     | | 73     | | 81     | | 89     | | 97     | | 105    | | 57     | | 49     |
  p1  | 66     | | 74     | | 82     | | 90     | | 98     | | 106    | | 58     | | 50     |
  ..  | ..     | | ..     | | ..     | | ..     | | ..     | | erased | | ..     | | ..     |
  p7  | 72     | | 80     | | 88     | | 96     | | 104    | | erased | | 64     | | 56     |
      +--------+ +--------+ +--------+ +--------+ +--------+ +--------+ +--------+ +--------+
  key   65         73         81         89         97         105        57         49

  range U0..U7:  read U7, key 65
  probe U3:      key 97 > 65   the jump is in U0..U3
  probe U1:      key 57 > 97?  no, it is in U2..U3
  probe U2:      key 105 > 97  it is in U2..U2: U2 is the current unit, after 4 reads
```

The result is checked before it is trusted: the current unit is a local maximum in write order, so
its two neighbours must have lower keys or no key, and the record the search ends on (section 6c)
must have a higher version than both. That costs two more header reads. When the check fails, which
only hostile content (Case 2) or a bug can cause, the scan falls back to the linear pass over the
units, the baseline of section 4c. Unit indices stay inside the area either way, so the
memory-safety argument of THREAT-MODEL.md is untouched: what hostile keys can do is make the scan
pick the wrong unit, and data integrity is not a Case 2 goal.

### 6c. Finding the newest record inside it

Positions are started from the top down: the write path only uses a position whose start marker
reads erased below the current record, and pins the start markers of the positions it skips. So
"position started" is monotone inside the current unit, a run of started positions then a run of
unstarted ones, and the boundary is found by halving too, with one probe reading one write unit.
The magic alone is not monotone: a write cut during its data leaves a position with data but no
magic, which the next write skips. Erased-ness of the whole position is not monotone either: that
same torn position can read all erased if the cut came during its first word, and a skipped torn
position whose weak word drifts to erased would open a hole in the run. The start marker is
monotone because a skipped position's marker is pinned, and because the only start marker that can
be weak is the last started one, which is read once per scan and gives the same newest record
whichever way it reads: nothing after a weak start marker was ever programmed.

```
       U2 (current)
      +------------------------------------------+
  p0  | started   105                            |
  p1  | started   106                            |
  p2  | started   data, no magic: a torn write   |   skipped by the write of 107, which pinned its start marker
  p3  | started   107  newest                    |
  p4  | erased                                   |
  p5  | erased                                   |
  p6  | erased                                   |
  p7  | erased                                   |
      +------------------------------------------+

  probe p3:  started, the boundary is below it
  probe p5:  not started, the boundary is above it
  probe p4:  not started: p3 is the last started position, after 3 reads of one write unit each
```

The newest candidate is the last started position when it holds a candidate, or the first candidate
above it when it holds a torn or tombstoned record; then one checksum, and the pin or tombstone of
section 2 if its end marker calls for one.

### 6d. What it adds up to

A boot costs a number of header reads and checksums logarithmic in the unit count, a number of
single-unit reads logarithmic in the records per unit, two header reads for the check, one
checksum for the record found and at most one word program, whatever the size of the area.
Today's scan reads one top magic per erased unit and walks the current unit from its bottom, so it
grows with both counts; the within-unit search on start markers needs the start marker, the unit
search needs lazy erasing, because today only one unit is programmed and the others are erased,
which gives no order to search along. Both searches rest on two properties EWLF already has, a
strict write order (the unit below, wrapping to the top) and the top record being the first one
written into a unit. Regular areas keep their one version read per slot, which is fine at the
wear-leveling factors they are used with, and their layout stays as it is.

## 7. Distance from today's lean-ftl

Less than the list above suggests: the regular-area scan is already max-version, the whole-unit
reading of `checksum2` exists since 426fcb1, and `write_ewlf_core()` already finds the next free
position by reading the cells. The changes would be:

- `find_current_slot()`: pin `checksum2` when it is partially programmed, tombstone the version
  item instead of `erase_slot()` when it is all erased; `other_valid_slot()` stays as the guard for
  the only-slot case. No layout change: this part can be done on its own (LFTL-011).
- `find_current_record()`: the two searches of section 6, with the linear pass of section 4c as the
  fallback, and the pin or tombstone of the record found; `ewlf_record_prev_erased_confirmed()`,
  `ewlf_write_record_prev_erased()` and `copy_forward_ewlf()` go, and with them the `copy_current`
  parameter of `write_ewlf_core()`.
- `write_ewlf_core()`: program the start marker first, the end marker as zeros last; choose the
  position by start markers and pin the skipped ones; at a rollover, erase the target unit
  unconditionally and do not erase the vacated one. `format_ewlf()` writes a start marker and a
  zero end marker and no flag.
- Tests 24, 33 and 34 rewritten around pinning and tombstoning; test 29's wrap case and tests 22
  and 23's erase counts re-derived for lazy erasing; the raw-state builders of tests 28 and 29
  given a start marker.
- EWLF.md's write and read process, THREAT-MODEL.md's overview, O1, O2, T1.10, T1.13, the
  `checksum2` and `prev_erased` rows of the Case 2 table, and LFTL-010 and LFTL-011 closed.
