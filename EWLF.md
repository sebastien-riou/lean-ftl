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
    - header magic (uint64_t)
    - meta data (lftl_meta_t)
        - version
        - checksum
        - checksum2
- data
- pad (if needed)

The data identifier allows to place several variables into a single EWLF area.

The writes are done from the highest possible address towards lower address. This allows to quickly find the newest header.

The read process search the data identifier in headers, starting from the base address of the EWLF area
- check the header magic integrity:
- if the header integrity check failed, resume the search for the next header
- if data identified matches:
    - check the data integrity
    - if integrity is valid, return that version
- else, skip the length of the data associated with that area and resume the search for the next header

The write process is the following:
- read last version
- find the highest base address of the next free space in the area
- write the data
- write the header except checksum2
- write checksum2

NOTE: when the current erase unit does not have enough free space
the write targets the erase unit immediately below or roll over to the highest one
if that was the lower erase unit for that EWLF area. After the write, the previous erase unit is erased. 
As a results, all but one erase units within an EWLF area is programmed at any time under normal operations.
When a tearing occurs, the initialization of the library detect if two are programmed and erase the one containing
old or invalid data.

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
