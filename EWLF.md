# Extreme Wear Leveling Factors
This outlines the concept to support the EWLF feature.

## Concept

The main idea is to store several versions of the same data into the same erase unit.
Unlike normal LFTL areas, in EWLF areas we store the data and the meta data packed together.
If we can store 100 times the data within an erase unit, then the wear leveling factor is 100 times the number of erase units that we allocate to the data.

The main assumptions are:
- the data is small compared to the erase unit
- the write unit is equal to or smaller than the data
- erased bytes read `0xFF`: free record positions are recognized by that value (R6 of the documentation's [Flash requirements and assumptions](https://lean-ftl.readthedocs.io/en/latest/flash.html#flash-required) page)

The memory layout of a record is the following:
- header
    - header magic: size is max(sizeof(uint64_t),LFTL_WU_SIZE)
    - start marker: one write unit, max(sizeof(uint8_t),LFTL_WU_SIZE), programmed to all zeros, and first
    - meta data (lftl_meta_t)
        - version
        - checksum
        - end marker: one write unit, programmed to all zeros, and last (the `checksum2` item of regular areas)
- data
- pad (if needed)

The writes are done from the highest possible address towards lower address, and the erase units are used in write
order from the highest one down, wrapping back to the highest one.

### Reading

Records can only start at the top of an erase unit minus a multiple of the record size, so only these positions are
checked: data can never be mistaken for a header, even if it contains a copy of one, and every record checked fits in
its erase unit. A position holds a *candidate* when its magic matches, its checksum is the one of its data and
version, and its end marker is not all erased. The current record is the candidate with the highest version over all
erase units (two candidates sharing a version is an error, `LFTL_ERROR_VERSION_COLLISION`); inside a unit the lowest
candidate position is the newest.

The scan reads and chooses; it never erases, so that a power cut at every boot costs no erase (ISSUES.md LFTL-010,
closed). The one word a power cut can leave weak is the end marker of the interrupted write, and the scan decides
about it on a single reading:
- end marker programmed in part: its own program had started, so the checksum before it is complete and the record
  is stable: it is taken;
- end marker all erased: the cut came during the checksum word at the latest, which may be weakly programmed and
  read differently at a later boot: the record is not taken, the previous one is current.

On a flash declared with `zero_overwrite` (the STM32L5 and STM32U5, N3 of the flash page) the scan pins the decision
with one word program: a partially programmed end marker is programmed to all zeros, so that it cannot read all
erased later and bring the previous version back (LFTL-011, closed); a record not taken is tombstoned, its version
item programmed to all zeros, so that its checksum can never verify again. Without `zero_overwrite` the scan programs
nothing, the same choice is made again at every boot on the word as it reads then, and the next write settles it
(section 7 of READ-ONLY-SCAN.md).

Boot time does not grow with the number of erase units. The units are ordered by the version of their top record,
their *key*: the top record is the first one written after the unit's erase, so every record of a newer unit has a
higher version than every record of an older one. The current unit is found by halving on that cyclic order and,
inside it, with `zero_overwrite`, the last started position by halving on start markers; the result is checked
against the two neighbouring units, and the linear pass over every unit is the fallback (section 6 of
READ-ONLY-SCAN.md). The Linux test build runs both at every scan and raises `LFTL_INTERNAL_ERROR` if they disagree.

### Writing

- read the current version
- find the next free space: the first record position below the current record, in its erase unit, whose start
  marker reads all erased, or else the top of the next erase unit in write order, which is erased first, whatever it
  reads
- write the start marker (all zeros)
- write the data
- write the magic, then the version and the checksum (computed from the data meant to be written, not read back)
- write the end marker (all zeros)

NOTES:
- A started position on the way to the free one holds a record a power cut tore, or one the scan tombstoned. It is
  skipped, and the new record is numbered above its version when its checksum is valid, so that two candidates can
  never share a version whatever that record's end marker reads later. With `zero_overwrite` its start marker is
  programmed to zeros again when it does not read all zeros, so that a weak start marker cannot later read erased and
  open a hole in the run of started positions the in-unit halving relies on.
- A unit is erased exactly once per cycle, right before its first record is programmed, and never when the write
  order leaves it: the other units keep their stale records until their turn comes. A torn erase is redone by the next
  attempt to use the unit, and the cells a torn record left behind are never programmed over without an erase
  (LFTL-013, closed). A rollover is therefore exactly one erase, in every state.
- Nothing after an erased start marker was ever programmed: the start marker is the first word of a record to be
  programmed, which is why it, and not the magic, says whether a position is started.

Example with an area spanning 2 erase units, U1 at the higher address and U0 at the lower one:
- writes start at the top of U1 and proceed downward as records accumulate
- once U1 has no room left, the next write erases U0 (the unit immediately below) and starts again from U0's highest address; U1 keeps its records
- once U0 in turn fills up, since it is the lowest unit in the area, the write rolls over back to U1: U1 is erased and the record goes to its top
- the area cycles between these two states indefinitely; the unit that is not current holds the previous cycle's records until it is erased

Limitations:
- one area can store only one data of fixed size
- at least 2 records must fit in an erase unit (a constraint kept from when a torn record was copied forward within its unit)
- the whole data is written every time
- no support for transactions

NOTE: Some writes are slow compared to others since they involve an erase while most don't: exactly the writes that roll over to the next erase unit.

## Does each word earn its place?

Taken field by field before the next layout change, on the code of v0.2.0 (`write_ewlf_core()`,
`ewlf_record_state()`, `ewlf_choose_fast()`, `find_current_record()`). The version and the checksum are not in
question. Every other field is judged on three points: what the code uses it for, what else could do that job, and
what breaks without it. EWLF is not in v0.1.0, the format the compatibility test (test 37) guards, so the layout can
change without a migration.

### What a record costs today

The header is the magic, `max(write unit, 8)` bytes, the start marker, `max(write unit, 1)`, and three items of
`max(write unit, 4)` each; the data is rounded up to whole write units and the record to a multiple of 8 bytes. For
16 bytes of data:

| Target | Write unit | Header | Record | Records per erase unit |
| --- | --- | --- | --- | --- |
| STM32U5 | 16 | 80 | 96 | 85 (8 KB) |
| STM32L5 | 8 | 40 | 56 | 36 (2 KB) |
| M5531 | 4 | 24 | 40 | 204 (8 KB) |
| CH32V307 | 2 | 22 | 40 | 102 (4 KB) |
| W25Q32RV | 1 | 21 | 40 | 102 (4 KB) |

On the STM32U5 the header is five times the data, and one write unit less is one record in five. The table at the
end of this section gives the figures per target for the layouts discussed.

### Magic

**What the code does with it.** `ewlf_record_state()` reads it first and answers "no record" without reading the
meta-data or checksumming the data when it does not match. `write_ewlf_core()` programs it third, after the start
marker and the data. The checksum does not cover it. Its value did not change when the start marker and the all-zero
end marker were introduced, so it has never served as a format tag.

**What it decides.** Nothing on its own. Every decision is taken on the checksum: a unit's key, a candidate, a torn
record. The design page says it of the key: "the magic and the marker alone would not do". Against hostile content
(threat model, case 2) it is a constant, forged for free.

**What it buys.** Three things:

- a format tag: an area holding something else, an area registered at the wrong address or a record of an older
  layout reads as `LFTL_ERROR_NO_VALID_VERSION` instead of being parsed. That works only if the constant changes with
  the layout;
- an early out: a position that is erased, torn before its magic or partly erased costs one 8-byte read instead of
  a checksum over the data;
- a deterministic rejection of a record torn during its data, where the checksum alone gives 2^-32.

**Without it.** Foreign content becomes the checksum's problem, 2^-32 per position; a formatted area never holds
foreign content at a position, since every unit is erased right before its first record, and an unformatted one may,
where one position in four billion would then read as a record. An erased position costs a start marker read, one
write unit, cheaper than the magic. A record torn during its data has its version and checksum items erased,
`0xFFFFFFFF` each, and is rejected unless the CRC32C of its partial data is 0; the deterministic rejection then needs
the rule regular areas already have, "version `0xFFFFFFFF` is no version". Without that rule the 2^-32 case is ugly:
the torn record is an unmarked record of version `0xFFFFFFFF`, the next write numbers above it and wraps to 0.

**Verdict.** The magic does not earn a write unit. A constant could do the start marker's job, next, but a word of
zeros does it better, so the magic goes.

### Start marker

**What the code does with it.** `write_ewlf_core()` looks for room at the first position below the current record
whose start marker reads all erased, steps over the started ones and, on a flash with `zero_overwrite`, programs the
start marker of each stepped-over position to zeros again unless it already reads zeros. `ewlf_choose_fast()` finds
the last started position of the current unit by halving on start markers, one write unit per probe, on a flash
with `zero_overwrite`. It is the first word programmed.

**What the job needs.** A word that says "this position was started", with three properties:

1. programmed first, so that an erased one proves that nothing after it was ever programmed, which is what makes the
   position safe to program (R3 of the flash page: only erased units are programmed);
2. one write unit, read alone, so that the search for room and the halving stay cheap;
3. programmed again without harm after its own program was cut. A cut during the first word leaves it weak; at a
   later boot it may read erased (T2), the position is then free again, and the word is programmed a second time
   over the same cells.

The third property is the one that constrains the choice. A word of zeros has it: the second program pushes the same
cells. A constant has it too: the cells intended at 0 are the same cells each time, and the others were never
touched. A value does not: the version of the next record differs from the version of the torn attempt, so some of
its cells intended at 1 are cells the first program half charged, and T2 says they may read 0 at a later boot. The
record then fails its checksum at some boot after it was taken: a rollback by one version that no pin can close,
since the weak cells are inside a value. So the version cannot be the first word, and that is the reason the start
marker exists. That it is all zeros is one of the two ways to satisfy the third property, not the only one.

**Verdict.** The job is needed, and the word stays. Two values satisfy the three properties, zeros and a constant
such as the magic programmed first instead of third, and zeros win on every count that matters:

- size: a zero marker is one write unit, `max(write unit, 1)`; a 64-bit constant is `max(write unit, 8)`, which is
  4, 6 and 7 bytes more per record on the M5531, the CH32V307 and the W25Q32RV, and nothing on the STM32L5 and
  STM32U5;
- the step-over pin: a stepped-over zero marker already reads zeros unless it is weak, so the write path programs
  nothing in the common case and a stepped-over record stays readable, as today. A constant would be pinned to zeros
  at every step-over, which tombstones the record: that works too, but zeros change nothing;
- weight: every cell of the marker is programmed, where a constant programs about half of them. The "started" test
  does not lean on T3, so this is margin, not an argument;
- consistency: regular areas have no magic, and a deployed fleet reads them.

What a constant would add is the format tag and the early out of the magic section, and the paragraphs above put a
low price on both. With zeros, a position holds a record when its start marker is not all erased and its checksum
verifies, with the `0xFFFFFFFF` rule; the search for room, the halving and the pins are unchanged. A record of the
current layout read by the new code has its non-zero magic where the start marker is expected: started, and rejected
on its checksum. EWLF has no deployed areas, so no migration is needed.

### End marker

**What the code does with it.** Programmed last, to zeros, after the version and the checksum.
`ewlf_record_state()` turns a valid checksum into a candidate when the marker is not all erased and into an unmarked
record otherwise; `find_current_record()` pins a partially programmed marker to zeros on a flash with
`zero_overwrite`.

**What it decides.** The one decision a power cut makes hard: whether the checksum before it is complete. A cut
during the checksum word leaves a word that may read valid at one boot and not at the next, and the marker behind
it, all erased, is the one sign of it. The marker's own test is "not all erased", which a partial program satisfies
for good on a flash that can pin, and which T3 keeps satisfied on any flash once the marker read complete, since its
32 zero bits are 16 programmed bits on every geometry.

**What else could do it.**

- Nothing, a record whose last word is a value. That is OpenTitan's `boot_data`, an append-only log with a digest
  and no marker: a cut during its last word leaves an entry valid at some boots and not at others, no word to pin,
  and no help from T3 since a value must read exactly. The full T2 flip-flop, in both directions, until the next
  write.
- A copy of the checksum, `checksum2` as in regular areas: the same test, but its programmed bits are the
  checksum's, about 16 on average and often fewer. Regular areas get the weight from the zero padding of 8- and
  16-byte items, which 2- and 1-byte write units do not have. The all-zero marker is the choice that works on every
  geometry.
- The first word of the next record: the current record has none.

**Size.** `max(write unit, 4)`. Two bytes would carry T3's 16 bits on the two targets with write units under 4
bytes and save 2 bytes per record there; not worth a second item size.

**Verdict.** Earns its place as it is.

### Data rounding and the 8-byte pad

The data is rounded up to whole write units because writes cover whole units (R2, R3); an unaligned `lftl_write()`
extends into that room. Not a field, and not in question.

The pad rounds the record up to a multiple of 8 bytes so that every position, and so every magic, is 8-byte aligned
(`ewlf_record_phy_size()`). The library never dereferences a record: every read goes through the NVM's read callback
into a local buffer, and the read callbacks of `target/` are `memcpy()`. The pad is empty on 8- and 16-byte write
units and costs up to 4, 6 and 7 bytes per record on the M5531, the CH32V307 and the W25Q32RV. Without the magic
it has no reason left at all.

**Verdict.** Drop it with the layout change. A record is then a multiple of the write unit, which is all a position
needs.

### A reserved word, if one is added

**What it would be.** An item the writer leaves erased, programmed later from erased: by the scan to record a
decision, or by the writer to retire a record, as `boot_data` does. A program of an erased word needs R3 only: no
`zero_overwrite`, and on flash with ECC a fresh codeword instead of a program over an inconsistent one, which is the
open measurement of T1.13.

**What it would not do.** Replace the end marker: `boot_data` has a retire word and no marker, and gets the
flip-flop above.

**What it would buy**, decision by decision, against the
[residual table](https://lean-ftl.readthedocs.io/en/latest/design.html#design-residual) of the design page:

- the pin of a partially programmed end marker, row 1: a rollback by one version after the record was shown, closed
  today on `zero_overwrite` flash only;
- the tombstone of a torn record newer than the one taken, row 3: a late new by one version. The write path's
  numbering already prevents the collision, so that is all it closes;
- one word holds one durable bit, and a partial program blurs any value encoding, so the two decisions need two
  words, or the first one alone.

**Who gains.** Targets without `zero_overwrite`: the W25Q32RV, the CH32V307, the M5531 until measured. On the
STM32L5 and STM32U5, only the ECC codeword point.

**Cost.** One item, 16 bytes per record on the STM32U5: the write unit the magic gives back, spent again.

### The record each choice gives

| | Today | Magic and pad dropped | Same, plus one reserved word |
| --- | --- | --- | --- |
| Fields, in address order | magic, start marker, version, checksum, end marker, data, pad | start marker, version, checksum, end marker, data | start marker, version, checksum, end marker, taken, data |
| Order of programming | start marker, data, magic, version and checksum, end marker | start marker, data, version and checksum, end marker | same; `taken` by the scan only |
| Header, 16-byte write unit | 80 | 64 | 80 |
| Header, 8-byte write unit | 40 | 32 | 40 |
| Header, 4-byte write unit | 24 | 16 | 20 |
| Header, 2-byte write unit | 22 | 14 | 18 |
| Header, 1-byte write unit | 21 | 13 | 17 |
| Records of 16 bytes per unit: STM32U5, STM32L5, M5531, CH32V307, W25Q32RV | 85, 36, 204, 102, 102 | 102, 42, 256, 136, 141 | 85, 36, 227, 120, 124 |

Recommendation: the middle column. Drop the magic and the pad, keep the start marker as it is, add the `0xFFFFFFFF`
rule to `ewlf_record_state()`; nothing else in the scan or the write path changes. The reserved word is worth its
unit only where a target without `zero_overwrite` is a deployment rather than a port; it is then the taken-pin,
programmed by the scan when it takes a record whose end marker read partially programmed, and by nothing else.

## Support for extended transactions
EWLF could be the base to support extended transactions which can involve data from several areas, including EWLF areas.
This would be done by allocating a 'system' EWLF area which would be used to track the state of such transaction. Each record
would contain enough information to let the init function roll back if a tearing happens.
This would need some adaptation of current functions, to prevent them to erase old version before the extended transaction is marked completed.


## EWLF without rollback without T3

Current status: do we have roolback issue ?

- zero_overwrite=true and T3 -> no: a partially programmed end marker is pinned at the first boot that shows V, and a complete one cannot drift to erased. 
- zero_overwrite=false -> yes, through the partially programmed marker, until the next update. 
- Without T3 -> yes on every flash, through the complete marker. 
- With the second end marker -> no on any flash, with the two-cut residual as the only exception. The late-new row is V−1 then V, which your definition does not count.

"no on any flash, with the two-cut residual as the only exception" actually means "yes" ! So here we try to make a design
that really does not have rollback without T3 and zero_overwrite=true.

Concept:
- we have the following fields, each as a full programming words:
  - version
  - checksum
  - flag-a (boolean, physically all ones/all zeroes)
  - flag-b (boolean, physically all ones/all zeroes)
  - valid (boolean, physically all ones/all zeroes)
- programming order:
  - step 0: flag-b of previous page (overwrite)
  - step 1: data, version, checksum
  - step 2: flag-a
  - step 3: valid of previous version (valid=0, so previous page is now invalid)
  - step 4: flag-b


notation: 
- 1: all bits are 1
- !1: not all bits are 1, but not all bits are 0
- 0: all bits are 0

when version is latest and checksum is valid:

    flag-a | flag-b | valid | prev-valid | meaning
       1   |    1   |   1   |      1     | tearing before flag-a -> invalidate and never exposed to app, read return previous page
       1   |    1   |  !1   |      1     | tearing during invalidation -> invalidate and never exposed to app, read return previous page
       1   |    1   |   0   |      1     | invalidated, never exposed to app, read return previous page
      !1   |    1   |   1   |      1     | tearing on flag-a -> repair before exposing to app
       0   |    1   |   1   |     !1     | tearing on prev-valid -> repair before exposing to app
       0   |   !1   |   1   |      0     | tearing on flag-b -> repair before exposing to app
       0   |    0   |   1   |      0     | page looks good, expose to app

repair operation: step 2,3 and 4
invalidate operation: program valide of the latest page

### Review of the proposal

Not as written. The shape is right: flag-a says the data is complete, flag-b says flag-a is solid, and nothing
is exposed before the repair is done. The table is not exhaustive, though, and two of the missing rows hold a
rollback. The `valid` word and the steps on the previous page are where it breaks.

**Counter-example 1, the kill against the repair.** Two cuts.

1. The write of V is cut during flag-a. Flag-a is weak.
2. Boot 1: flag-a reads 1. Row 1: the scan programs `valid` of V, and the power is cut during that program.
   `valid` is weak too.
3. Boot 2: `valid` reads 1, flag-a reads !1. Row 4: repair, then expose. The application reads V.
4. Boot 3: `valid` reads !1. The state is flag-a 0, flag-b 0, `valid` !1, prev-valid 0. No row covers it. By
   row 2, a `valid` word that is not all ones means invalidated, so the scan returns the previous page. The
   application reads V-1.

If flag-a and flag-b win over `valid` instead, there is no rollback, but a completed kill is then overridden the
first time flag-a reads !1, so `valid` guarantees nothing. There is no safe kill word programmed in place: a
word the scan programs to force "dead" can itself be left weak by a cut, it then reads 1 at one boot and !1 at
another, and the alive path has no way to erase it. "Never exposed to app" in rows 1 to 3 is therefore not
achievable this way, and the definition of rollback does not need it: V-1 then V is allowed. The next section
has a kill that is safe, because its alive path does not depend on the killed record's words.

**Counter-example 2, a weak word that reads zeros.** The write of V is cut during flag-a, and flag-a happens to
read 0. Step 2 of the repair re-programs flag-a only if the code programs a word that already reads all zeros:
`pin_unit()` deliberately never does, and whether the STM32L5 and STM32U5 accept that program is open point 2 of
FLASH.md. If the repair skips flag-a and is then cut during flag-b, the next boot can read flag-a 0 and flag-b 0
with both words weak: row 7, V exposed. A later boot reads both 1, with prev-valid 0: the previous page is
invalidated and V is gone. An in-place repair therefore needs "all zeros over a word that reads all zeros", and
a measurement that the program really pulses the cells, since a PROGERR without a pulse passes the accessor's
read-back and silently reopens the hole.

**prev-valid and step 0** take no part in the decision. prev-valid belongs to another record, so it cannot
witness anything about this one, and if the positional predecessor was itself killed, its `valid` reads 0 for a
reason unrelated to this record. Step 0 re-programs a word that is already solid in every case constructed
here, and is zeros over zeros in the common case. Both go, unless they protect something not seen here.

**What the table gets right**, and what the next section keeps: a flag programmed only once the data is
complete, a second flag programmed only once the first is complete, exposure only after the repair, and the
repair done by the scan, before the application runs.

## Two flags, repair by copy, sticky abort

This is the design the branch `ewlf-two-flags` implements. The section was first written as a proposal and
then reviewed; "What the review found", below, lists the holes it had and the rules that close them. The
rest of the section is the corrected design.

### Record

A record is: start marker, meta field, flag-a, flag-b, data. The start marker and each flag are one write
unit. The meta field holds the 32-bit version and a 64-bit checksum, 12 bytes rounded up to whole write
units, and is programmed in one call. The data is rounded up to whole write units. No magic, no pad.

| write unit | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| header bytes | 15 | 18 | 24 | 40 | 64 |
| records of 16 B of data per erase unit (W25Q32RV, CH32V307, M5531, L5, U5) | 132 | 120 | 204 | 36 | 102 |

The checksum is CRC-64/NVME (polynomial `0xAD93D23594C93659`, reflected `0x9A6C9329AC4BC9B5`, init and
final XOR all ones) over the four version bytes then the data, computed from the data meant to be
written, as today. A version that reads `0xFFFFFFFF` (erased, or torn in the meta field) or `0` (a
tombstone) is no record, and that test comes before the checksum, so an erased position costs one small
read. Flag-a is today's end marker, flag-b the reserved word of "Does each word earn its place?"; the
third column of that section's table is this record, with the smaller header. The model is T1, T2 and R3
of the flash page. T3 is not used. `zero_overwrite` buys the sticky abort and the stuck-state pin.

Writer, each step completed before the next one starts:

1. start marker, zeros;
2. data;
3. meta field, one program;
4. flag-a, zeros;
5. flag-b, zeros.

Two facts follow from the order, whatever the cuts, because the scan programs flag-a or flag-b in place
in one situation only, the stuck state below, where they belong to a record whose data is already solid:

- flag-a has a programmed bit only if step 3 completed: data, version and checksum are solid;
- flag-b has a programmed bit only if step 4 completed: flag-a is solid zeros.

### States and scan

From one read of the meta field and both flags, a position is one of:

    state    | checksum | flag-a   | flag-b   | how it got there
    NONE     | invalid  | x        | x        | erased, torn before or during the meta field, tombstoned, foreign
    ABORTED  | valid    | erased   | erased   | cut before flag-a, or flag-a weak and reading erased
    HALF     | valid    | has a bit| erased   | cut during flag-a, between the flags, or during flag-b with flag-b reading erased
    COMPLETE | valid    | x        | has a bit| complete, or cut during flag-b

Notation as in the previous section: `x` is any reading. The scan collects the newest COMPLETE record
(`complete`), the newest HALF (`half`), the newest ABORTED (`torn`, with which units of its meta field read
all zeros) and the highest version of every valid record it saw, then loops, at most one pass per record
position plus two, over:

1. `torn` newer than both `complete` and `half`: with `zero_overwrite`, tombstone it and start over.
   Without `zero_overwrite`, ignore it.
2. `half` newer than `complete`: if a position exists for a copy without erasing a protected unit, copy it
   forward and start over, so that the copy is found as COMPLETE. Otherwise, the stuck state: with
   `zero_overwrite`, pin flag-a of `half` then flag-b in place and start over; without, expose `half`.
3. Expose `complete`. None: `LFTL_ERROR_NO_VALID_VERSION`.

`ctx->data` is assigned once, at the end. Every version comparison is strict, and
`LFTL_ERROR_VERSION_COLLISION` is raised for two COMPLETE records of one version only: that stays
impossible (COMPLETE implies a solid meta field and the numbering below), while a HALF or ABORTED record
sharing a version with a COMPLETE one is reachable after a cut tombstone and harmless.

Tombstone: the meta field programmed to zeros, every write unit of it that did not read all zeros in the
reading the decision was made on, checksum units first. After it the version reads 0 and the record is
NONE for good. A cut tombstone can leave the meta field weak; the record then reads NONE or ABORTED at
later boots, and ABORTED is tombstoned again.

Copy: the record's data is written again as a new record through the write path, with the half record as
the current one (next position below it, stepping over started positions), the physical address of its
data as the source, the version base taken from the scan's own validated readings and never from a
re-read meta field, and a rollover allowed only into a unit that holds neither `complete` nor any HALF
record newer than `complete`. The copy ends with a solid flag-b, since the scan returned from its program.
If the copy is cut, nothing was exposed at this boot, and the next boot finds the torn copy as the newest
record and applies the same rules to it.

Stuck state: no position for the copy. It is reached after a cut in a flag of a write at the top of a unit
followed by r-1 power cuts each landing inside a repair copy, r records per unit, in a two-unit area;
larger areas offer more positions first. The pin then makes the record COMPLETE for good once it returns,
and a cut pin is retried at the next boot. Its one residual: a weak flag that reads all zeros is left
unprogrammed on a flash that refuses zeros over zeros (FLASH.md open point 2), and a later reading of it as
erased can turn the record ABORTED after it was exposed. Where the flash accepts that program, the
STM32L5 and U5 to be measured, the pin programs unconditionally and there is no residual at all.

Step-over: the writer, stepping over a started position whose checksum fails, tombstones its meta field
with `zero_overwrite`, as it already re-pins its start marker. A record whose meta field reads garbage at
this boot must not come back later with a version the writer is about to reuse.

### Why there is no rollback

A record is exposed either after a program the scan completed in this boot (the copy's flag-b, or the
stuck-state pin), or on a reading of flag-b with a programmed bit, which proves flag-a solid. A solid
zero word never reads all ones again, so an exposed record is HALF or COMPLETE at every later boot, it is
never tombstoned (only ABORTED readings are), its unit is never erased by a copy (protected) nor by the
application's writer (which erases only the unit below the current record's), and a newer record takes its
place only when it is exposed under the same rules. The application can read V then V, or V then V+k;
never V then V-1. Any number of cuts, any flash under T1 and T2, with the stuck-state residual above as
the only exception. Rows 1 and 2 of the residual table of `design.rst` are closed; row 3, late new, is
closed by the tombstone when it completes, and is V-1 then V, not counted, when it is cut.

Why the abort sticks where `valid` did not. Both are a word the scan programs over a record it rejects,
and both can be left weak by a cut. With `valid`, the record came back in place, so its weak kill word
kept a say at every later boot and a reading of it could take the record away after the application had
seen it. Here a tombstoned record comes back, if at all, only to be copied, and the copy is a new record
whose fate no word of the old one decides. A completed tombstone is final.

### What the review found

The first version of this section tombstoned the half record in the stuck state, protected only the copy's
source from the rollover, let the write path number the copy from a re-read meta field, and let two half
records share a version. Each was a rollback or a loss:

1. **Tombstoning the half record.** A write cut in flag-b has a solid flag-a and is exposed at a boot where
   flag-b reads partially programmed. At a later boot flag-b reads erased, the record is HALF and copied,
   the copy is cut, and when no room is left the stuck state tombstoned the source: V then V-1. A HALF
   record is never tombstoned; the stuck state pins instead.
2. **The chain erasing its ancestor.** Copies cut in flag-a are mortal (their flag-a may read erased
   later); the exposed ancestor is in another unit, and a chain long enough to roll over could erase that
   unit, then every remaining copy reads ABORTED: loss after V was exposed, or V-1. A copy never erases
   the unit of `complete` nor one holding a HALF record newer than `complete`.
3. **The version base.** The write path read the current record's version without a checksum; a half
   record whose meta field is weak after a cut tombstone can read a smaller value, the copy is numbered
   below `complete`, and a stale COMPLETE record of that number collides. The scan passes the base.
4. **Two half records of one version.** A write aborted and tombstoned, the tombstone cut, its meta field
   reading garbage when the next write is numbered, that write reusing the number and cut in flag-b: at a
   later boot both read HALF with the same version, and the older write could be copied after the newer
   was shown. The step-over tombstone and the strict comparisons close it.

Smaller points from the same review, all applied: the fast scan keys a unit on its top record whenever
the checksum verifies, walks the whole next unit in write order instead of its top record, and falls back
to the linear pass when the current unit holds no COMPLETE record; the copy bypasses the source
translation of the application write path; a tombstone on a multi-unit meta field skips units that
already read zeros; `ctx->data` is never left on a half record when a copy fails; the test harness must not
take `LFTL_INTERNAL_ERROR` for a simulated cut.

### Costs and bounds

- One position per copy, after a cut during flag-a or between flag-a and flag-b; one word program per
  tombstone, after a cut before or during flag-a; a flag pin in the stuck state. Nothing is programmed at
  boot otherwise.
- A record that was exposed is never lost: not tombstoned, not erased by the scan, and the application's
  writer erases only the unit below the current record's. The two-page `boot_data` log does worse: 17
  consecutive torn writes erase its last complete entry.
- Without `zero_overwrite` (CH32V307, W25Q32RV) there is no tombstone and no pin: an aborted write may come
  back once through a copy, V-1 then V, and the stuck state exposes the half record directly, which is a
  rollback only if that record was never exposed and its flag-a later reads erased. Today those flashes
  roll back after a single cut; with this design after r cuts in a row.
- The start-marker pin of the step-over is unchanged and plays no part here: it lets the fast scan halve on
  start markers, and without it the current unit is read bottom-up, as today.
- EWLF areas written by v0.2.0 are not readable by this layout and must be formatted again; regular areas
  are untouched. v0.1.0 had no EWLF.
