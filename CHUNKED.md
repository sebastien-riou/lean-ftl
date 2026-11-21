# Chunked areas: large data, partial updates, transactions, without rollback

> **The design of record is [LARGE.md](LARGE.md)**: on 2026-11-21 the combination "one chunk
> per erase unit, the map in the pair, one CRC-64 over the data" was chosen and named *large
> areas*. This file is the study it came out of. It still describes the wider design (several
> chunks per unit, the map in units of its own, no checksum of the data) and keeps its
> results.

This is the design proposal for a third type of area, next to regular and EWLF areas. A
regular area and an EWLF area both rewrite the whole data at every update: 64 KB of data
updated 500 000 times need fifty slots, 3.1 MB on the STM32U5A5, whatever the size of each
update. A *chunked area* cuts the data into chunks and rewrites only the chunks an update
touches. It keeps:

- **the guarantee of EWLF areas**: after any sequence of power cuts at any instant, a boot
  shows the content the last acknowledged update left or a newer one, a content shown at one
  boot is never taken back at a later one, an update that was never acknowledged is never
  shown after an older content was, and nothing is lost. An update is atomic whatever the
  number of chunks it touches: a boot never shows a mix of two contents;
- **the transactions of regular areas**: several writes, committed together or not at all,
  with the same calls;
- the stuck state of EWLF areas, its count of remaining cuts and the two-phase scan,
  unchanged.

It is also meant to stand in for a regular area. With one chunk per erase unit, an update of
the whole data uses the flash and the time it uses in a regular area, an update of a few
bytes one erase unit instead of all of them, and nothing is ever copied behind the
application's back: "Compared with a regular area" below.

