# Large areas: partial updates, transactions, no rollback, for data of any size

This is the design proposal for *large areas*, a type of area meant to take over from regular
areas as the data grows. A regular area rewrites the whole data at every update: 64 KB of data
updated 500 000 times need fifty slots, 3.1 MB on the STM32U5A5, whatever the size of each
update. A large area cuts the data into erase units and rewrites only the units an update
touches. It gives:

- **the guarantee of EWLF areas**: after any sequence of power cuts at any instant, a boot
  shows the content the last acknowledged update left or a newer one, a content shown at one
  boot is never taken back at a later one, an update that was never acknowledged is never
  shown after an older content was, and nothing is lost. An update is atomic whatever the
  number of units it touches: a boot never shows a mix of two contents;
- **the transactions of regular areas**: several writes, committed together or not at all,
  with the same calls;
- **a checksum of the data**, a CRC-64 over the whole of it;
- **the cost of a regular area** when the whole data is rewritten: the same flash per update,
  two erase units more per area, and nothing ever copied behind the application's back.

A large area is the combination chosen on 2026-11-21 out of the study in
[CHUNKED.md](CHUNKED.md): **one chunk per erase unit, the map in the pair of the log, one
CRC-64 over the data**. CHUNKED.md keeps the study: several chunks per unit, the map in units
of its own, the alternatives that were dropped, and their results.

