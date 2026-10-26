# Extreme Wear Leveling Factors
This outlines the concept to support the EWLF feature.

## Concept

The main idea is to store several versions of the same data into the same erase unit.
Unlike normal LFTL areas, in EWLF areas we store the data and the meta data packed together.
If we can store 100 times the data within an erase unit, then the wear leveling factor is 100 times the number of erase units that we allocate to the data.

The main assumptions are:
- the data is small compared to the erase unit
- the write unit is equal to or smaller than the data
- erased bytes read `0xFF`: free record positions and the `prev_erased` flag are recognized by that value (R6 of the documentation's [Flash requirements and assumptions](https://lean-ftl.readthedocs.io/en/latest/flash.html#flash-required) page)

The memory layout of a record is the following:
- header
    - header magic: size is max(sizeof(uint64_t),LFTL_WU_SIZE)
    - prev_erased flag: size is max(sizeof(uint8_t),LFTL_WU_SIZE) 
    - meta data (lftl_meta_t)
        - version
        - checksum
        - checksum2
- data
- pad (if needed)

The writes are done from the highest possible address towards lower address. This allows to quickly find the newest header.

Records can only start at the top of an erase unit minus a multiple of the record size. The read process checks
these positions by increasing address, starting from the base address of the EWLF area, and returns the first
record whose header magic and data integrity (same approach as with regular areas) are valid. If its checksum2
does not match its checksum, a tearing happened while its meta data was written. When checksum2 reads all erased,
the cut came during the checksum word at the latest, which may be weakly programmed: the record is copied forward
first, as a new record with complete meta data, so that nothing depends on how that word reads at a later boot.
When checksum2 is partially programmed, its own program had started, so the checksum is complete and the record
is kept as it is. (A regular area cannot copy a torn slot without erasing another slot first: it erases the torn
slot instead, and the previous version is current.)

NOTE: checking only these positions means that data can never be mistaken for a header, even if it contains a
copy of one, and that every record checked fits in its erase unit.

The write process is the following:
- read the current version
- find the next free space in the area: the first record position below the current record, in its erase unit,
    whose cells all read erased, or else the top of the next erase unit (see the notes below)
- write the data
- write the header except checksum2 (the checksum is computed from the data meant to be written, not read back)
- write checksum2
- if current version was in another erase unit:
    - erase it
    - write `prev_erased` flag

NOTES: 
- When the current erase unit does not have enough free space
    the write targets the erase unit immediately below or roll over to the highest one
    if that was the lower erase unit for that EWLF area. After the write, the previous erase unit is erased. 
    As a result, under normal operations exactly one erase unit is programmed at any time (the others sit erased,
    waiting for their turn); at most one previously-used unit remains additionally programmed alongside it, and only
    for the brief duration of an in-progress transition, until that unit's erase completes.
- When a tearing occurs during a record write, it can leave a partial record below the current one. The next write
    skips any position whose cells are not all erased, since some flash refuses to program cells that are not erased.
    When the write rolls over to another erase unit and that unit is not erased (stale or partial records left by a
    tearing), it is erased before the new record is written.
- When a tearing occurs during the wrap from the lowest erase unit back to the highest, after the new record is
    complete but before the lowest unit is erased, both units are programmed and the scan finds the older record
    first, at a lower address. The initialization therefore also checks the newest record of the next erase unit in
    write order (a copy forward can leave two records there), takes it if its version is higher, and then completes
    the wrap as below.
- The initialization checks that the `prev_erased` flag of the top record of the erase unit holding the current
    record is set. If it is not set, the previous erase unit may not have been erased, or only 'weakly': it is erased
    again and after that the `prev_erased` flag is written. This comes after any copy forward, so nothing is erased
    before a complete record exists.

Example with an area spanning 2 erase units, U1 at the higher address and U0 at the lower one:
- writes start at the top of U1 and proceed downward as records accumulate
- once U1 has no room left, the next write goes to U0 (the unit immediately below), starting again from U0's highest address
- after that write completes, U1 -- the unit just vacated -- is erased
- once U0 in turn fills up, since it is the lowest unit in the area, the write rolls over back to U1 (already erased)
- after that write, U0 is erased
- the area cycles between these two states indefinitely; at every point exactly one unit is erased and the other is programmed

Limitations:
- one area can store only one data of fixed size
- at least 2 records must fit in an erase unit, so that a record torn at the top of a unit can be copied forward
    within that unit
- the whole data is written every time
- no support for transactions

NOTE: Some writes are slow compared to others since they involve an erase while most don't

## Support for extended transactions
EWLF could be the base to support extended transactions which can involve data from several areas, including EWLF areas.
This would be done by allocating a 'system' EWLF area which would be used to track the state of such transaction. Each record
would contain enough information to let the init function roll back if a tearing happens.
This would need some adaptation of current functions, to prevent them to erase old version before the extended transaction is marked completed.
