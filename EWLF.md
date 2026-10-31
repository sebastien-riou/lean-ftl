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

## Support for extended transactions
EWLF could be the base to support extended transactions which can involve data from several areas, including EWLF areas.
This would be done by allocating a 'system' EWLF area which would be used to track the state of such transaction. Each record
would contain enough information to let the init function roll back if a tearing happens.
This would need some adaptation of current functions, to prevent them to erase old version before the extended transaction is marked completed.