Nothing here is implemented: the document is to be reviewed before any library code is
written. The target and the tearing model are those of [EWLF.md](EWLF.md): the STM32U5A5,
16-byte write units ("words"), 8 KB erase units ("units"), T1 to T5 of the
[flash page](https://lean-ftl.readthedocs.io/en/latest/flash.html).

## The principle: a map, replaced atomically by the EWLF log

The data is cut into `n` *chunks* of one erase unit each. The chunks live in a **pool** of
erase units, and a **map** says, for each chunk, which pool unit holds its current copy.

- An update writes the new copy of each chunk it touches into a free unit; the old copies
  stay where they are.
- It then writes a new map, which names the new units for the chunks it touched and the old
  units for the others, and commits it.
- The commit is one pair of an EWLF log, in its record-in-the-pair form (EWLF.md, "Variant"):
  the map is the data of the pair. Before the pair, every boot shows the old content; after
  it, the new one. Nothing in between exists.

The commit, the scan, the repair, the log switch, the certificates and the stuck state are
those of EWLF.md. What is new is the pool: where a chunk may be written and when a pool unit
may be erased. The rules follow the principle of the log: a reading proves nothing, a
returned program does.

## Layout

```
area = U pool units, then 2 log units

area base                                              area end
+---------+---------+---  ---+---------+---------+---------+
|   P0    |   P1    |        | P(U-1)  |   L0    |   L1    |
|  chunk  |  chunk  |        |  chunk  |  pairs  |  pairs  |
+---------+---------+---  ---+---------+---------+---------+
|<--------------- pool --------------->|<------ log ------>|
```

**Pool unit.** It holds one chunk, which fills it: no marker, no flag, no checksum, and
nothing says which chunk it is. Only the map says. The last chunk of the data may be
shorter; the rest of its unit stays erased.

**Log unit.** As in the variant of EWLF.md: pair positions, regular then reserved, then the
certificate words. A pair is:

```
+----------+------------------------------+----------+----------+
|  marker  |             map              |   meta   | confirm  |
| all zeros|                              |          | all zeros|
| one word |         whole words          | one word | one word |
|   1st    |             2nd              |   3rd    |   4th    |
+----------+------------------------------+----------+----------+
```

each part programmed only after the one before it returned. The meta word holds the
generation of the log unit, the 32-bit version and the CRC-64 of those and of the map.

**Map.**

```
+----------+----------+---  ---+------------+-----------+--------------+
| unit of  | unit of  |        |  unit of   |   head    |  CRC-64 of   |
| chunk 0  | chunk 1  |        | chunk n-1  |   unit    |   the data   |
|  16 bit  |  16 bit  |        |   16 bit   |  16 bit   |    64 bit    |
+----------+----------+---  ---+------------+-----------+--------------+
```

`0xFFFF` means that the chunk was never written and reads erased. The *head unit* is the
pool unit the update that wrote this map filled last; `0xFFFF` before the first write. The
map of 64 KB is 26 bytes, that of 1 MB 266 bytes.

## Read

A read looks up the unit of each chunk it covers in the map of the current pair, and reads
the chunk there; a chunk whose entry is `0xFFFF` reads as erased bytes. No RAM copy of the
map is kept: it is read from the flash, two bytes per chunk.

## Update

A plain write (`lftl_write`, `lftl_basic_write`, `lftl_erase_all`) covers a range of the data,
hence a run of consecutive chunks:

1. **room**: the update needs one free unit per chunk it touches. When the pool does not have
   them it raises `LFTL_ERROR_LARGE_NO_SPACE` and changes nothing;
2. **the chunks, in increasing order**: for each chunk touched, the head moves to the next
   free unit (below), erases it, then programs the new chunk: the written bytes from the
   caller, the others copied from the current copy, or left erased when there is none;
3. **the commit**: a pair. Its marker, then the map, word by word: the entries of the chunks
   not touched are copied from the current map, those of the chunks touched are the new
   units; then the head unit and the checksum of the data (below); then the meta word and the
   confirm. When the log unit has no regular position left the log switches first, as in
   EWLF.md. The RAM this takes does not grow with the data: one word of map at a time;
4. the update is acknowledged once the confirm of its pair returned.

`lftl_erase_all` is an update whose map names no unit.

The order makes one chain: every program of every chunk returned before the pair was
started. So a map that the log names, names only whole chunks.

## The pool

**Free or current.** A pool unit is *free* when it holds no chunk of the current map, none of
the open transaction and none of the update in progress. Whatever a free unit holds is
garbage: a chunk of an older map, or what a power cut or an aborted transaction left. There
is no third state: a unit holds a whole chunk or nothing of value, so nothing is ever copied
to make room, and there is no garbage collection.

**The head goes round the pool.** After a reset the head is the head unit of the current map.
For each chunk to write, the head moves to the first free unit after it, in increasing order,
wrapping from the last unit to the first. It steps over the units that hold a current chunk,
which are left alone, and over those that hold a chunk of the open transaction or of the
update in progress.

**Erased at entry.** The unit entered is erased at that instant, whatever it reads, and its
chunk is programmed only after the erase returned: a free unit is never trusted because it
reads erased (T4), it may hold the remains of an erase that a power cut interrupted. Nothing
is erased anywhere else, and nothing is erased while a repair of the scan is pending. The
erase is guarded as the rollover of an EWLF area is: if anything started in the log could
still outrank the decision, the decision is buried first.

**Wear.** The free units take the writes in turn, so the erases spread over all the units
but those whose chunk never changes. `W` chunk writes over the life of the area need
`W` / endurance units on top of the chunks that are not rewritten. A unit whose chunk never
changes is never erased and never copied.

**What a power cut burns.** A session that is cut, or a transaction that is aborted, leaves
chunks that no map names, in units that are still free: the next session starts from the head
unit of the committed map, finds the same free units, and erases them again when it enters
them. A power cut burns nothing that lasts; it costs the erases of the units it wrote.

Whether a unit is free is read from the map when needed, one pass, 2 `n` bytes. Nothing about
the pool is kept in RAM beyond the head.

## The checksum of the data

The map carries one CRC-64/NVME over the whole data: the `n` chunks in order, each taken as a
whole erase unit, a chunk never written counting as erased bytes. The CRC-64 of the pair
covers the map, hence that checksum: one chain covers the map and the data.

It is not computed again over the whole data at every update. A CRC is linear, so replacing a
run of chunks changes the checksum by

```
(CRC of the old content  xor  CRC of the new content)  x  x^(8 b)  mod P
```

where `b` is the number of bytes of data after the run and `P` the polynomial. An update
thus reads the old content of the chunks it replaces and checksums the old and the new; the
multiplication is a few dozen shifts and xors of 64-bit words, of bounded time. An update of
the whole data computes the checksum afresh.

What it gives and what it does not:

- a mismatch is an error. It is never a fallback to older data: there is none to fall back
  to, and a fallback would be a rollback;
- it does not say which chunk is wrong, and nothing less than the whole data can be checked;
- a chunk that was already corrupt when it was replaced leaves the checksum wrong although
  the data is right again, until the whole data is written (`lftl_erase_all`, or a write of
  all of it);
- it plays no part in the argument below: a map names a chunk only after the program of that
  chunk returned. It is there for what the flash does to data after it was written.

**When it is checked.** Once per reset, and never by the scan:

- `lftl_check_data` checks it when the application chooses: it reads the whole data,
  computes its CRC-64, and returns whether it matches the one in the map. It raises no error
  on a mismatch: the application has the answer and decides;
- when the application does not call it, the check runs by itself, lazily: before the first
  call that uses data the area already holds. That is a read, a write that does not replace
  the whole data, or the start of a transaction. A write of the whole data and
  `lftl_erase_all` need no check: they replace the data and compute its checksum afresh.

So no data leaves the area unchecked, and an application that never calls
`lftl_check_data` only moves the cost to its first read. Either way the check takes 43 ms
for 64 KB and 0.7 s per megabyte on the STM32U5A5, in one go; calling it explicitly is how an
application chooses the moment.

After the check the area is *sound* or *corrupt* until the next reset. In a corrupt area
every call that uses the data it holds raises `LFTL_ERROR_DATA_CHECKSUM`;
`lftl_check_data` can be called again and returns false; a write of the whole data,
`lftl_erase_all` or `lftl_repair_checksum` (below) is accepted and makes the area sound
again. An error on the area, which
makes it scan again, also makes it check again.

**Reading a corrupt area.** `lftl_read_unchecked` reads the data whatever its checksum
says: it runs no check and needs none, in a sound area, in a corrupt one, and in one that was
not checked yet. It is how an application saves what is still right in a corrupt area, by
its own means of telling, before it writes the data again. It reads through the map the scan
chose, so it also works while a repair is pending and in the stuck state, like
`lftl_read_unproven`, which keeps the check.

**Repairing the checksum.** `lftl_repair_checksum` takes the data as it is: it computes
the CRC-64 of what the area reads now and commits a map that names the same chunks with that
checksum. The area is then sound, and the application repairs the chunks it knows are wrong
by ordinary writes, one erase unit at a time, with no need to hold the data anywhere else.
It is a commit like any other: one pair, atomic, and after a power cut the area has the old
checksum or the new one. It changes no content. It needs an area that is ready, as a write
does, and commits nothing when the checksum already matches.

The call does not make the data right: it declares it accepted. It is for an application
that has looked at the data with `lftl_read_unchecked` and decided what to keep.

While a repair of the scan is pending, the check, asked for or lazy (by
`lftl_read_unproven`), is done on the map `D` names. That map is the one the repair
makes current and the repair touches no data, so the result holds after it.

## Transactions

The calls are those of regular areas, and so are the rules: one transaction at a time per
area, a write unit is written once per transaction, a read returns the committed data unless
it asks for the newer one.

- `lftl_transaction_start` takes a caller-owned *tracker*: one bit per write unit of the data,
  as for a regular area, then one 16-bit unit number per chunk. It erases nothing, where a
  regular area erases a whole slot;
- `lftl_transaction_write` takes a free unit for each chunk it touches for the first time,
  and programs the write units it covers straight into that unit, marking them in the
  tracker. A second write to a marked write unit is `LFTL_ERROR_TRANSACTION_OVERWRITE`;
- `lftl_transaction_commit` completes each chunk the transaction touched (the write units not
  marked are copied from the current copy), then commits the map, as an update does from its
  step 3;
- `lftl_transaction_abort` forgets the tracker. The units it took are free again;
- `lftl_read_newer` and `lftl_transaction_read` return the new value of the marked write
  units and the committed one elsewhere.

A transaction holds the old and the new copy of every chunk it touches until its commit, so
its size is bounded by the room of the pool: see the sizing rule.

## Scan

The scan is the scan of the log, as in EWLF.md: both log units are read once, and the
decision `D` is the newest valid pair of the live unit, whose map is checked against the
CRC-64 of its pair. The pool is not read. The time of the scan does not depend on the size of
the data, nor on the size of the pool.

- When the top of the log is solid, the map is current and the area is readable and writable.
- Otherwise a repair is pending, done by `lftl_complete_scan`; until then reads and
  writes raise `LFTL_ERROR_SCAN_PENDING`, `lftl_scan_status` reports the remaining cuts, and
  `lftl_read_unproven` reads the data through the map `D` names. The chunks of that map
  are intact, whichever of the two candidates it is: no unit is erased while a repair is
  pending. A repair copies a map, not data.

**After an error.** Any error raised on a large area, a refused program, a failed callback,
the stuck state, a lack of room, aborts its transaction and makes the next call scan the area
again, as after a reset. What the area knows in RAM is thus never ahead of, nor behind, what
a boot would find.

## Why there is no rollback, no loss, and no mixed content

1. *The map is the data of an EWLF pair.* What is shown is a map that a proven pair holds;
   the order of maps is the generation and the position of their pairs; a map once shown is
   never outranked. This is the argument of EWLF.md for its variant, not repeated here, and
   it does not depend on what the data of the pair is.
2. *A map names only whole chunks.* Each chunk it names was programmed, to its last word,
   before the pair of the map was started, in the same session, into a unit whose erase had
   returned in that session.
3. *No erase destroys a chunk that a boot can show.* A pool unit is erased only when the head
   enters it, in a session where the area is ready: its scan found the top solid or its
   repair returned, so the current map is final and no older map can be elected again. The
   unit is free: it holds no chunk of that map. The only other map that can still become
   current is the one being written, and the unit holds none of its chunks either. After a
   cut, the two candidates are the map that was current and the one that was being committed;
   nothing is erased until a boot has made one of them final.
4. *The content is never mixed.* A single pair replaces the map. A transaction or an update
   that touched ten chunks is shown with its ten new chunks or with none.

The order in which the head enters the free units plays no part in this argument: it decides
the wear, not what a boot shows.

| The cut lands in | The next boot sees | Outcome |
|---|---|---|
| a chunk | a unit that no map names | the previous content; the unit is free, and is erased again when the head enters it |
| the erase of a pool unit | the remains of a chunk that no map names | the same |
| the pair of the commit | as in EWLF.md: the old map or the new one, repaired before it is shown | the whole previous content or the whole new one, never taken back |

## Residuals

- **The stuck state**, inherited from the log, with the same count and the same report
  (`remaining_cuts`). Nothing in the pool adds a way to it.
- **No room.** An update or a transaction that touches more chunks than the pool has free
  units raises `LFTL_ERROR_LARGE_NO_SPACE` and changes nothing. With the pool sized by the
  rule below it does not happen, whatever the power cuts (simulation below); it is the error
  of a transaction larger than the area was sized for.
- **The life of the log.** Every commit programs a pair that holds the map, so the larger the
  data, the fewer commits the two log units endure: 1.76 million for 64 KB, 320 000 for 1 MB
  (table below). This is the limit the ring of log units is to lift ("Follow-up").
- **Wear under power cuts.** A cut update costs the erases of the units it wrote, like the
  update itself; a boot alone writes nothing.
- As for the other areas: `lftl_format` is not protected against a power cut, and superseded
  chunks stay physically readable until their unit is erased.

The design adds no assumption about the flash to those of EWLF.md.

## Compared with a regular area

On the STM32U5A5, for data that fills 8 erase units less 48 bytes, the best case of a regular
area: its three meta-data items fit in the last unit. With 64 KB exactly a regular area needs
a ninth unit per slot, a large area does not.

| | Regular area | Large area |
|---|---|---|
| Update of the whole data: flash | 8 units | 8 units and an 80-byte pair |
| Update of the whole data: time | 556 ms | 580 ms |
| Update of a few bytes | 8 units, 556 ms | 1 unit, 78 ms |
| Smallest area | 128 KB | 144 KB |
| Area for 500 000 updates of the whole data | 3.12 MB | 3.14 MB |
| Area for 500 000 updates of one unit or less | 3.12 MB | 480 KB |
| Fixed cost on top of the copies of the data | none | 2 log units, 16 KB |
| Limit on the number of updates | 10 000 per slot | 1.76 million, set by the log |
| Start of a transaction | one slot erased, 12.5 ms | nothing |
| Transaction tracker | 512 bytes | 528 bytes |
| First access after a reset | the version of every slot, the checksum of half of them on average: about 30 ms with 2 slots, 0.5 s with 50 | 16 KB of log, a fraction of a millisecond; the data is checked apart, once, 43 ms: when the application asks, or else at the first read |
| Checksum of the data | CRC-32, 27 bits effective, per slot | CRC-64 over the whole data |
| A current version that fails its checksum | the previous version is taken, or an error | an error |
| After a power cut | the old or the new version; a rollback by one version in the cases of the design page | the old or the new content, never taken back |
| Repair call, stuck state | none | as an EWLF area |
| Flash required | any | erased bytes read `0xFF`, refused programs reported (R6 and R9 of the flash page) |

The times are estimates from the figures of the next section. An update of the whole data
is 4 % slower, and all of it is the checksum: the bitwise CRC-64 of the library is estimated
at twice the time per byte of its CRC-32. Without the checksum of the data a large area would
take 538 ms.

## Sizing and costs on the STM32U5A5

Estimates, from 128 us per word programmed, 1.56 ms per erase, about 0.65 us per byte of
CRC-64 and 0.3 us per byte of CRC-32 at 160 MHz. Nothing here is measured. The tables are
printed by `test/ewlf-model/chunked_sim.py --large`.

**Sizing rule.** With `kmax` the number of chunks of the largest update or transaction, and
`W` the number of chunk writes over the life of the area:

```
pool units >= n + kmax              room: the data and the largest update
pool units >= W / endurance + n     wear: the free units take the writes in turn. An area
                                    whose every update rewrites the whole data needs
                                    W / endurance
area        = pool units + 2        the two log units
commits    <= 2 x endurance x regular pairs per log unit
```

**Layout, time and the life of the log**, for data of whole erase units less 48 bytes. A pair
is the map rounded up to whole words, plus three words; a log unit holds floor(480 / words of
a pair) positions, 8 of them reserved, after its 32 certificates:

| Data | Chunks | Map | Pair | Regular pairs per log unit | Commits the log endures | Update of one chunk | Update of the whole data | Regular area, any update | Check of the data | Tracker |
|---|---|---|---|---|---|---|---|---|---|---|
| 8 KB | 1 | 12 B | 64 B | 112 | 2.24 M | 72.9 ms | 73 ms | 70 ms | 5 ms | 66 B |
| 16 KB | 2 | 14 B | 64 B | 112 | 2.24 M | 78.3 ms | 145 ms | 139 ms | 11 ms | 132 B |
| 32 KB | 4 | 18 B | 80 B | 88 | 1.76 M | 78.4 ms | 290 ms | 278 ms | 21 ms | 264 B |
| 64 KB | 8 | 26 B | 80 B | 88 | 1.76 M | 78.4 ms | 580 ms | 556 ms | 43 ms | 528 B |
| 128 KB | 16 | 42 B | 96 B | 72 | 1.44 M | 78.6 ms | 1.16 s | 1.11 s | 85 ms | 1056 B |
| 256 KB | 32 | 74 B | 128 B | 52 | 1.04 M | 78.9 ms | 2.32 s | 2.23 s | 170 ms | 2112 B |
| 512 KB | 64 | 138 B | 192 B | 32 | 640 k | 79.4 ms | 4.64 s | 4.45 s | 341 ms | 4224 B |
| 664 KB | 83 | 176 B | 224 B | 26 | 520 k | 79.7 ms | 6.01 s | 5.77 s | 442 ms | 5478 B |
| 1 MB | 128 | 266 B | 320 B | 16 | 320 k | 80.6 ms | 9.27 s | 8.90 s | 682 ms | 8448 B |
| 2 MB | 256 | 522 B | 576 B | 5 | 100 k | 83.0 ms | 18.54 s | 17.81 s | 1.36 s | 16896 B |

An update counts its chunks, the erases of their units, its pair, the checksum of the map,
the checksum of the data (the old and the new content of the chunks, or the whole data) and
its share of the erases of the log. 500 000 commits hold up to 83 chunks, 664 KB of data.

**Area**, 10 000 erase cycles, a transaction of the whole data allowed (`kmax` = `n`); the
smallest area, then the area for 500 000 writes by their size:

| Data | Smallest area | Regular area, 2 slots | Writes of one chunk | of two chunks | of the whole data | Regular area, any write |
|---|---|---|---|---|---|---|
| 8 KB | 32 KB | 16 KB | 416 KB | 416 KB | 416 KB | 400 KB |
| 16 KB | 48 KB | 32 KB | 432 KB | 816 KB | 816 KB | 800 KB |
| 32 KB | 80 KB | 64 KB | 448 KB | 848 KB | 1.58 MB | 1.56 MB |
| 64 KB | 144 KB | 128 KB | 480 KB | 880 KB | 3.14 MB | 3.12 MB |
| 128 KB | 272 KB | 256 KB | 544 KB | 944 KB | 6.27 MB | 6.25 MB |
| 256 KB | 528 KB | 512 KB | 672 KB | 1.05 MB | 12.52 MB | 12.50 MB |
| 512 KB | 1.02 MB | 1.00 MB | 1.02 MB | 1.30 MB | 25.02 MB | 25.00 MB |

A write of a few bytes is a write of one chunk, or of two when it straddles two erase units.
In every cell a large area is the regular area plus the two log units when the whole data is
rewritten, and smaller by the ratio of the data to the write otherwise.

**Boot and RAM.** The scan is the scan of an EWLF log, a fraction of a millisecond. Nothing in
RAM grows with the data outside a transaction.

## Simulation of the pool

`test/ewlf-model/chunked_sim.py` plays the pool at real sizes, with the rules above and
without the tearing model, for sequences of writes, transactions, aborts and power cuts:
uniform, skewed (90 % of the writes on 10 % of the chunks), always the same chunk, mixed
sizes, the whole data every time, every chunk for a while then one chunk only, and an attack
on the room (every other operation is the largest transaction, cut right before its commit).
The pool does not depend on where the map is kept, so these are the runs of CHUNKED.md with
one chunk per unit (`-S 1`).

- *Room.* 112 runs of 30 000 operations at the smallest pool the room rule allows, data of 2,
  8 and 16 chunks, a largest update of 1, 8 or all chunks, with and without 5 % of cut and
  5 % of aborted operations: no operation lacked room. With one unit less, operations lack
  room in 96 of the 112 runs: the rule has no slack to give back.
- *Copies.* None in any run: a chunk written costs one unit, 1.09 with the cut and aborted
  operations.
- *Wear.* 100 000 operations on 8 chunks in 16 and in 58 units and on 32 chunks in 64 units,
  six sequences each. Per erase of its most erased unit the pool took at least `U` - `n` + 1
  chunk writes in every sequence, where the sizing rule counts on `U` - `n`; the lowest
  figure is for the sequence that always writes the same chunk. With 5 % of cut and 5 % of
  aborted operations it is 8 % lower: a cut operation costs the units it wrote. Under the
  attack each cut costs what the transaction it interrupts would have cost, as for the other
  areas.

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

The pool rules are one module, `proto_c2.py`, run over three logs; a large area is its
geometry with one slot per unit:

- `proto_c2p.py`: over the record-in-the-pair log of `proto_v4p.py`, whose functions are
  imported unchanged. This is the large area;
- `proto_c2a.py`: over the *contract* of the log instead of the log: a commit that is cut
  leaves the old map, or either map at each later boot until a repair returns, or the new
  one. It has far fewer states, and explores the pool further; it says nothing about the
  log, which EWLF.md covers;
- `proto_c2.py`: over the log of `proto_v4.py`, with the map in units of its own. Not the
  layout of a large area; the same pool rules over the other form of the log.

A chunk is one word in the model, and so is the map: a cut inside a chunk is that word weak.
"Warm" runs start from a pool in use (five writes without cuts first), so that the first
operation explored meets a pool that has gone round. On the real logs the warm runs ignore
the stuck state: the model's log has two certificates, and EWLF.md counts the cuts that lead
there. Every run is `--strict`. The pools are the smallest the room rule allows:

- *A*: 2 chunks, 4 units (`2,4,1`);
- *D*: 3 chunks, 6 units (`3,6,1`), operations `w0,w12,t012,a1`.

```
cd test/ewlf-model
python3 chunked_model.py --protocol c2p --geometry 2,4,1 --warmup w01,w0,w0,w0,w0 --ignore stuck --cuts 2 --boots 4 --strict
```

| Module | Pool | Start | Cuts / boots | Options | Result |
|---|---|---|---|---|---|
| `c2p` | A | format | 1 / 4 | | no violation (8.8 k states, 25 k sessions) |
| `c2p` | A | warm | 2 / 4 | | no violation (94 k states, 29.9 M sessions, 38 min) |
| `c2p` | A | warm | 2 / 4 | one regular pair per log unit (`2,4,1,2,2,1,2,2`): a log switch at every commit | no violation (83 k states, 2.4 M sessions) |
| `c2a` | A | format | 2 / 5 | | no violation (107 k states, 304 k sessions) |
| `c2a` | A | warm | 2 / 5 | | no violation (110 k states, 313 k sessions) |
| `c2a` | A | warm | 2 / 5 | `--weak-erased` | no violation, the same 313 k sessions: the pool never reads a unit whose erase was cut |
| `c2a` | A | warm | 3 / 6 | | no violation (2.1 M states, 7.3 M sessions) |
| `c2a` | D | warm | 3 / 6 | | no violation (3.0 M states, 8.6 M sessions) |
| `c2a` | A | warm, two operations per session | 1 / 4 | | no violation (1.3 M states, 3.7 M sessions) |
| `c2a` | A | the same | 2 / 3 | | no violation (235 k states, 975 k sessions) |
| `c2a` | A | the same | 2 / 4 | | no violation (6.2 M states, 24.0 M sessions) |
| `c2` | A | format | 1 / 4 | | no violation (12 k states, 28 k sessions) |
| `c2` | A | warm | 2 / 4 | | no violation (142 k states, 4.1 M sessions) |
| `c2` | A | warm | 2 / 4 | a rollover of the map units and a log switch at every commit (`2,4,1,2,1,1,2,2`) | no violation (114 k states, 972 k sessions) |
| `c2` | A | warm, two operations per session | 1 / 3 | | no violation (169 k states, 1.8 M sessions) |
| `c2` | A | warm | 2 / 5 | | no violation (825 k states, 22.9 M sessions, 31 min) |

Still running when this was written, on `c2p`, and added here as they end: pool A with 2 cuts
and 5 boots, pool D with 2 cuts and 4 boots, pool A with two operations per session.

Negative controls (`C2_KNOCKOUT=` in the environment), warm, stuck ignored. Each rule,
removed alone, gives its violation:

| Rule removed | Control | Result | Seen on |
|---|---|---|---|
| a unit is entered only when it holds no chunk of the current map | `live`: those chunks are not looked at | **readback**: the update erases a current chunk, and the content read after it is wrong | `c2a`, `c2`, `c2p` |
| nor of the open transaction | `txn`: those chunks are not looked at; 2 chunks in 3 units, no room ignored | **readback**: the transaction erases its own first chunk | `c2a`, `c2`, `c2p` |
| a unit is erased when the head enters it, whatever it reads | `trust`: no erase when the unit reads erased | **corrupt**: a chunk written into cells an interrupted erase left reads garbage later (T4) | `c2a`, `c2`, `c2p` |

One rule has no control: the bury before an erase in the pool when the top of the log is not
solid. As for the rollover of an EWLF area, a session that scanned never meets that case; the
rule is a defence. The order of the round has none either: it decides the wear, which the
simulation measures, not what a boot shows.

The checker proves the absence of counterexamples within its bounds and under its model. It
does not cover: more than three chunks, the words inside a chunk, partial writes of a chunk,
the room of the pool at real sizes (the simulation covers it without the tearing model), and
the checksum of the data, which no rule above reads.

## Limits

- the residuals above: the stuck state, the repair to ask for after a reset, the life of the
  log;
- the flash must read `0xFF` when erased and report a refused program, as for an EWLF area;
- every commit rewrites the whole map: with two log units the map is bounded by the life
  wanted from the log, 83 chunks for 500 000 commits, and by the room for the reserved pairs,
  about 350 chunks;
- the pool must be sized for the largest transaction;
- the first read after a reset pays the check of the whole data, unless the application
  asked for it earlier; the check is not split into steps;
- an update of a few bytes rewrites one erase unit: 78 ms on the STM32U5A5, where the
  several-chunks-per-unit form of CHUNKED.md took 7 ms and paid for it with garbage
  collection.

## API

```c
LFTL_AREA_LARGE(name, area_content, pool_pages)

lftl_ctx_t area = { ..., .data_size = sizeof(nvm.name_data) };
lftl_register_area_large(&area);

bool lftl_check_data(lftl_ctx_t *ctx);   /* optional: true when the data matches its checksum */
void lftl_read_unchecked(lftl_ctx_t *ctx, void *dst, const void *src_nvm_addr, uintptr_t size);
void lftl_repair_checksum(lftl_ctx_t *ctx);
```

"Page" is the library's word for the erase unit in its macros; the area is `pool_pages` + 2
pages.

Then the calls of the other areas: `lftl_format`, `lftl_read`, `lftl_write`,
`lftl_write_any`, `lftl_basic_write`, `lftl_erase_all`; the transaction calls of regular
areas, with `LFTL_TRANSACTION_TRACKER_SIZE` giving the larger tracker; and the three calls of
the two-phase scan, `lftl_scan_status`, `lftl_complete_scan`, `lftl_read_unproven`.

Those three are the `lftl_ewlf_status`, `lftl_ewlf_complete_scan` and
`lftl_ewlf_read_unproven` of EWLF areas under neutral names: one set of calls, which
dispatches on the area as `lftl_read` does, for every type of area that has a log. The
`lftl_ewlf_*` names exist only on the unreleased v0.4.0 branch (the last tag, v0.2.0, has
none of them), so they are renamed, not kept as aliases. With them, on the same ground:
`lftl_ewlf_status_t` becomes `lftl_scan_status_t`, `LFTL_ERROR_EWLF_PENDING` becomes
`LFTL_ERROR_SCAN_PENDING` and `LFTL_ERROR_EWLF_STUCK` becomes `LFTL_ERROR_STUCK`. On a regular
area nothing is ever pending: `lftl_scan_status` says so, `lftl_complete_scan` runs its scan,
`lftl_read_unproven` is a read.

Three calls are new ("The checksum of the data"): `lftl_check_data`, optional, checks the
data against its checksum at the moment the application chooses, and reports a mismatch by
its return value, not by an error; `lftl_read_unchecked` reads the data without that check,
corrupt or not; `lftl_repair_checksum` makes the checksum that of the data as it is. Their
names are neutral too, because what they do is not tied to the large type: any area whose
data has a checksum that is checked apart from the scan could offer them. Today only a
large area does, and on an area of another type they raise `LFTL_ERROR_BAD_CONFIG`, as a
call on the wrong type of area does elsewhere in the library. Only what declares the type
keeps its name: `LFTL_AREA_LARGE`, `lftl_register_area_large`.

A large area is also accepted as the source of a write into any area, itself included: the
source is then read chunk by chunk. Two new errors: `LFTL_ERROR_LARGE_NO_SPACE`, and
`LFTL_ERROR_DATA_CHECKSUM` for the use of data whose checksum does not match. The number of
pool pages is part of the on-flash format, like the data size: nothing on the flash records
it. The format is new in v0.5.0; regular and EWLF areas are unchanged.

A large area is one area for the library: one context, registered once, in a list of large
areas next to the lists of regular and of EWLF areas. Its log is not an EWLF area inside it:
it has no context of its own and is in no list, so the functions that walk the registered
areas (to find the area of an address, to resolve the source of a write) see the large area
and nothing else, and have nothing to leave out. The data range they match is the data of
the large area; the log units lie after the pool and are never part of it.

In the library this is a second form of the log: the EWLF areas of v0.4.0 keep their record
in data units, a large area keeps its map in the pair. The scan, the bury, the carry and the
switch of that form are new code next to the existing ones.

## Follow-up

1. **A ring of log units instead of the pair of log units.** With `M` log units, generation
   `g` in unit `g` mod `M` and every switch going to the next unit, the life of the log grows
   with `M`: 640 000 commits for 1 MB of data with four log units, against 320 000 with two.
   EWLF.md describes it for its variant; `proto_v4p.py` asserts two log units, so the ring
   needs its own model check.
2. EWLF areas may then be renamed **small areas**: the same log, with a record instead of a
   map.
3. Regular areas may be dropped entirely. What stands in the way today: a large area needs a
   flash that reads `0xFF` when erased and reports a refused program, and deployed regular
   areas (the v0.1.0 format that test 37 guards) need a way across.

## Considered and not kept

CHUNKED.md has the detail and the figures of each.

- **Several chunks per erase unit.** A small update then programs a fraction of a unit: 7 ms
  instead of 78 ms for 64 KB in 496-byte chunks. Its price is a start marker per chunk, units
  that are partly garbage, a garbage collection that copies chunks, a reserve, and an update
  time that depends on the state of the pool. Its rules were model checked and simulated.
- **The map in units of its own**, with the log of EWLF areas as it is in the library: no
  second form of the log and 3 million commits whatever the data, for two erase units more.
- **A checksum per chunk**, in the last word of each unit (the chunk no longer fills its
  unit) or in the map (4 bytes per chunk in every pair).
- **No checksum of the data.**

## Open points

None is left from the review. Decided along the way: the name of the type; one chunk per
erase unit, the map in the pair, a CRC-64 over the data; an optional check call and a lazy
check otherwise; no check in steps; a read of a corrupt area and a repair of its checksum;
neutral names for the calls of the two-phase scan and for those of the checksum. This file, CHUNKED.md and EWLF.md go once
the documentation holds the designs well enough.

Confirmed on 2026-11-21:

1. "large" means one chunk per erase unit only; several chunks per unit is not kept;
2. "checked lazily" means once, in full, before the first use of the data after a reset;
3. the neutral names extend to the status type and to the two error codes above.
