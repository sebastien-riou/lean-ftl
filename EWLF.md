# Extreme Wear Leveling Factors
This outlines the concept to support the EWLF feature.

## Concept

The main idea is to store several versions of the same data into the same erase unit.
Unlike normal LFTL areas, in EWLF areas we store the data and the meta data packed together.
If we can store 100 times the data within an erase unit, then the wear leveling factor is 100 times the number of erase units that we allocate to the data.

The main assumptions are:
- the data is small compared to the erase unit
- the write unit is equal to or smaller than the data

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

The read process search starting from the base address of the EWLF area:
- check the header magic
- if the header magic is incorrect, resume the search for the next header at the next uint64_t boundary.
- if the header magic matches:
    - check the data integrity (same approach as with regular areas)
    - if integrity is valid, return that version
- else
    - if only checksum2 check fails, skip the whole record and resume the search for the next header
    - else, resume the search for the next header at the next uint64_t boundary.

NOTE: if only checksum2 check fails, we assume we are dealing with a real record, so we can skip the whole record.

The write process is the following:
- read the current version
- find the highest base address of the next free space in the area
- write the data
- write the header except checksum2
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
- When a tearing occurs, the initialization of the library detect if two are programmed and erase the one containing
    old or invalid data.
- Even if a single erase unit appears to be programmed, the initialization of the library check that the `prev_erased` flag is 
    set. If it is not set, the previous erase unit may be 'weakly erased'. In such case, the previous erase unit is erased again and after that the `prev_erase` flag is written.

Example with an area spanning 2 erase units, U1 at the higher address and U0 at the lower one:
- writes start at the top of U1 and proceed downward as records accumulate
- once U1 has no room left, the next write goes to U0 (the unit immediately below), starting again from U0's highest address
- after that write completes, U1 -- the unit just vacated -- is erased
- once U0 in turn fills up, since it is the lowest unit in the area, the write rolls over back to U1 (already erased)
- after that write, U0 is erased
- the area cycles between these two states indefinitely; at every point exactly one unit is erased and the other is programmed

Limitations:
- one area can store only one data of fixed size
- the data size must be smaller than an erase unit (a record needs to fit in an erase unit)
- the whole data is written every time
- no support for transactions

NOTE: Some writes are slow compared to others since they involve an erase while most don't

## Support for extended transactions
EWLF could be the base to support extended transactions which can involve data from several areas, including EWLF areas.
This would be done by allocating a 'system' EWLF area which would be used to track the state of such transaction. Each record
would contain enough information to let the init function roll back if a tearing happens.
This would need some adaptation of current functions, to prevent them to erase old version before the extended transaction is marked completed.