It costs four erase units of its own next to the pool that holds the chunks, and, when
several chunks share an erase unit, a garbage collection that copies chunks. Nothing here is
implemented: the document is to be reviewed before any library code is written. The target and the tearing model are those of
[EWLF.md](EWLF.md): the STM32U5A5, 16-byte write units ("words"), 8 KB erase units ("units"),
T1 to T5 of the [flash page](https://lean-ftl.readthedocs.io/en/latest/flash.html).

## Changes since the first draft

The first draft lost to a regular area when updates are close to the whole data: 3.5 MB
against 3.1 MB for the example above, and an update that could have to wait for a round of
the pool. Three causes, each removed:

1. **The slots did not fill the erase units.** A slot was a marker word, the chunk and a
   checksum word, and 15 slots of 544 bytes fit in 8192: 6 % of every unit held no data, 12 %
   with 1 KB chunks. The area is now declared with the *number of chunks per erase unit*, and
   a slot is the unit divided by that number. The checksum word is gone (open point 2). With
   one chunk per unit the marker is gone too: the chunk is the unit.
2. **The head evacuated every unit in its way.** A unit that held nothing but current chunks
   was copied like the others, so data that never changes was copied once per round of the
   pool. The head now steps over the units that are full of current chunks, and evacuates
   only those that hold garbage. With one chunk per unit a unit is full or free, and nothing
   is ever copied.
3. **The pool kept two units in reserve.** With one chunk per unit it keeps none: the
   smallest pool is the data plus the largest update, the two slots of a regular area.

| 64 KB of data on the STM32U5A5 | First draft | Now, 1 chunk per unit | Now, 16 chunks per unit | Regular area |
|---|---|---|---|---|
| Flash an update of the whole data uses | 8.53 units | 8.00 units | 8.31 units | 8 pages |
| Area for 500 000 updates of the whole data | 3.5 MB | 3.16 MB | 3.28 MB | 3.12 MB |
| Area for 500 000 updates of one chunk | 192 KB | 496 KB | 184 KB | 3.12 MB |
| Copies of data that does not change | once per round of the pool | none | none for the units it fills | at every update |
| Copies an update may have to wait for | all the chunks of the data | none | all the chunks that do not fill their unit | n/a |

A chunked area can now also be the source of a write into any other area, which the first
draft left out. The log, the map, the commit, the scan, the transactions and the argument are
those of the first draft.

## The principle: the EWLF log, unchanged, and a map as its record

An EWLF area already gives what is hard: a small record that is replaced atomically, shown
only when a returned program proves it, never taken back. A chunked area uses exactly that,
and makes the record a **map**: for each chunk of the data, where its current copy is.

- The chunks live in a **pool** of erase units, in *slots*. An update writes the new copy of
  each chunk it touches into a free slot; the old copies stay where they are.
- The update then writes a new map, which names the new slots for the chunks it touched and
  the old slots for the others, and commits it as an EWLF record: a pair in the log.
- One entry of the log replaces the whole map. Before it, every boot shows the old content;
  after it, the new one. Nothing in between exists.

The commit, the scan, the repair, the log switch, the certificates and the stuck state are
those of EWLF.md, word for word. What is new is the pool: where a slot may be written, when a
pool unit may be erased, and how the slots of superseded chunks are recovered. The rules for
that are few, and they follow the same principle as the log: a reading proves nothing, a
returned program does.

## Layout

```
area = U pool units, then Nm map units, then 2 log units

area base                                                                      area end
+---------+---  ---+---------+---------+---  ---+----------+---------+---------+
|   P0    |        | P(U-1)  |   M0    |        | M(Nm-1)  |   L0    |   L1    |
|  slots  |        |  slots  |  maps   |        |   maps   |  pairs  |  pairs  |
+---------+---  ---+---------+---------+---  ---+----------+---------+---------+
|<---------- pool ---------->|<----- an EWLF area whose record is the map ---->|
```

The area is declared with `S`, the number of chunks per erase unit. A pool unit is `S` slots
of equal size, which fill it exactly, and the data, `data_size` bytes, is cut into `n` chunks
of `c` bytes, the last one possibly shorter. The map units and the log units are laid out as
the data units and the log units of an EWLF area.

**Slot.**

```
S = 1: the slot is the erase unit, and the chunk is the slot

+-----------------------------------------------------+
|                        chunk                        |
|                   one erase unit                    |
+-----------------------------------------------------+

S > 1: a start marker, then the chunk

+--------------+--------------------------------------+
| start marker |                chunk                 |
|  all zeros   |       erase unit / S - one word      |
|  one word    |                                      |
|  programmed  |                                      |
|  1st         |                 2nd                  |
+--------------+--------------------------------------+
```

The *start marker* is the one of an EWLF record: a slot is *started* once its marker does not
read erased, and nothing after an erased marker was ever programmed. It is what lets a
session that follows a power cut go on in a unit that is partly filled. With one chunk per
unit no unit is ever partly filled: a unit is written once, right after its erase, and needs
no marker.

On the STM32U5A5 a chunk is 8192 bytes for `S` = 1, 2032 for 4, 496 for 16, 240 for 32. A
slot has no flag, no checksum, and does not say which chunk it holds: only the map says.

**Map**, the data of the EWLF record:

```
+----------+----------+---  ---+------------+-----------+
| slot of  | slot of  |        |  slot of   |   head    |
| chunk 0  | chunk 1  |        | chunk n-1  |   unit    |
|  16 bit  |  16 bit  |        |   16 bit   |  16 bit   |
+----------+----------+---  ---+------------+-----------+
```

A slot is named by its number in the pool, unit x `S` + position; `0xFFFF` means that the
chunk was never written and reads erased. The *head unit* is the pool unit the update that
wrote this map was filling; `0xFFFF` before the first slot. The record around the map is an
EWLF record: start marker, the map, then the 32-bit version and the CRC-64 of both.

The map of 64 KB is 18 bytes with one chunk per unit and 268 bytes with sixteen. At least two
records must fit in a map unit, as in any EWLF area: about 2000 chunks at most on the
STM32U5A5.

## How many chunks per erase unit

**One.** The area behaves as a regular area that rewrites only the erase units an update
touches. An update of the whole data programs the data once, into as many units as it fills;
an update of a few bytes programs one unit. A unit holds a chunk or is free, so there is no
garbage to collect, no copy, no reserve, and the time of an update depends only on the number
of units it touches. This is the choice when updates are large, or whole erase units, or when
the worst update time matters.

**Several.** A small update programs one chunk and the map, `c` + 2 `n` bytes, which is
smallest for `c` near the square root of twice the data size: 362 bytes for 64 KB, hence
sixteen chunks per unit on the STM32U5A5. The price is one word per slot, two more units in
the pool, and collections: the slots of superseded chunks come back only when their unit is
erased, so the current chunks that share that unit are copied away first. This is the choice
when most updates are small.

## Read

A read looks up the slot of each chunk it covers in the current map record, and reads the
chunk there; a chunk whose entry is `0xFFFF` reads as erased bytes. No RAM copy of the map is
kept: the map is read from the flash, two bytes per chunk.

## Update

A plain write (`lftl_write`, `lftl_basic_write`, `lftl_erase_all`) covers a range of the data,
hence a run of consecutive chunks:

1. **room**: the update needs one new slot per chunk it touches. When the pool does not have
   them (below), garbage is collected first;
2. **the map record is opened**: its position and its start marker, as an EWLF update does
   (a rollover of the map units when the current one is full);
3. **the chunks, in increasing order**: for each chunk touched, the next slot, its start
   marker when it has one, then the new chunk: the written bytes from the caller, the others
   copied from the current copy, or left erased when there is none. The map is programmed
   along the way, word by word: the entries of the chunks not touched are copied from the
   current map, those of the chunks touched are the new slots. The RAM this takes does not
   grow with the data: one word of map at a time;
4. **the end of the map record**: the head unit, then the meta field of the record;
5. **the commit**: the pair that names the record, by the log of EWLF.md: a regular pair, or
   a log switch;
6. the update is acknowledged once the confirm of its pair returned.

`lftl_erase_all` is an update whose map names no slot.

The order makes one chain: every program of every chunk returned before the meta field of the
map record was programmed, which returned before the entry was started. So a map that an
entry names, names only whole chunks.

With several chunks per unit a step can come up short: a slot whose marker the flash refuses
(a marker a power cut left weak, which reads erased) is stepped over, and the head unit then
holds one slot less than step 1 counted. When that leaves the update without room, it
abandons the map record it opened, which is then a dead record like one a power cut left,
collects garbage and starts again at step 1. Each weak marker causes this once.

## The pool

**Free, full, garbage.** A pool unit is *free* when it holds no chunk of the current map,
none of the open transaction and none of the update in progress. Whatever else it holds is
garbage: the chunks of older maps, and what a power cut or an aborted transaction left. A
unit is *full* when each of its `S` slots holds a chunk of the current map. Between the two
it *holds garbage*: some of its slots hold current chunks, and the others come back only when
the unit is erased. With one chunk per unit a unit is free or full.

**The head.** Slots are taken in order, in one pool unit at a time, the *head unit*. After a
reset the head unit is the one the current map names. With several chunks per unit the next
slot is the one above the last started slot of that unit, found by reading its `S` start
markers, and a slot whose marker is refused counts as started. With one chunk per unit the
head unit a reset finds is full.

**Round the pool.** When the head unit has no slot left, the head moves to the first free
unit after it, in increasing order, wrapping from the last unit to the first; the head unit
itself comes last, when it is free. From there it goes on from free unit to free unit, in
that order. On its way it steps over the full units, which have nothing to give and are left
alone, and over the units that hold a chunk of the open transaction or of the update in
progress. It does not go past a unit that holds garbage: that unit, the *tail*, is evacuated
first, and the head then enters it in its turn.

A unit that holds garbage *before* the first free unit is not the tail: it was full when the
head came to it, lost a chunk since, and its turn comes at the end of the round. The tail is
looked for from the first free unit on, so that there is always a free unit to evacuate it
into.

The unit entered is **erased at that instant, whatever it reads**, and its first slot is
programmed only after the erase returned: a free unit is never trusted because it reads
erased (T4), it may hold the remains of an erase that a power cut interrupted. Nothing is
erased anywhere else, and nothing is erased while a repair of the scan is pending. The erase
is guarded as the rollover of an EWLF area is: if anything started in the log could still
outrank the decision, the decision is buried first.

**The reserve**, with several chunks per unit. An update may enter a unit only if another
unit it may enter is left after it; a collection may enter the last one, because its commit
frees the tail. So there is always room to evacuate the tail. With one chunk per unit there
is no tail and no reserve.

**Garbage collection**, with several chunks per unit. A collection evacuates the tail:

1. the chunks of the current map that lie in that unit are found by one pass over the map;
2. each is copied to a new slot at the head;
3. a map that names the copies is committed, as an update commits.

From that commit on the tail is free. It is erased later, when the head enters it. Until the
commit is acknowledged the old map still names the chunks of that unit, and they are intact:
nothing was erased. Each collection gains at least one slot.

A collection changes no content: the map it commits describes the same data. It is done:

- **inside an update**, in the same commit: when the update lacks room and the copies fit
  with its own chunks, both go under one map;
- **alone**, with a commit of its own, as many times as needed: when that is not enough, and
  when a write of a transaction finds no room.

Whether a unit is free, full or holds garbage is read from the map when needed, one pass per
unit looked at, 2 `n` bytes each. Nothing about the pool is kept in RAM beyond the head and
its next slot.

**Wear.** The head never goes past garbage, so every unit is erased once per round of the
head, except the full ones, which are neither erased nor copied. A round therefore gives the
application all the slots of the pool less those of the chunks it did not rewrite in that
round: `U` x `S` - `n` at least. A unit whose chunks never change costs nothing once they
fill it.

**What a power cut burns.** A session that is cut, or a transaction that is aborted, leaves
slots that are started and that no map names. Those in units the head entered since the last
commit come back at once: the next session starts from the head unit of the committed map,
finds the same free units and erases them again when it enters them. Those in the committed
head unit, with several chunks per unit, stay started until that unit is evacuated: at most
one unit's worth. With one chunk per unit a power cut burns nothing that lasts.

## Transactions

The calls are those of regular areas, and so are the rules: one transaction at a time per
area, a write unit is written once per transaction, a read returns the committed data unless
it asks for the newer one.

- `lftl_transaction_start` takes a caller-owned *tracker*: one bit per write unit of the data,
  as for a regular area, then one 16-bit slot number per chunk. It erases nothing, where a
  regular area erases a whole slot;
- `lftl_transaction_write` takes a new slot for each chunk it touches for the first time,
  and programs the write units it covers straight into that slot, marking them in the
  tracker. A second write to a marked unit is `LFTL_ERROR_TRANSACTION_OVERWRITE`. When it
  needs a slot and the pool has no room, a collection runs first, with its own commit: it
  changes no content, so it may happen inside a transaction;
- `lftl_transaction_commit` completes each chunk the transaction touched (the write units not
  marked are copied from the current copy), then writes the map and commits it, as an update
  does from its step 2;
- `lftl_transaction_abort` forgets the tracker. The slots it took are garbage;
- `lftl_read_newer` and `lftl_transaction_read` return the new value of the marked write
  units and the committed one elsewhere.

A transaction holds the old and the new copy of every chunk it touches until its commit, so
its size is bounded by the room of the pool: see the sizing rule.

## Scan

The scan is the scan of the EWLF part, unchanged: both log units are read once, the decision
`D` is the newest valid entry of the live unit, and the record it names, the map, is checked
against its checksum. The pool is not read. The time of the scan does not depend on the size
of the data, nor on the size of the pool.

- When the top of the log is solid, the map is current and the area is readable and writable.
- Otherwise a repair is pending, done by `lftl_ewlf_complete_scan`; until then reads and
  writes raise `LFTL_ERROR_EWLF_PENDING`, `lftl_ewlf_status` reports the remaining cuts, and
  `lftl_ewlf_read_unproven` reads the data through the map `D` names. The chunks of that map
  are intact, whichever of the two candidates it is: no unit is erased while a repair is
  pending.

The state of the pool is derived at the first write after a reset: the head unit from the
map, its next slot from its start markers, the free units from the map.

**After an error.** Any error raised on a chunked area, a refused program that is not a start
marker, a failed callback, the stuck state, a lack of room, aborts its transaction and makes
the next call scan the area again, as after a reset. What the area knows in RAM is thus never
ahead of, nor behind, what a boot would find.

## Why there is no rollback, no loss, and no mixed content

1. *The map is an EWLF record.* What is shown is a map that a proven entry names; the order
   of maps is the generation and the position of their entries; a map once shown is never
   outranked. This is the argument of EWLF.md, not repeated here, and it does not depend on
   what the record holds.
2. *A map names only whole chunks.* Each chunk it names was programmed, to its last word,
   before the meta field of the map record, in the same session. Each was programmed into a
   unit whose erase had returned: either in that session, before its first slot, or in the
   session that committed a map with that unit as head unit, before that commit.
3. *No erase destroys a chunk that a boot can show.* A pool unit is erased only when the head
   enters it, in a session where the area is ready: its scan found the top solid or its
   repair returned, so the current map is final and no older map can be elected again. The
   unit is free: it holds no chunk of that map. The only other map that can still become
   current is the one being written, and the unit holds none of its chunks either. After a
   cut, the two candidates are the map that was current and the one that was being committed;
   nothing is erased until a boot has made one of them final.
4. *The content is never mixed.* A single entry replaces the map. A transaction or an update
   that touched ten chunks is shown with its ten new chunks or with none.
5. *A collection is invisible.* It commits a map that names copies of the same chunks.

The order in which the head enters the free units, and the unit a collection evacuates, play
no part in this argument: they decide the wear and the number of copies, not what a boot
shows.

What the next boot makes of a cut, by where it lands:

| The cut lands in | The next boot sees | Outcome |
|---|---|---|
| a slot: its marker or its chunk | a slot that no map names | the previous content; the slot is stepped over, or goes with the next erase of its unit |
| the erase of a pool unit | the remains of chunks that no map names | the previous content; the unit is free and is erased again when the head enters it |
| the map record | a started record that no entry names | the previous content |
| the pair of the commit | as in EWLF.md: the old map or the new one, repaired before it is shown | the whole previous content or the whole new one, never taken back |
| a collection: its copies, its map, its pair | the same, with a map that names the copies or not | the same content either way |

## Residuals

- **The stuck state**, inherited from the log, with the same count and the same report
  (`remaining_cuts`). Nothing in the pool adds a way to it.
- **No room.** An update or a transaction that needs more new slots than the pool can free
  raises `LFTL_ERROR_CHUNKED_NO_SPACE` and changes nothing. With the pool sized by the rule
  below it does not happen, whatever the power cuts (simulation below); it is the error of a
  transaction larger than the area was sized for.
- **Wear under power cuts.** A cut update costs the slots it started, like the update itself;
  the erases stay those of the application's writes, and a boot alone writes nothing.
- **Update time.** With one chunk per unit: the units the update touches and its commit,
  nothing else, in every state of the pool. With several: an update that finds the pool
  short evacuates units until it has room, in the order of the round. A small update waits
  for one unit, a large one in a pool that small updates have fragmented may wait for the
  whole round: the simulation gives the figures.
- **Corruption of a chunk by the flash** is not detected: the scan checks the map, not the
  chunks, and a slot carries no checksum (open point 2).
- As for the other areas: `lftl_format` is not protected against a power cut, and superseded
  chunks stay physically readable until their unit is erased.

The design adds no assumption about the flash to those of EWLF.md. The pool does not use the
one that a cut erase of erased cells leaves them erased: it erases every unit it enters, and
the model gives the same result with and without it (below).

## Compared with a regular area

On the STM32U5A5, for data that fills 8 erase units less 48 bytes, the best case of a regular
area: its three meta-data items fit in the last unit. With 64 KB exactly a regular area needs
a ninth unit per slot, a chunked area does not.

| | Regular area | Chunked, 1 chunk per unit | Chunked, 16 chunks per unit |
|---|---|---|---|
| Update of the whole data: flash | 8 units | 8 units, 64 bytes of map, 48 of log | 8.31 units, 304 bytes of map, 48 of log |
| Update of the whole data: time | 556 ms | 538 ms | 561 ms |
| Update of a few bytes | the same | 1 unit, 68 ms | 1 slot, 7.3 ms |
| Longest update | the same | the same as any other of its size | 63 ms more per unit evacuated before it |
| Smallest area | 128 KB | 160 KB | 184 KB |
| Area for 500 000 updates of the whole data | 3.12 MB | 3.16 MB | 3.28 MB |
| Area for 500 000 updates of one chunk | 3.12 MB | 496 KB | 184 KB |
| Start of a transaction | one slot erased, 12.5 ms | nothing | nothing |
| Transaction tracker | 512 bytes | 528 bytes | 778 bytes |
| First access after a reset | the version of every slot, the checksum of the data of half of them | 16 KB of log, the checksum of one map | the same |
| After a power cut | the old or the new version; a rollback by one version in the cases of the design page | the old or the new content, never taken back | the same |

The times are estimates from the figures of the next section. The 556 ms of a regular area
include the checksum of the data it computes at every update, about 20 ms if its bitwise
CRC-32 runs at 0.3 us per byte; a chunked area checks only its map. The first access of a
regular area of 50 slots checksums 1.6 MB of data on average, about half a second at that
rate.

What a regular area keeps:

- **no fixed cost**: a chunked area has two map units and two log units on top of its pool,
  32 KB on the STM32U5A5 whatever the data. Below a few erase units of data, a regular or an
  EWLF area is smaller. Open point 3 would bring the fixed cost down to the two log units;
- **any flash**: a chunked area needs what an EWLF area needs, erased bytes that read `0xFF`
  and a write callback that reports a refused program (R6 and R9 of the flash page);
- **no repair call and no stuck state**: after a power cut a chunked area may have a repair
  to do before it is read, as an EWLF area;
- **a check of the data**: a regular area checks the checksum of its current slot at the
  first access after a reset, and an EWLF area that of its current record. A chunked area
  checks its map only (open point 2);
- the log allows about 3 million commits (EWLF.md, "Key figures"), where a regular area
  endures as many updates as it is given slots.

## Sizing and costs on the STM32U5A5

Estimates, from 128 us per word programmed, 1.56 ms per erase, about 0.65 us per byte of
CRC-64 and 0.3 us per byte of CRC-32 at 160 MHz. Nothing here is measured. The tables are
printed by `test/ewlf-model/chunked_sim.py --tables`.

**Sizing rule.** With `kmax` the number of chunks of the largest update or transaction, and
`W` the number of chunk writes over the life of the area:

```
pool units >= n + kmax                    room, S = 1: the data and the largest update
pool units >= ceil((n + kmax) / S) + 2    room, S > 1: the same, the reserve, and one unit of
                                          slots a power cut may have burnt
pool slots >= W / endurance + n           wear: a round of the head erases each unit once at
                                          most and gives all the slots less those of the
                                          chunks it did not rewrite. An area whose every
                                          update rewrites the whole data needs W / endurance
map units  >= commits / endurance / records per map unit, 2 at least
```

The log allows about 3 million commits, as for any EWLF area.

**Layout and time**, for data of 2, 8 and 16 erase units less 48 bytes:

| Data | Chunks per unit | Chunk | `n` | Map record | Update of one chunk | One unit evacuated, at most | Update of the whole data | Regular area, any update | Tracker |
|---|---|---|---|---|---|---|---|---|---|
| 16 KB | 1 | 8192 B | 2 | 48 B | 67.9 ms | never | 135 ms | 139 ms | 132 B |
| 16 KB | 4 | 2032 B | 9 | 64 B | 17.7 ms | + 50 ms | 152 ms | 139 ms | 146 B |
| 16 KB | 16 | 496 B | 33 | 112 B | 5.6 ms | + 63 ms | 140 ms | 139 ms | 194 B |
| 16 KB | 32 | 240 B | 69 | 176 B | 4.0 ms | + 65 ms | 147 ms | 139 ms | 266 B |
| 64 KB | 1 | 8192 B | 8 | 64 B | 68.0 ms | never | 538 ms | 556 ms | 528 B |
| 64 KB | 4 | 2032 B | 33 | 112 B | 18.1 ms | + 50 ms | 555 ms | 556 ms | 578 B |
| 64 KB | 16 | 496 B | 133 | 304 B | 7.3 ms | + 63 ms | 561 ms | 556 ms | 778 B |
| 64 KB | 32 | 240 B | 273 | 592 B | 7.7 ms | + 65 ms | 578 ms | 556 ms | 1058 B |
| 128 KB | 1 | 8192 B | 16 | 80 B | 68.2 ms | never | 1075 ms | 1113 ms | 1056 B |
| 128 KB | 4 | 2032 B | 65 | 176 B | 18.7 ms | + 50 ms | 1092 ms | 1113 ms | 1154 B |
| 128 KB | 16 | 496 B | 265 | 576 B | 9.7 ms | + 63 ms | 1117 ms | 1113 ms | 1554 B |
| 128 KB | 32 | 240 B | 546 | 1136 B | 12.5 ms | + 65 ms | 1155 ms | 1113 ms | 2116 B |

An update counts its chunks, its map record, its pair, the checksum of the map and its share
of the erases of the pool, map and log units.

**Flash an update of the whole data uses**, in erase units, the map and the pair aside:

| Data | Regular area | 1 chunk per unit | 4 | 16 | 32 |
|---|---|---|---|---|---|
| 8 KB less 48 B | 1 | 1.00 | 1.25 | 1.06 | 1.06 |
| 8 KB | 2 | 1.00 | 1.25 | 1.06 | 1.09 |
| 16 KB less 48 B | 2 | 2.00 | 2.25 | 2.06 | 2.16 |
| 16 KB | 3 | 2.00 | 2.25 | 2.12 | 2.16 |
| 64 KB less 48 B | 8 | 8.00 | 8.25 | 8.31 | 8.53 |
| 64 KB | 9 | 8.00 | 8.25 | 8.31 | 8.56 |
| 128 KB less 48 B | 16 | 16.00 | 16.25 | 16.56 | 17.06 |
| 128 KB | 17 | 16.00 | 16.25 | 16.56 | 17.09 |

With one chunk per unit it is never more than in a regular area, and one unit less whenever
the data fills its erase units. With several it is the marker of each slot more, 3 % at
sixteen chunks per unit, and the rounding of the data to whole chunks.

**Area for 500 000 writes**, 10 000 erase cycles, a transaction of the whole data allowed
(`kmax` = `n`), by the size of a write, at any offset:

| Data | Chunks per unit | 1 chunk | 2 KB | 8 KB | the whole data | Regular area |
|---|---|---|---|---|---|---|
| 16 KB | 1 | 448 KB | 552 KB | 848 KB | 832 KB | 800 KB |
| 16 KB | 4 | 152 KB | 256 KB | 560 KB | 936 KB | 800 KB |
| 16 KB | 16 | 88 KB | 176 KB | 488 KB | 864 KB | 800 KB |
| 16 KB | 32 | 88 KB | 168 KB | 488 KB | 896 KB | 800 KB |
| 64 KB | 1 | 496 KB | 600 KB | 896 KB | 3.16 MB | 3.12 MB |
| 64 KB | 4 | 200 KB | 304 KB | 608 KB | 3.26 MB | 3.12 MB |
| 64 KB | 16 | 184 KB | 232 KB | 536 KB | 3.28 MB | 3.12 MB |
| 64 KB | 32 | 208 KB | 240 KB | 560 KB | 3.38 MB | 3.12 MB |
| 128 KB | 1 | 560 KB | 664 KB | 960 KB | 6.28 MB | 6.25 MB |
| 128 KB | 4 | 312 KB | 368 KB | 672 KB | 6.38 MB | 6.25 MB |
| 128 KB | 16 | 336 KB | 336 KB | 624 KB | 6.52 MB | 6.25 MB |
| 128 KB | 32 | 376 KB | 376 KB | 656 KB | 6.75 MB | 6.25 MB |

With one chunk per unit and updates of the whole data, the area is the regular area plus the
four fixed units. The 8 KB column of that line counts two units per write, a write at any
offset; a write of exactly one erase unit is the "1 chunk" column.

**Smallest area**, a transaction of the whole data allowed:

| Data | Chunks per unit | Pool | Area | Regular area, 2 slots |
|---|---|---|---|---|
| 8 KB | 1 | 2 units | 48 KB | 16 KB |
| 8 KB | 16 | 5 units | 72 KB | 16 KB |
| 16 KB | 1 | 4 units | 64 KB | 32 KB |
| 16 KB | 16 | 7 units | 88 KB | 32 KB |
| 64 KB | 1 | 16 units | 160 KB | 128 KB |
| 64 KB | 16 | 19 units | 184 KB | 128 KB |
| 128 KB | 1 | 32 units | 288 KB | 256 KB |
| 128 KB | 16 | 36 units | 320 KB | 256 KB |

**Boot and RAM.** The scan is the scan of an EWLF area, a fraction of a millisecond, and
checks one map record. Nothing in RAM grows with the data outside a transaction.

## Simulation of the pool

`test/ewlf-model/chunked_sim.py` plays the pool at real sizes, with the rules above and
without the tearing model, for sequences of writes, transactions, aborts and power cuts:
uniform, skewed (90 % of the writes on 10 % of the chunks), always the same chunk, mixed
sizes, the largest update every time, a pool fragmented by single chunks then hit by the
largest update, every chunk for a while then one chunk only, and an attack on the room (every
other operation is the largest transaction, cut right before its commit). `--suite` is the
room check, `--compare` the rest, with the first draft in the same pool.

- *Room.* 400 runs of 30 000 operations at the smallest pool the room rule allows: 1, 4, 16
  and 32 chunks per unit, a largest update of 1, 8 or all chunks, the eight sequences, with
  and without 5 % of cut and 5 % of aborted operations, collection inside updates or not. No
  operation lacked room. With one unit less, operations lack room in 96 of the 112 runs with
  one chunk per unit and in 41 of the 288 others: the rule has no slack to give back.
- *One chunk per unit.* No copy in any run: a chunk written costs one unit, 1.09 with the
  cut and aborted operations.
- *Several chunks per unit.* 64 KB in 496-byte chunks, a pool of 20 units, 100 000
  operations, updates of 1 to 8 chunks; without cuts and, in brackets, with 5 % of cut and
  5 % of aborted operations:

  | Sequence | Copies per chunk written | Most copies in one update | First draft, copies per chunk written | First draft, most copies in one update |
  |---|---|---|---|---|
  | uniform | 0.17 (0.21) | 9 (10) | 0.20 (0.24) | 10 (10) |
  | skewed | 0.55 (0.61) | 26 (24) | 0.61 (0.66) | 22 (21) |
  | always the same chunk | 0.12 (0.13) | 15 (15) | 0.84 (0.91) | 21 (16) |
  | always the same 8 chunks | 0.07 (0.08) | 8 (8) | 0.70 (0.75) | 117 (118) |
  | mixed sizes | 0.17 (0.21) | 19 (19) | 0.18 (0.23) | 18 (16) |
  | every chunk, then one only | 0.65 (0.63) | 15 (14) | 0.72 (0.78) | 15 (15) |

  The gain is where the data that does not change fills its units. Where it is scattered
  among chunks that change, the last line, it is copied about as much as before: once per
  round.
- *Large updates.* When updates go up to the whole data in that same pool, one that follows
  many small ones waits for most of a round: up to 103 copies before its own 133 chunks, 133
  with the cut operations. More pool units make it rarer; one chunk per unit has no such
  case.
- *Wear.* Per erase of its most erased unit, that pool took at least 186 chunk writes in
  every sequence without cuts, where the sizing rule counts on 187, the slots of the pool
  less the chunks. The first draft took 174 in the sequence that always writes the same
  chunk. With 5 % of cut and 5 % of aborted operations the figure is 167 at least: a cut
  operation costs the slots it wrote. Under the attack each cut costs what the transaction it
  interrupts would have cost, as for the other areas.

## Model check

`test/ewlf-model/chunked_model.py` explores the design exhaustively within bounds, with the
flash model of `ewlf_model.py` (EWLF.md, "Model check"): every reading of every weak word,
every instant of a power cut, every choice of the application. The application picks, at each
session, operations among plain writes of one chunk or of several, a transaction that
commits, and a transaction that aborts. What a boot shows must be the content some operation
left when it committed; that content must not be older than one shown or acknowledged before,
nor one that was abandoned after an older one was shown. A chunk that does not read as written
is *corrupt*, a content no operation left is *torn*. The content read before the repair of the
scan is checked too.

Three modules, one pool layer:

- `proto_c2.py`: the pool rules of this document over the log of `proto_v4.py`, whose
  functions are imported unchanged. Geometry `chunks, pool units, slots per unit`, then the
  map units, records per map unit, regular pairs, reserved pairs and certificates of
  `proto_v4.py`, 2 each by default;
- `proto_c2a.py`: the same pool rules over the *contract* of the log instead of the log: a
  commit that is cut leaves the old map, or either map at each later boot until a repair
  returns, or the new one. It has far fewer states, and explores the pool further; it says
  nothing about the log, which EWLF.md covers;
- `proto_c2p.py`: the same pool rules with the map in the pair, over `proto_v4p.py`, for open
  point 3.

`proto_c1.py` and `proto_c1a.py` are the first draft, kept for comparison; its results are
no longer in this document.

A chunk is one word in the model, and so is the map: a cut inside a chunk is that word weak.
"Warm" runs start from a pool in use (five writes without cuts first, twelve for pool E), so
that the first operation explored meets a pool that has gone round. On the real log the warm
runs ignore the stuck state: the model's log has two certificates, and EWLF.md counts the cuts
that lead there.

```
cd test/ewlf-model
python3 chunked_model.py --protocol c2a --geometry 2,4,1 --warmup w01,w0,w0,w0,w0 --cuts 3 --boots 6 --strict
```

Every run is `--strict`. The pools are the smallest the room rule allows for their largest
operation:

- *A*: 2 chunks, 4 units of 1 slot (`2,4,1`): one chunk per unit;
- *B*: 2 chunks, 4 units of 2 slots (`2,4,2`);
- *C*: 3 chunks, 5 units of 2 slots (`3,5,2`), operations `w0,w12,t012,a1`;
- *D*: 3 chunks, 6 units of 1 slot (`3,6,1`), the same operations;
- *E*: 3 chunks, 3 units of 4 slots (`3,3,4`), operations of one chunk `w0,w1,w2,t1,a0`,
  after twelve writes.

| Module | Pool | Start | Cuts / boots | Options | Result |
|---|---|---|---|---|---|
| `c2a` | A | format | 2 / 5 | | no violation (107 k states, 304 k sessions) |
| `c2a` | B | format | 2 / 5 | | no violation (169 k states, 457 k sessions) |
| `c2a` | A | warm | 2 / 5 | | no violation (110 k states, 313 k sessions) |
| `c2a` | B | warm | 2 / 5 | | no violation (253 k states, 686 k sessions) |
| `c2a` | C | warm | 2 / 5 | | no violation (381 k states, 943 k sessions) |
| `c2a` | E | warm | 2 / 5 | | no violation (346 k states, 824 k sessions) |
| `c2a` | A | warm | 2 / 5 | `--weak-erased` | no violation, the same 313 k sessions: the pool never reads a unit whose erase was cut |
| `c2a` | B | warm | 2 / 5 | `--weak-erased` | no violation, the same 686 k sessions |
| `c2a` | B | warm | 2 / 5 | collection inside the update that lacks room (`C2_COMBINE=need`) | no violation (264 k states, 700 k sessions) |
| `c2a` | C | warm | 2 / 5 | the same | no violation (355 k states, 836 k sessions) |
| `c2a` | B | warm | 2 / 5 | collection inside every commit that can hold it (`C2_COMBINE=1`) | no violation (243 k states, 660 k sessions) |
| `c2a` | C | warm | 2 / 5 | the same | no violation (211 k states, 485 k sessions) |
| `c2` | A | format | 1 / 4 | | no violation (12 k states, 28 k sessions) |
| `c2` | B | format | 1 / 4 | | no violation (13 k states, 30 k sessions) |
| `c2a` | A | warm | 3 / 6 | | no violation (2.1 M states, 7.3 M sessions, 3 min) |
| `c2a` | D | warm | 3 / 6 | | no violation (3.0 M states, 8.6 M sessions) |
| `c2a` | B | warm | 3 / 6 | | no violation (7.2 M states, 24.3 M sessions) |
| `c2a` | B | warm | 3 / 6 | `C2_COMBINE=need` | no violation (7.3 M states, 23.9 M sessions) |
| `c2a` | C | warm | 3 / 6 | `C2_COMBINE=need` | no violation (11.0 M states, 32.8 M sessions, 17 min) |
| `c2a` | C | warm | 3 / 6 | `C2_COMBINE=1` | no violation (5.7 M states, 16.5 M sessions) |
| `c2a` | C | warm | 3 / 6 | | **no verdict**: the run reached its memory cap of 1.5 GB |
| `c2a` | E | warm | 3 / 6 | | **no verdict**: the same |
| `c2a` | A | warm, two operations per session | 1 / 4 | | no violation (1.3 M states, 3.7 M sessions) |
| `c2a` | A | the same | 2 / 3 | | no violation (235 k states, 975 k sessions) |
| `c2a` | B | the same | 2 / 3 | | no violation (469 k states, 1.7 M sessions) |
| `c2a` | C | the same | 2 / 3 | | no violation (620 k states, 1.9 M sessions) |
| `c2a` | A | the same | 2 / 4 | | no violation (6.2 M states, 24.0 M sessions, 13 min) |
| `c2` | A | warm | 2 / 4 | | no violation (142 k states, 4.1 M sessions) |
| `c2` | B | warm | 2 / 4 | | no violation (286 k states, 7.4 M sessions) |
| `c2` | C | warm | 2 / 4 | | no violation (382 k states, 9.4 M sessions) |
| `c2` | B | warm | 2 / 4 | `C2_COMBINE=need` | no violation (266 k states, 6.4 M sessions) |
| `c2` | C | warm | 2 / 4 | `C2_COMBINE=need` | no violation (313 k states, 6.8 M sessions) |
| `c2` | A | warm | 2 / 4 | one record per map unit and one regular pair per log unit (`2,4,1,2,1,1,2,2`): a rollover of the map units and a log switch at every commit | no violation (114 k states, 972 k sessions) |
| `c2` | B | warm | 2 / 4 | the same (`2,4,2,2,1,1,2,2`) | no violation (202 k states, 1.9 M sessions) |
| `c2` | A | warm, two operations per session | 1 / 3 | | no violation (169 k states, 1.8 M sessions) |
| `c2` | A | warm | 2 / 5 | | no violation (825 k states, 22.9 M sessions, 31 min) |
| `c2` | B | warm | 2 / 5 | | no violation (1.5 M states, 40.3 M sessions, 59 min) |
| `c2p` | A | format | 1 / 4 | | no violation (8.8 k states, 25 k sessions) |
| `c2p` | A | warm | 2 / 4 | | no violation (94 k states, 29.9 M sessions, 38 min) |

Without an option a collection always has a commit of its own; `C2_COMBINE=need` is the
policy of this document, `C2_COMBINE=1` puts a collection inside every commit that can hold
it.

Two runs ended without a verdict, at their memory cap: pool C with every collection in a
commit of its own, and pool E, both with 3 cuts and 6 boots. Both pools pass with 2 cuts and
5 boots, and pool C passes with 3 cuts and 6 boots under the policy of this document.

The runs on the contract of the log are all in the table. Still running when this was
written, and added here as they end: on the real log, pool A with 3 cuts and 5 boots; the
map in the pair (`c2p`), pool B with 2 cuts and 4 boots, a log switch at every commit on
pool A, then pool A with 2 cuts and 5 boots.

Negative controls (`C2_KNOCKOUT=` in the environment), warm, stuck ignored, on the contract
of the log with 2 cuts and 5 boots and on the real log with 2 cuts and 4 boots. Each rule,
removed alone, gives the same violation on both:

| Rule removed | Control | Pool | Result |
|---|---|---|---|
| a unit is entered only when it holds no chunk of the current map | `live`: those chunks are not looked at | A, B | **readback**: the update erases a current chunk, and the content read after it is wrong |
| nor of the open transaction | `txn`: those chunks are not looked at | 2 chunks, 3 units of 1 slot, one unit under the rule, no room ignored | **readback**: the transaction erases its own first chunk |
| a unit is erased when the head enters it, whatever it reads | `trust`: no erase when the unit reads erased | A, B | **corrupt**: a chunk written into cells an interrupted erase left reads garbage later (T4) |
| a slot whose marker is refused is stepped over | `nostep`: the slot is used all the same | B | **corrupt**: the chunk is programmed over a weak word |
| a unit is erased only once the map that frees it is acknowledged | `early`: the collection erases the tail before its commit | B | **corrupt**: the commit is cut, the old map is elected, its chunks are gone |
| the reserve | `noreserve`: an update may take the last unit it can enter | E | **no room**: the twelve writes leave a chunk in each unit, and nothing can be evacuated |

With the map in the pair (`c2p`, 2 cuts and 4 boots), `trust` on pool A and `early` on pool
B give the same violations.

One rule of this document has no control: the bury before an erase in the pool when the top
of the log is not solid. As for the rollover of an EWLF area, a session that scanned never
meets that case; the rule is a defence. The order of the round has none either: it decides
the wear, which the simulation measures, not what a boot shows.

The checker proves the absence of counterexamples within its bounds and under its model. It
does not cover: more than three chunks, the words inside a chunk, partial writes of a chunk, and
the room of the pool at real sizes, which the simulation covers without the tearing model.

## Limits

- the fixed cost: two map units and two log units, and the limits that come with the log: the
  stuck state, the repair to ask for after a reset, about 3 million commits;
- the flash must read `0xFF` when erased and report a refused program, as for an EWLF area;
- every update rewrites the whole map: the chunk count is bounded by what an update can
  afford, and by half a map unit;
- the pool must be sized for the largest transaction;
- with several chunks per unit, the time of an update depends on the state of the pool.

## API

```c
LFTL_AREA_CHUNKED(name, area_content, chunks_per_page, pool_pages, map_pages)

lftl_ctx_t area = { ..., .data_size = sizeof(nvm.name_data),
                    .chunks_per_page = ..., .map_pages = ... };
lftl_register_area_chunked(&area);
```

"Page" is the library's word for the erase unit in its macros. `LFTL_CHUNK_SIZE(chunks_per_page)`
gives the chunk size, for an application that lays its data out along the chunks.

Then the calls of the other areas: `lftl_format`, `lftl_read`, `lftl_write`,
`lftl_write_any`, `lftl_basic_write`, `lftl_erase_all`; the transaction calls of regular
areas, with `LFTL_TRANSACTION_TRACKER_SIZE` giving the larger tracker; and the three calls of
the two-phase scan, `lftl_ewlf_status`, `lftl_ewlf_complete_scan`, `lftl_ewlf_read_unproven`.
A chunked area is also accepted as the source of a write into any area, itself included: the
source is then read chunk by chunk, and a write unit of the destination that straddles two
chunks of the source is assembled in RAM. One new error, `LFTL_ERROR_CHUNKED_NO_SPACE`. The
number of chunks per page and the numbers of pool and map pages are part of the on-flash
format, like the data size: nothing on the flash records them. The format is new in v0.5.0;
regular and EWLF areas are unchanged.

## Considered and not kept

- **No start marker with several chunks per unit.** It would save one word per slot, 3 % at
  sixteen chunks per unit. A session can then no longer tell which slots of a partly filled
  unit a power cut touched: it must leave that unit after every cut, so a cut costs the rest
  of a unit instead of the slots it started, and a boot needs a way to know that a previous
  session wrote in the pool since the last commit. The marker is what makes a small update
  cheap after any number of power cuts, and with one chunk per unit there is none to save.
- **The map records in the pool**, to save the two map units. For small updates a map record
  is about as large as a chunk, for an update of the whole data it is one record for all the
  chunks: no fixed share of a pool unit suits both, and two rings of units, each wearing at
  its own rate, do.
- **Free units taken wherever they are, and the unit with the fewest chunks evacuated.** It
  copies less: 0.13 copies per chunk written against 0.17 in the uniform sequence of the
  simulation above, 4 at most in one update against 9. But a unit that holds a few chunks
  that no longer change is then never chosen and never erased, and the erases fall on the
  units that are left. In the sequence that writes every chunk for a while and then one
  only, the most erased unit takes an erase per 51 chunk writes, where the sizing rule counts
  on 187 and the round gives 194. The order of the round is what makes the sizing rule hold.

## Open points

1. The name of the type. "Chunked" is a working name; "page" already means erase unit here.
2. A checksum of the chunks. As this document stands, the data is covered by no checksum:
   the CRC-64 of the map record covers the map, and the argument needs nothing else, since a
   map names a chunk only after the program of that chunk returned. What is lost is the
   detection of a chunk that the flash corrupts after its write. A regular area and an EWLF
   area check their data once, at the first access after a reset: a regular area skips the
   slot and falls back to the previous version, an EWLF area raises
   `LFTL_ERROR_NO_VALID_VERSION`. Neither checks at later reads.

   A CRC-32 per chunk can be kept in the last word of the slot or in the map. For the 64 KB
   of the tables above, with the same estimates:

   | | No checksum | In the last word of the slot | In the map |
   |---|---|---|---|
   | 1 chunk per unit: update of the whole data | 8 units, 538 ms | 9 units, 627 ms | 8 units, 558 ms |
   | 1 chunk per unit: update of one chunk | 68.0 ms | 70.5 ms | 70.8 ms |
   | 16 chunks per unit: update of the whole data | 8.31 units, 561 ms | 8.56 units, 597 ms | 8.31 units, 585 ms |
   | 16 chunks per unit: update of one chunk | 7.3 ms | 7.5 ms | 12.1 ms |
   | 16 chunks per unit: area for 500 000 updates of one chunk | 184 KB | 192 KB | 216 KB |

   In the slot it costs one word per slot: with one chunk per unit a chunk is 8176 bytes, and
   data that fills the pages of a regular area exactly needs one more unit (with 80 bytes
   less it does not). In the map it costs 4 bytes per chunk in every map record: 32 bytes
   with one chunk per unit, 532 bytes per update with sixteen, and six map units instead of
   two for 500 000 commits. A 16-bit CRC in the map halves that: 9.8 ms and 200 KB. Either
   way, checking the whole data once takes about 20 ms per 64 KB, 315 ms per megabyte, and a
   mismatch can only be an error: a chunked area has no older version to fall back to.

   A third form needs neither: **one CRC-64 of the whole data, kept as 8 more bytes of the
   map**, which the CRC-64 of the map record covers in turn. It is not computed again at
   every update: a CRC is linear, so replacing chunk `i` changes the checksum of the whole
   data by the CRC of the old content xor the CRC of the new one, multiplied by a power of
   `x` that depends only on the position of the chunk. An update thus reads the old content
   of every chunk it replaces, and checksums the old and the new; an update of the whole
   data computes the checksum afresh. A collection does not touch it.

   | | No checksum | One CRC-64 of the data in the map |
   |---|---|---|
   | 1 chunk per unit: update of the whole data | 8 units, 538 ms | 8 units, 580 ms |
   | 1 chunk per unit: update of one chunk | 68.0 ms | 78.7 ms |
   | 16 chunks per unit: update of the whole data | 8.31 units, 561 ms | 8.31 units, 604 ms |
   | 16 chunks per unit: update of one chunk | 7.3 ms | 8.0 ms |
   | 16 chunks per unit: area for 500 000 updates of one chunk | 184 KB | 184 KB |

   The layout of the pool is untouched and there is one mechanism for every number of chunks
   per unit. Its price: the bitwise CRC-64 is estimated at twice the time of the CRC-32 per
   byte (a check of the whole data is 43 ms per 64 KB, 0.7 s per megabyte); a mismatch does
   not say which chunk; nothing less than the whole data can be checked; an update reads
   the old content of the chunks it replaces, where it reads nothing today; and a chunk that
   was corrupt when it was replaced leaves the checksum wrong although the data is right
   again, until the whole data is written. Putting the data under the CRC-64 of the record
   itself, with no field, is the same computation, but then the scan cannot check the map
   without reading the whole data.

   Proposed: the third form. To be decided with it: whether the check runs at the first
   access after a reset, as in the other areas, or only on an explicit call. None of this is
   in the layout, the figures or the model above.
3. The map in the pair. With one chunk per unit the map is a few bytes: 18 for 64 KB. It
   could live in the log pair itself, the record-in-the-pair variant of EWLF.md, which was
   model checked but is not implemented. The area would then be the pool and the two log
   units: 144 KB instead of 160 KB for 64 KB of data, 3.14 MB for 500 000 updates of the
   whole data. Its price is a second form of the log in the library, and pairs that grow with
   the map, hence fewer commits before the log wears out: 1.76 million for the 8 chunks of
   64 KB, against 3 million; 1.44 million for 16 chunks; 320 000 for 133, which rules it out
   there. `proto_c2p.py` checks the pool over that variant (above). Proposed: the map units
   first; `map_pages` = 0 kept for this form, so that adding it later changes no existing
   area.

   Wear of the log with the map in the pair, one chunk per unit, on the STM32U5A5 (32
   certificates, 8 reserved pairs, 10 000 cycles). A pair is the map rounded up to whole
   words plus three words; a log unit holds floor(480 / pair words) of them, 8 reserved; the
   two log units endure 20 000 fills. The last column is the same count with map units,
   where the log allows 3.04 million commits whatever the data and the map units are sized
   apart:

   | Data | Chunks | Pair | Regular pairs per log unit | Commits the log endures | Erases per 1000 commits, map in the pair | Erases per 1000 commits, map units |
   |---|---|---|---|---|---|---|
   | 16 KB | 2 | 64 B | 112 | 2.24 M | 8.9 | 12.5 |
   | 32 KB | 4 | 64 B | 112 | 2.24 M | 8.9 | 12.5 |
   | 64 KB | 8 | 80 B | 88 | 1.76 M | 11.4 | 14.4 |
   | 128 KB | 16 | 96 B | 72 | 1.44 M | 13.9 | 16.4 |
   | 256 KB | 32 | 128 B | 52 | 1.04 M | 19.2 | 20.3 |
   | 512 KB | 64 | 192 B | 32 | 640 k | 31.2 | 28.3 |
   | 696 KB | 87 | 224 B | 26 | 520 k | 38.5 | 32.2 |
   | 1 MB | 128 | 320 B | 16 | 320 k | 62.5 | 45.0 |
   | 2 MB | 256 | 576 B | 5 | 100 k | 200 | 78.0 |

   The erases are those of the log, and of the map units in the last column; the pool wears
   the same in both forms, one erase per chunk written. Up to 256 KB the map in the pair
   erases less in total, but all of it falls on the two log units, which is what sets the
   limit. 500 000 commits hold up to 87 chunks, 696 KB of data. With the 8-byte checksum of
   the data in the map (open point 2) two lines move: 32 KB to 1.76 M and 696 KB to 480 k,
   and 500 000 commits hold up to 83 chunks. A ring of more than two log units would lift
   the limit in proportion (EWLF.md, the variant); the model does not cover it.
4. The names of the three `lftl_ewlf_*` calls when they apply to a chunked area.
5. Whether this file and EWLF.md stay once the design page of the documentation holds both.
