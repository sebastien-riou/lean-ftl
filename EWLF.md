# EWLF: extreme wear-leveling factor areas, without rollback

This is the design proposal for EWLF areas: areas that pack many versions of a small block of
data into few erase units, so that the wear-leveling factor is many times the number of erase
units allocated. The goal, beyond the factor, is an anti-tearing argument with **no residual
rollback and no residual loss**: after any sequence of power cuts at any instant, a boot shows
the last version an update acknowledged or a newer one, a version shown at one boot is never
taken back at a later one, a write that was never acknowledged is never shown after an older
version was, and the data is never lost. What remains is a *stuck* state, reached only by a
long run of consecutive boots each cut inside the same short window and sized by a
configuration constant, in which the area refuses to show a version it cannot prove rather
than guess. The data is then physically present but not vouched for: a loss of availability,
discussed under "Residuals". The area reports at every boot how many such cuts it can still
absorb, and writes nothing at a boot until the application has seen that figure and asked it
to proceed.

The target is the STM32U5A5: write unit (word) of 16 bytes, erase unit of 8 KB, erased bytes
read `0xFF`, and the programming rule of RM0456 section 7.3.9 as measured in `FLASH.md`: a
program is accepted into a word that reads erased, or when the data is all zeros into a word
that reads partially programmed; everything else, including all zeros over a word that reads
all zeros, is refused with PROGERR and programs nothing. The tearing model is T1-T5 of the
[flash page](https://lean-ftl.readthedocs.io/en/latest/flash.html): a cut affects only the
word or unit being written (T1); a word whose program was cut is *weak* and may read anything,
differently at every boot (T2); T3 is never assumed; a cut erase leaves cells that may read
erased at one boot and not at a later one, and a word programmed into them may later read
wrong (T4); a weak word may fault instead of returning data (T5). The design has been model
checked against exactly this model (section "Model check").

## The principle: a reading proves nothing, a returned program does

A word a cut left weak may read valid today and erased tomorrow. So no decision that a later
boot could contradict is ever taken on a reading: what is shown, what is erased, and the order
of versions rest only on programs that *returned*, either in the current session or, for
earlier sessions, through a **chain proof**: when the protocol programs word B only after the
program of word A returned, B reading programmed (all zeros, for a zero word that proves
something) proves that A is solid and reads its value for ever. The last word of a chain is never proven; the protocol is arranged so that nothing
depends on it. All zeros is special: a program of all zeros that returns leaves solid zeros
whatever the word held, so zero words are the only marks that can be renewed; they cannot be
confirmed once they read all zeros, so no single zero word is ever trusted as a durable mark.

Three consequences shape the layout:

- the data and the decision live apart: records hold data, a **log** of entries says which
  record is current, and the log is appended to, never decided from a reading;
- a torn erase is invisible from inside the unit (T4), and a weak entry of the new generation
  exposed to a torn erase can come back later, above the live content: so content enters a
  unit only after an erase that returned in the same session and a **certificate** that
  session saw return, written in the other unit, or above a word that entered that way; a
  unit without a certificate reading programmed is erased before use, which is safe because
  nothing entered it;
- a solid copy of the decision must exist outside a unit before that unit is erased, and
  above anything that could resurface before a record is erased or a version is shown:
  **reserved pairs** and the **bury** rule provide it.

## Layout

```
area = N data units (2 <= N <= 256) then 2 log units, all 8 KB on the STM32U5A5

area base                                              area end
+-----------+------  ------+-----------+-----------+-----------+
|    D0     |              |  D(N-1)   |    L0     |    L1     |
|  records  |              |  records  |  pairs    |  pairs    |
+-----------+------  ------+-----------+-----------+-----------+
```

Records fill a data unit from its top (highest address) down, and the data units are used
from D(N-1) down to D0, then D(N-1) again. Pairs fill a log unit from its base up. A "word"
below is a write unit, 16 bytes on the STM32U5A5; on a flash with smaller write units the
meta field and the entry span several, programmed in one call, and nothing else changes.

**Record**, in a data unit, positions of fixed size numbered from the top of the unit; the
highest started number is the newest:

```
+--------------+---------------------------+---------------------+
| start marker |           data            |        meta         |
|  all zeros   |      data_size bytes      | version, checksum   |
|  one word    | rounded up to whole words |  12 B in one word   |
|  programmed  |                           |                     |
|  1st         |           2nd             |         3rd         |
+--------------+---------------------------+---------------------+
```

The meta field holds the 32-bit version and the CRC-64/NVME of version and data. A record has
no flag: whether it is current is the log's business. A record whose words are weak is never
elected (no entry names it after a boot, see the bury rule) and never repaired; it is dead in
place until its unit is erased.

**Log unit** (generation g lives in unit g mod 2): from the top, `P` regular pair positions,
then `R` reserved pair positions, then `X` certificate words.

```
+---------+---------+-- --+---------+----------+-- --+----------+------+-- --+------+
| pair 0  | pair 1  |     | pair P-1| reserved |     | reserved | cert |     | cert |
| regular | regular |     | regular |  pair 0  |     | pair R-1 |  0   |     | X-1  |
+---------+---------+-- --+---------+----------+-- --+----------+------+-- --+------+
```

**Pair**: three words, programmed in this order, each only after the previous one returned:

```
+--------------+----------------------------------------------+--------------+
|   marker k   |                   entry e                    |  confirm c   |
|  all zeros   | generation 2 B, data unit 1 B, position 1 B, |  all zeros   |
|   one word   | version 4 B, CRC-64/NVME of the above 8 B    |   one word   |
+--------------+----------------------------------------------+--------------+
```

- *k* first, so that a position whose entry may be weak is visibly started at every boot
  (zeros never read as anything but zeros or erased, and a program of zeros over a weak
  marker makes it solid); a valued word is thus never programmed over a weak one;
- *e* names the record by unit and position (the lookup is one read), carries the version
  and the generation of its unit, and is *valid* when its CRC verifies and its generation
  has the parity of its unit. The record's own meta field carries the data checksum, so the
  entry needs no copy of it;
- *c* after e returned: c reading all zeros proves e solid (chain proof). An entry whose c
  reads all zeros is *proven*. All zeros, not any zero bit: T5 says that an erased line with
  two flipped bits goes undetected, so a few zero bits in a word that should be erased are no
  evidence; a returned program of zeros always reads all zeros, and a weak one that reads
  partially programmed only costs a bury.

Regular pairs take the application's updates. Reserved pairs are never taken by an update's
regular append: they serve the scan's re-commits and the switches, so that a full unit still
has room to make a decision solid. Certificate words are zero words, and count as programmed only when they read all zeros
(same reason as c); a certificate of unit L programmed and returned means: *the erase of the
other unit returned in the session that programmed it, and content of the next generation
may have entered the other unit since*.

Generations are compared as successors modulo 2^16: the two units always hold adjacent
generations, so 16 bits never wrap into a wrong order.

`X` is an area parameter, given at registration like the data size, with a build-time default
(`LFTL_EWLF_CERTIFICATES`, 32) for the areas that leave it at zero; `R` is a build-time
constant (8). Both are part of the on-flash format of the area: nothing in the area records
them, the layout follows from them at registration, `P` = (words per unit - `X` - 3 `R`) / 3,
and an area whose `X` changes must be formatted again. The configuration check requires
`X` >= 1 and room for the `R` reserved pairs plus two regular ones. The protocol does not
depend on the value: only the stuck count and `P` move, three certificates per regular pair.
On the STM32U5A5 with `R` = 8 and `X` = 32: 896 bytes reserved, `P` = 152 regular pairs
(48 bytes each) per 8 KB log unit; `X` = 16 gives `P` = 157, `X` = 64 gives 141, `X` = 128
gives 120.

## What a boot derives from one reading of the log

Every word of both log units is read once per session (one memory-mapped pass of 16 KB on
the STM32U5A5; a reading is never repeated within a session, T2). From these readings:

| name | definition |
|------|------------|
| started position | any of its three words reads anything but erased; a garbage or faulting reading (T5) counts |
| valid entry | e reads valid: CRC verifies, generation parity matches the unit |
| proven entry | valid, and its c reads all zeros (e is then solid by the chain proof); a faulting c counts as erased |
| `started(l)` | the last started pair position of unit l, regular or reserved |
| `closed(l)` | a reserved position of l is started, or any certificate of l reads all zeros; no regular append goes to a closed unit (reserved pairs may still be used) |
| `certified(l)` | any certificate of l reads all zeros; it speaks about the OTHER unit's erase; a faulting certificate counts as erased |
| live unit `L` | the unit with the highest generation among units holding a valid entry; `O` the other |
| decision `D` | the newest valid entry of L: highest position, reserved positions counting as newer than regular ones |
| `uncertain(O)` | L is certified, O has a started pair position and no valid entry: a carry attempt of generation g+1, possibly weak, may be there |
| top solid | D is proven, nothing is started above D in L, and O is not uncertain |

The order of consultation: first the valid entries of both units, which decide L by
generation; then the certificates of L; then, only when L is certified, O's started
positions. O's own certificate words are never read: they belong to O's previous life and may
be ghosts of a torn erase. Without a certificate of L, O's started positions and weak words
are a dead life or a ghost of a lower generation, and are not consulted. A weak word that
reads programmed may close or certify a unit too; that only costs an earlier switch, never a
wrong decision.

D is the newest *valid* entry, not the newest proven one, on purpose: with L = [w2 proven,
w3 with e solid and c weak], a boot that reads c programmed shows w3; a later boot that reads
it erased must not fall back to w2, it re-commits w3.

## Boot scan

The scan is two phases, so that the application decides before anything is written: a
read-only first phase, run by the first call that needs the area after a reset, and a
completion call that writes only when a repair is pending. Between the two the application
is running, not yet fully initialised; it can ask for the state of the area and decide.

**Phase 1, read-only** (`lftl_ewlf_status`, or the first of `lftl_ewlf_complete_scan`,
`lftl_ewlf_read_unproven`, a read or a write; registration reads nothing, an area is
registered before it is formatted):

1. Read both log units; find L, O and D. No valid entry anywhere: `LFTL_ERROR_NO_VALID_VERSION`
   (never formatted, a cut format, or foreign flash).
2. Check that D's record is in place (its meta field reads D's version and the checksum
   verifies). It always is for an entry this protocol wrote (section "Why there is no
   rollback"); a mismatch comes from a cut first format (section "Format") or a flash fault,
   and is reported as no valid version.
3. Derive the state of the area and keep it in the context for the session: whether the top
   is solid and, if not, which repair is pending (a bury in L, a carry into O, or a switch
   with an erase); the *remaining cuts* figure of the "Residuals" section; D's version. A
   status call returns them. A pending repair that has no room is reported as stuck by that
   call.

**Phase 2, completion** (`lftl_ewlf_complete_scan`):

4. If the top is solid: show D's record. Programs nothing, erases nothing: this is every boot
   after a session that was not cut, with the exception stated under residual (f).
5. Otherwise **bury** D first, then show it. To bury is to re-commit D above every started
   position, by the first of these that applies: when O is uncertain, carry D into O above O's
   started positions by switch step 3 (a copy appended in L would leave a possible
   generation g+1 entry above it); else append the copy at L's first free regular position
   above the started ones, if L is open and has one; else at a fresh reserved pair of L; else
   switch with D as the carry. The copy's three programs return before anything is shown.
   No room anywhere: stuck, nothing is written.

Phase 2 decides from phase 1's readings and never reads the log again (T2). While a repair is
pending, `lftl_read` and the write functions on the area are refused with
`LFTL_ERROR_EWLF_PENDING`, so that no unvouched data reaches the normal path; when the top is
solid nothing is pending, the area is readable and writable at once, the completion call is a
no-op, and the application may call it unconditionally. An explicit **unproven read**
(`lftl_ewlf_read_unproven`) is available from phase 1 on, pending or stuck: it returns D's record,
which is solid in place (its programs returned before D's entry was started, and its unit is
never erased while D can name it), and within the session it is exactly the version phase 2
will make solid; what is unproven is that a later boot, if this session ends before phase 2,
elects the same one. An application that needs this area's content to decide, for instance to
authenticate the user before proceeding, uses it knowing that; its own state for that
decision, such as a retry counter written before a check, must live in another area, since
this one accepts no write before phase 2.

For the argument and the checker the split changes nothing: a session that ends between the
two phases is a cut before the first program of the bury, a cut point the checker explores,
and a session that completes later is the uncut scan.

## Update

An update (`lftl_write`, `lftl_basic_write`, `lftl_erase_all`) does, in order, each step
started only after the previous one returned:

1. *Room for the record*: the first position of the current record's data unit above the last
   started one (start marker reading programmed). A position whose start marker the flash
   refuses to program (a weak marker that reads erased this instant) counts as started and is
   stepped over. If the unit has none left: rollover.
2. *Rollover*: erase the next data unit (cyclic order, never the unit holding the current
   record), whatever it reads, right before its first record (T4); use its top position. The
   implementation checks that the top is solid first and buries D if not; after a scan it
   always is, since the scan showed D only with a solid top and every program since
   returned: the check is defensive.
3. Program the record: start marker, data, meta.
4. *Commit*: append the pair (k, e, c) at L's first regular position above the started ones,
   if L is open and has one; otherwise **switch**, with the new entry as the carry. A refused
   program of k, e or c (a weak word reading erased this instant) counts the position as
   started; the next position is tried.
5. Return success. The update is acknowledged only after c returned.

The remaining cuts figure is the application's guard on this path: a write that needs a
switch is the attempt the certificate reserve counts, every boot shows D meanwhile, and the
library refuses nothing before stuck itself. After a switch that returned the figure is back
to `X`; the status call recomputes it from the session's state.

A cut before step 4's last word leaves the next boot free to make either version current: the
old one if the new entry does not read valid then (the new record is dead in place), the new
one if it does; whichever it is, the boot leaves it solid above everything started (a bury,
unless nothing above D is started) before showing it, and that choice is never revisited. A
cut after c returned leaves the new version current.

## Switch, from L (generation g) to O (generation g+1)

Done by the session that needs room in O, a scan or an update, with the entry to commit as the
*carry*, the first entry of O's new life (an ordinary entry of generation g+1):

1. If no certificate of L reads programmed and L has no proven entry: program a fresh reserved
   pair of L copying D, so that a solid decision exists outside O before O is erased (no free
   reserved pair: stuck). D may then be a weak entry reading valid, a forward choice that
   this pair makes solid before the only solid entry, in O's previous life, is erased.
2. If no certificate of L reads programmed, or no pair position of O is started: erase O in
   this session, whatever it reads; program a fresh certificate of L and see it return (no
   free certificate: stuck); write the carry at O's top position.
3. Otherwise (a certificate of L reads programmed and O has a started pair position): write
   the carry at O's first free position above its started ones, regular or reserved (none:
   stuck). No erase and no new certificate: the started word was programmed after a
   certificate returned, by the session that programmed both, so a solid certificate exists
   and no later boot erases O. When the carry lands on a reserved position, O is live and
   closed at birth: the next update switches again at once, and that update costs two log
   erases; when it lands on O's last reserved pair with its confirm cut, the next boot is
   stuck at once (no room to bury).

Why the certificate: without it a boot cannot tell a carry attempt, whose entry may be weak,
from a ghost of O's previous life, and must choose between erasing O (a torn erase then leaves
the weak generation g+1 entry able to come back above the live content, which the checker
turns into a resurrection then a rollback) and writing into O (into weakly erased cells if it
was a ghost). The certificate decides it: it reads programmed only if its program started,
after O's erase returned.

Why step 2 erases a certified O with no started pair position: the certificate that reads
programmed may be weak, the attempt it belongs to may have died in the carry's marker, and
the session that reads it cannot know which. Writing the carry on that reading would put a
generation g+1 entry under a certificate that may read erased at the next boot, which would
then erase O again, possibly torn: the case above. An erase this session sees return and a
certificate it sees return re-establish the invariant at the cost of 0.88 ms; O holds nothing
to lose (no started pair position). The checker confirms the clause is load-bearing (negative
control "empty" in the model-check table).

## Why there is no rollback and no loss

1. *What is shown is solid.* D is proven (its c reads a zero bit, programmed only after e
   returned) or was re-committed by three programs that returned this session, and the show
   comes after the last of them returned. A solid entry reads valid at every later boot until
   its unit is erased. Its record is solid too: programmed before the entry, into a unit
   erased in the session of that unit's first record, before it; and a unit holding a record
   that may still be elected is never erased (points 4 and 5).
2. *Order is (generation, position), never a reading.* An entry is written only above every
   started position of its unit, by an update (a newer version) or a bury (a copy of D).
   Generations advance by one per switch. Nothing can outrank a shown D later: everything
   started above D at the time, in L or in an uncertain O, was buried under a solid copy of D
   before D was shown, and later entries are newer by construction. This is also why an
   aborted write is never shown after an older version was (no resurrection): once a boot
   showed D, every started position that could hold the aborted write lies below a solid D.
3. *No ghost of a torn erase is elected or programmed into.* Content enters a unit only after
   an erase that returned in the same session and a certificate that session saw return, or
   above a word that entered that way. A unit whose certificate does not read programmed is
   erased again before use, and that is safe: nothing entered it since its erase. A ghost
   that reads valid entries holds its unit's previous generation, below the live one.
4. *Erasing a log unit destroys nothing elected.* O is erased only when nothing entered it
   (no certificate of L reads programmed, and then only after L holds a proven entry, made if
   needed by a reserved pair, since O's previous life may hold the only proven decision), or
   when it has no started pair position (nothing to lose).
5. *Erasing a data unit destroys nothing elected.* The current record's unit is never erased,
   and a data unit is erased only in a session whose scan left D solid above everything
   started, so every entry whose record goes is below a solid D.

What a possibly weak reading may trigger, and what it never decides:

| reading | may trigger | never |
|---------|-------------|-------|
| c of the newest valid entry reads erased | a bury (three programs) | choosing an older version |
| a started position above D, e reading erased or garbage | a bury | electing that position later: it is buried |
| a started position above D, e reading valid | that entry becomes D and is buried (a forward choice) | taking it back later |
| a certificate of L reads programmed | an early switch, no erase of O while a pair position of O is started | erasing content that entered O |
| a certificate of L reads erased | an erase of O | erasing content that entered O: none did |
| O started, no valid entry, L certified | a carry above O's started positions | electing O's weak attempt later |
| a start marker, a pair word or a certificate refused | the next position or word | a wrong version |

The classic failure shapes, one by one:

| the cut lands in | the next boot sees | outcome |
|------------------|--------------------|---------|
| record (marker, data, meta) | position started, no entry names it | old version; dead record stepped over |
| entry e, reads invalid | started position above D: bury | old version stands, solid |
| entry e, reads valid | D unproven: bury (re-commit) | new version stands, never taken back |
| confirm c | e solid; shown if c reads a bit, else re-committed | new version stands |
| bury or carry | same state one position higher | repeats, converges |
| data-unit erase | ghost records, no entry names them | redone before the first record (T4) |
| log-unit erase, O not certified | ghost of generation g-1 | erased again before use |
| log-unit erase, O certified with no started pair position | an empty O, or a weak carry marker now reading programmed | erased again, or a carry above it; nothing entered O |
| certificate | O erased, no started pair position | O erased again, new certificate |
| certificate returned, then the carry's marker | O started under a certificate, or empty | carry above it, or erase again (one certificate burnt) |

## Residuals

**Stuck.** The one state in which the protocol refuses to proceed, reported as
`LFTL_ERROR_EWLF_STUCK`. It has two faces. At an update: the switch finds no
free certificate, reserved pair or position; the write fails, every boot still shows D (the
top is solid), and every later write fails the same way. At a boot: phase 1 of the scan finds that
the bury would have no room; phase 2 refuses to show a version it cannot prove. Both are a
loss of availability, not of data: the content is physically present, and in the second case
the version the scan would have chosen is readable but not vouched for. The way out is the
unproven read of the "Boot scan" section, which salvages that content, then a format, which
gives the rest up. Stuck is reached only by exhausting a reserve, one word, pair or position
per cut, each cut inside a specific program of a switch or a bury:

| reserve | what one cut burns | window per cut on the STM32U5A5 | boots to stuck |
|---------|--------------------|--------------------------------|----------------|
| certificates `X` of L | a cut in the certificate's program, or in the carry's marker right after it, the marker then reading erased | 256 us | `X` = 32 attempts |
| positions of O | a cut in a carry program, O then holding a started position | 384 us | `P` + `R` = 160 attempts |
| reserved pairs `R` of L | reachable only once L holds no proven entry: a cut in every pair of L's life (`P` cuts, each in e or c, 256 us, or k if the program is refused later), then a cut per reserved pair | 256 to 384 us | `P` + `R` = 160 attempts |

Burnt words stay burnt until their unit is erased, two switches later; what ends the count
is a returned carry. The attempts need not be consecutive boots: a boot that does not switch
neither burns nor resets, so `X` bounds consecutive attempts at one switch, possibly spread
over days. The certificates dominate: 32 attempts each cut in the same 256 us window, each
preceded by the 0.88 ms erase of O. A brown-out that recurs at a fixed point of a switch on
marginal power is the realistic way there, so `X` should be sized for it: 64 certificates
cost 1 KB of each log unit. A third log unit does not help here; a larger `X` does.

**Remaining cuts, the figure the application gets.** Phase 1 of the scan, and the status
call at any time in the session, report one integer: the smallest number of further cuts,
each at the worst instant, after which the area is stuck. It is the minimum of three counts
from the session's reading of the log, every word that does not read erased counting as
burnt, a faulting one included:

- the certificate words of L that read erased, when the next switch from L needs one (L is
  not certified, or O has no started position): that switch burns one per cut;
- when L is certified, the pair positions of O above its started ones: one per cut in a
  carry;
- when L holds no proven entry, the free positions of L, regular if it is open plus
  reserved: one per cut in a bury, after which step 1 finds no pair.

The first count is at most `X`, the others at most `P` + `R`, so outside a bad episode the
figure is the certificate count. It is back to `X` after a switch that returned, since the
new live unit's certificates are fresh; the application's own writes never move it, nor does
a cut outside the windows of the table above. The application applies the threshold it
wants: before a write, from the status call; at a boot, between the two phases of the scan.
What the figure cannot see is a burnt word that a cut left weak and that reads erased at this
boot: it is the word the next attempt programs, and zeros that return over it are solid, so
the figure is right for this session; T2 allows several such words to read programmed at a
later boot, so across sessions the figure is not monotonic in the strict model. A tally word
of zeros, programmed and returned before each erase of O, would make it a lower bound at
every boot (`X` minus the tallies reading programmed) for one word per switch and `X` words
per log unit, without changing any decision; it is not part of this proposal, pending a
measurement of whether a word the STM32U5A5 was cut in ever reads erased afterwards.

**Writes at a clean boot (f).** A boot after a session that was not cut programs nothing,
with one exception inherited from T2: a word left weak by an older cut that reads differently
than it did at the boot that followed the cut. A weak confirm of D reading all zeros then
erased, or a weak marker above D (or a weak carry marker in a certified O) reading erased
then programmed, makes the top not solid at that later boot: one bury or one carry, three
words, or, when L has no free position left, the switch the next update would have made (one
erase, one certificate, one carry). The decision does not change. With the two-phase scan
this write is a pending repair that phase 1 reports and phase 2 performs, not a silent one.

**Availability after a stuck-free cut.** The next boot programs at most one bury (three
words), or one switch: at most one reserved pair, one erase, one certificate and one carry,
plus the words it steps over, bounded by the positions of a unit; an update in the same
session may add a data-unit erase and its own commit.

## Costs on the STM32U5A5

- **Flash**: two log units per area (16 KB) plus `N` data units; two words per record (start
  marker and meta) and 48 bytes of log per update.
- **Update**: the record (two words plus the data) and three words of log, 21 word programs
  for 256-byte data (2.7 ms), 9 for 64-byte data (1.2 ms); one data-unit erase every `K`
  updates (`K` records per unit) and one log-unit erase every 152 updates of the area, each
  1.56 ms: the erase of a unit holding 8 programmed words or more, measured, sometimes
  1.79 ms; an erased unit takes 0.88 ms, and the time does not grow with the number of
  programmed words beyond that (`FLASH.md`, section 4, point 5).
- **Boot**: one pass over 16 KB of log and one record check in the read-only phase; the
  completion phase programs nothing after a clean session, but for residual (f), and nothing
  at all until the application calls it.
- **Wear**: with 256-byte data, `K` = 28 records per unit and `N` = 2: each data unit erases
  every 56 updates, each log unit every 304; the area outlives 560 k updates on 32 KB where a
  regular area of the same size allows 40 k.
- **Given up**: transactions (every update writes a whole record) and record repair: a dead
  record stays dead until its unit rolls over.

*Variant: the record inside the pair.* Marker, data, meta, confirm in the log unit itself,
no data units: two words fewer per update, two units fewer per area, a minimum area of 16 KB.
Its price: a bury or a carry copies the data; the reserved positions are data-sized (with
`R` = 8 they take 17 % of a unit at 128-byte data); and the "positions of O" reserve falls to
the positions of one unit (43 for 128-byte data, 25 for 256-byte). At equal area its wear
budget is 0.9 to 2.9 times the base's depending on the data size (table below), and only
through a ring of more than two log units. The two-phase scan, the remaining cuts figure
and the unproven read apply unchanged. Model checked with the base (`proto_v4p.py`, section
"Model check"); worth it for data of a few tens of bytes.

### Key figures

Both designs on the STM32U5A5: 16-byte word, 512 words per 8 KB unit, 128 us per word
program, 1.56 ms per erase of a programmed unit, the usual value measured (1.79 ms was seen
in one erase out of eight), and 0.88 ms of an erased one (`FLASH.md`),
10 k erase cycles per unit, `R` = 8, `X` = 32,
core at 160 MHz for the scan estimates. Nominal figures: a session that is cut burns the
positions and words it started, which the figures do not count. `w` is the data size in
words.

**Base proposal**, `N` data units and 2 log units, 4 units at least. A record is `w` + 2
words, `K` = 512 / (`w` + 2) records per data unit, `P` = 152 regular pairs per log unit; the
area supports 10 k x min(`N` x `K`, 2 x `P`) updates, "log" marking the cells where the log
factor 2 x `P` = 304 is the smaller one:

| data size | 4 B | 16 B | 64 B | 128 B |
|-----------|-----|------|------|-------|
| record, words | 3 | 3 | 6 | 10 |
| records per data unit `K` | 170 | 170 | 85 | 51 |
| updates, 4 units (`N` = 2, 32 KB) | 3.04 M, log | 3.04 M, log | 1.7 M | 1.02 M |
| updates, 8 units (`N` = 6, 64 KB) | 3.04 M, log | 3.04 M, log | 3.04 M, log | 3.04 M, log |
| updates, 16 units (`N` = 14, 128 KB) | 3.04 M, log | 3.04 M, log | 3.04 M, log | 3.04 M, log |
| cuts to stuck | 32 | 32 | 32 | 32 |
| next reserve, positions of O | 160 | 160 | 160 | 160 |
| scan, clean boot (estimate) | 0.14 ms | 0.15 ms | 0.18 ms | 0.22 ms |
| boot after a cut, worst, on top of the scan | 2.5 ms | 2.5 ms | 2.5 ms | 2.5 ms |
| update, typical | 0.77 ms, 6 words | 0.77 ms, 6 words | 1.15 ms, 9 words | 1.66 ms, 13 words |
| update, worst (rollover and switch) | 4.0 ms | 4.0 ms | 4.4 ms | 4.9 ms |
| flash programmed per update | 96 B | 96 B | 144 B | 208 B |

**Variant**, record in the pair, 2 log units at least. A pair is `w` + 3 words; a unit holds
floor(480 / (`w` + 3)) positions after the 32 certificate words, `P'` of them regular after
the 8 reserved ones; the area supports 10 k x `M` x `P'` updates with `M` log units. The
4- and 8-unit rows assume a ring of `M` units, generation g in unit g mod `M`, every switch
targeting the next unit: an extension that `proto_v4.py` does not cover (it asserts two log
units).

| data size | 4 B | 16 B | 64 B | 128 B |
|-----------|-----|------|------|-------|
| pair, words | 4 | 4 | 7 | 11 |
| regular positions per unit `P'` | 112 | 112 | 60 | 35 |
| updates, 2 units (16 KB) | 2.24 M | 2.24 M | 1.2 M | 0.7 M |
| updates, 4 units (32 KB, ring) | 4.48 M | 4.48 M | 2.4 M | 1.4 M |
| updates, 8 units (64 KB, ring) | 8.96 M | 8.96 M | 4.8 M | 2.8 M |
| cuts to stuck | 32 | 32 | 32 | 32 |
| next reserve, positions of O | 120 | 120 | 68 | 43 |
| scan, clean boot (estimate, 2 units) | 0.14 ms | 0.16 ms | 0.22 ms | 0.30 ms |
| boot after a cut, worst, on top of the scan | 2.7 ms | 2.7 ms | 3.5 ms | 4.5 ms |
| update, typical | 0.51 ms, 4 words | 0.51 ms, 4 words | 0.90 ms, 7 words | 1.41 ms, 11 words |
| update, worst (switch) | 2.2 ms | 2.2 ms | 2.6 ms | 3.1 ms |
| flash programmed per update | 64 B | 64 B | 112 B | 176 B |

How the lines are computed:

- *Cuts to stuck*: `X` consecutive switch attempts, each cut inside the certificate's program
  or the carry's marker (the 256 us window of the residual table); cuts anywhere else never
  lead to stuck. The next reserve is the "positions of O" line of that table, `P` + `R` or
  `P'` + `R`; the reserved pairs are the third. Every reserve stays above `X`, so `X` sizes
  the residual in every cell.
- *Scan, clean boot*: one memory-mapped pass over the log (16 KB, about 0.13 ms with the
  erased/zeros classification of every word), then the CRC-64 of two entries and of one record
  (base: data + 4 B) or of the newest position of each unit (variant: data + 6 B), with the
  bitwise CRC-64 of `ftl_internal.h` at about 0.6 us per byte. A ring adds 8 KB of reading
  per unit. Estimates, to be measured on the board.
- *Boot after a cut, worst*: base: a switch with step 1, one reserved pair, one erase, one
  certificate and one carry, 0.38 + 1.56 + 0.13 + 0.38 ms; a bury alone is 0.38 ms. Variant:
  the same with data-sized pairs, 2 x (`w` + 3) + 1 words and one erase; a bury alone is
  `w` + 3 words, 0.5 ms at 16 B and 1.4 ms at 128 B.
- *Update, typical*: `w` + 5 words in the base, `w` + 3 in the variant. The amortised erases,
  1.56 / `K` + 1.7 / `P` ms (data unit, log unit with its certificate), add under 0.05 ms
  everywhere.
- *Update, worst*: base: a rollover (one erase) and a switch (one erase, one certificate, the
  carry): `w` + 6 words + 3.12 ms. Variant: a switch, `w` + 4 words + 1.56 ms. Switch step 1
  adds a reserved pair, three words in the base and `w` + 3 in the variant, only when L holds
  no proven entry.

What the figures say:

- Below 64 B the base is log-limited at 3.04 M updates whatever `N`: the data units wear at a
  fraction of the log units' rate, and twice or four times the area buys nothing. The ring of
  log units that the variant's rows assume would lift it (8 units as 4 data + 4 log: 6.08 M);
  the other way is the variant. At 128 B, `N` = 6 balances the two (data 3.06 M, log 3.04 M).
- At equal area the variant gives 1.4 to 1.5 times the base's updates with 4 units, and with
  8 units from 0.9 (128 B, where the base's six data units are balanced) to 2.9 (4 B, where
  the base is log-limited). Its clear wins are the 16 KB minimum and the update time; its
  wear advantage needs the ring, which is not checked.
- Stuck is identical everywhere, 32 attempts, because `X` is the smallest reserve in every
  configuration; the variant's second reserve shrinks with the data size (43 at 128 B) but
  stays above `X`. `X` being an area parameter, each area buys its own count at three
  certificates per regular pair: `X` = 64 brings the base's log limit from 3.04 M to 2.82 M
  updates and the variant's 2-unit figures from 2.24 / 1.2 / 0.7 M to 2.08 / 1.12 / 0.64 M.
- The scan is flat and small. The base never reads a data unit beyond one record, so the boot
  time does not grow with the area; the variant's ring reads every unit.
- The 16-byte word makes 4 B and 16 B cost the same; at those sizes the three log words are
  half of a base update. For data of at most 6 bytes, arithmetic suggests a third shape, not
  designed here: the data inside the entry word with a CRC-32 (generation 2 B, version 4 B,
  data 6 B, CRC 4 B), three words per update and 152 positions per unit, 3.04 M updates on
  two units, at the price of a 32-bit check against foreign ghosts.

## Model check

`test/ewlf-model/ewlf_model.py` explores every weak-word reading, every cut point and every
number of updates per session, depth first over boots, against the model above (a word
programmed into weakly erased cells may read garbage for ever; a cut program over a weakly
erased word may read its old value again; the STM32U5A5 refusal rule, with a weak word
accepting or refusing a program at the checker's choice), and checks every shown version for
rollback, lost acknowledgement, loss, stuck, resurrection (`--strict`) and writes at a clean
boot (`--clean-boot`); `--weak-erased` hardens the model of a cut erase (below). `proto_v4.py` is this design, with the geometry `nd,rpu,nl,ppu,nres,nx`
(data units, records per unit, log units, regular pairs, reserved pairs, certificates); its
record data stands for the content the record checksum covers. Every run below used
`--strict`; "stuck ignored" rows pass `--ignore stuck` and explore past stuck states.
`proto_v4p.py` is the record-in-the-pair variant: the same module without data units, a
position being marker, data, meta, confirm, with the geometry `nl,ppu,nres,nx`; its results
are in the second table.

```
cd test/ewlf-model
python3 ewlf_model.py --protocol v4 --cuts 3 --boots 6 --geometry 2,2,2,2,2,2 --strict --ignore stuck
```

| geometry | cuts / boots | options | result |
|----------|--------------|---------|--------|
| 2,2,2,2,2,2 | 2 / 5 | stuck ignored | no violation, w5 acknowledged |
| 2,2,2,2,2,2 | 2 / 5 | stuck ignored, `--app 2` | no violation, w10 |
| 2,1,2,1,2,2 | 2 / 5 | stuck ignored, `--app 2` (a switch and a rollover at every update) | no violation, w10 |
| 2,2,2,2,3,3 | 2 / 6 | stuck counted | no violation, w6 |
| 2,2,2,2,2,2 | 3 / 6 | stuck ignored | no violation (31 k states, 4.1 M sessions, w6) |
| 2,2,2,2,2,2 | 3 / 6 | stuck ignored, `--app 2` | no violation (145 k states, 25.7 M sessions, w12) |
| 3,2,2,2,2,2 | 3 / 6 | stuck ignored | no violation (w6) |
| 2,2,2,2,4,4 | 3 / 8 | stuck counted | no violation (15.3 M sessions, w8): stuck needs more cuts than certificates |
| 2,2,2,2,4,4 | 4 / 9 | stuck counted | stuck after four cuts in the carry's marker, one per attempt: the residual, as sized |
| 2,2,2,1,1,1 | 3 / 7 | stuck ignored, `--clean-boot` | boot-write: a weak confirm reading programmed then erased, residual (f) |
| 2,2,2,2,2,2 | 4 / 8 | stuck ignored | no violation (592 k states, 315 M sessions, w8; 13 h wall clock on a loaded machine, 1.1 GB) |

The variant, `proto_v4p.py`, geometry `nl,ppu,nres,nx`, every run `--strict`:

| geometry | cuts / boots | options | result |
|----------|--------------|---------|--------|
| 2,2,2,2 | 2 / 5 | stuck ignored, `--app 2` | no violation (5.6 k states, 5.0 M sessions, w10) |
| 2,1,2,2 | 2 / 5 | stuck ignored, `--app 2` (a switch at every update) | no violation (w10) |
| 2,2,3,3 | 2 / 6 | stuck counted | no violation (w6) |
| 2,2,2,2 | 3 / 6 | stuck ignored | no violation (21.6 k states, 107 M sessions, w6; 53 min, 52 MB) |
| 2,1,1,1 | 3 / 7 | stuck ignored, `--clean-boot` | boot-write: residual (f), as for the base |
| 2,2,3,3 | 3 / 7 | stuck counted | stuck after three cuts in the carry's marker, one per attempt, every boot still showing D: the residual, as sized (`X` = 3) |
| 2,2,4,4 | 3 / 8 | stuck counted | no violation (48.9 k states, 401 M sessions, w8; 13 h, 122 MB): stuck needs more cuts than certificates |
| 2,2,2,2 | 3 / 6 | stuck ignored, `--app 2` | no violation (96 k states, 533 M sessions, w12; 5.6 h, 218 MB) |

The variant explores about 25 times the base's sessions at equal bounds (four words per
position instead of three, each a cut point and a weak reading), so the 4-cut runs of the
base were not repeated on it: its 4-cut, 8-boot run would take days. The two modules differ
only by the data word in the pair and the absence of data units; the argument is the same.

Negative controls (`V4_KNOCKOUT=` in the environment), 3 cuts, 6 boots, strict, stuck ignored,
geometry 2,2,2,2,2,2 unless stated: without switch step 1 (`step1`): **loss** at geometry
2,2,2,1,1,1; without the bury at boot (`bury`): **rollback**; without certificates (`cert`):
**resurrection**; without the erase of an empty certified O (`empty`): **resurrection**. Each
rule is load-bearing. The same four knock-outs on the variant (`proto_v4p.py`, geometry
2,2,2,2, or 2,1,1,1 for `step1`) give the same four violations.

The checker proves the absence of counterexamples within its bounds and under its model; the
argument above covers the rest. The cut format of the section below is not modelled.

**Physics the design relies on beyond the tearing page.** Two assumptions:

- a returned program of all zeros over a partially programmed word leaves solid zeros (the
  acceptance is measured, the solidity is an inference, still to be measured);
- a cut erase of a unit whose cells were already erased leaves them erased: only programmed
  words become weakly erased. The checker assumes it by default, and it is **load-bearing**:
  with `--weak-erased`, which lets a cut erase leave every word of the unit weakly erased,
  the checker finds a **loss after two cuts** (geometry 2,2,2,2,2,2, 2 cuts, 5 boots). The
  trace: a switch is cut in the carry's marker, after its certificate returned; the marker
  reads erased, so the next update erases O again, and that erase is cut; O's blank cells
  now read garbage here and there, which under a solid certificate looks like a carry
  attempt; the scan carries the decision above it, into weakly erased cells; that copy makes
  O the live unit, the next switch erases the old live unit, and the copy later reads
  garbage.

  **Measured on 2026-11-14 on a NUCLEO-U5A5ZJ-Q: the assumption held** (`FLASH.md`, section
  4, point 5; `test/boards/torn-erase.c`). The erase of an erased page was interrupted by a
  system reset at 20 instants from 0 to 874 us after its start, twice each: in all 40 cases
  every word of the page read erased at three readings, without any ECC flag, and all 512
  words then took a program and read back right. The control, the same sweep on a programmed
  page, shows that the reset does interrupt the erase: all 512 words read garbage, with ECC
  errors, from 46 us on, and the page reads erased from about 740 us. What the measurement
  does not cover: a loss of power in place of the reset (the high voltage may collapse
  differently), other parts and temperatures, and aged cells. If a later measurement says
  otherwise, the design needs one more rule, to be found with `--weak-erased` as the oracle:
  a unit whose last erase this boot cannot prove returned must not be written, whatever the
  certificates say. The simulator of the C tests is on the harsh side: a cut erase leaves
  every write unit of the unit weak, and it refuses a valued program over a weak unit, so in
  the C tests this trace ends in a stuck area, not in a loss.

## Implementation notes

- Read every log word once per session and decide from that reading; never re-read a
  possibly weak word (T2 allows two readings to differ). The derived per-unit state (started,
  newest valid entry, proven, closed, certified, first free certificate) fits in a few words.
- A read that faults (ECC double error, T5) counts as *programmed* for a marker, a confirm or
  a certificate, as *invalid* for an entry or a meta field, and makes its position *started*.
- Wait for each program or erase to complete before issuing the next: the chain proofs rest
  on that order. Readback is not a substitute.
- Never program a valued word over a word that does not read erased. A refused program of a
  marker, entry, confirm or certificate is stepped over, never retried on the same word in
  that session. The library tells a refused program from any other failure by the status of
  the write callback, `LFTL_NVM_STATUS_REFUSED` (PROGERR on the STM32U5A5: nothing was
  programmed); any other failure is fatal, as everywhere in the library.
- A read that faults is the read callback's to survive: it fills the write unit with a value
  that is neither all erased nor all zeros, which the rules above then count as started and
  as proving nothing.
- Only the live unit's certificates are read as certificates. They are read a second time,
  by the switch, only to pick one that reads erased to program: a reading that differs from
  the scan's costs a word and decides nothing. The switch checks that one is left before it
  erases the other unit, so that a stuck write erases nothing.
- An entry names its record on two bytes, data unit and position: at most 256 data units,
  and at most the first 256 record positions of a unit are used.
- A flash operation that fails, a power cut apart, makes the library forget what it derived:
  the next access scans again, as after a reset.
- The state phase 1 derives lives in the context from phase 1 to phase 2 and for the rest
  of the session: per unit the last started position, the newest valid entry and whether it
  is proven, closed, certified and the first free certificate; D; the pending repair; the
  remaining cuts figure. The update and the status call use it; nothing reads the log again.
- `X` comes from the context (a trailing field, zero for the build-time default), so that
  a test can register an area with `X` = 2 on the real geometry and reach stuck in two cuts.
- API: `lftl_ewlf_status` runs phase 1 if nothing did and returns the remaining cuts figure,
  the pending repair, D's version and whether the area is stuck; `lftl_ewlf_complete_scan`
  runs phase 2; `lftl_ewlf_read_unproven` reads D's record while a repair is pending or the
  area is stuck; `lftl_read` and the write functions raise `LFTL_ERROR_EWLF_PENDING` while a
  repair is pending and `LFTL_ERROR_EWLF_STUCK` when it has no room; a write that finds no
  reserve for its switch raises `LFTL_ERROR_EWLF_STUCK` and the area keeps showing D. After
  a reset the application sets the context's `data` to `LFTL_INVALID_POINTER`, as for a
  regular area.

## Format

`lftl_format` erases every unit of the area, whatever it holds, writes a record with version 1
at the top of the highest data unit, its data left erased, then its pair at the first
position of log unit 1 (generation 1); log unit 0 stays erased. It is the way out of a stuck
area, and the one call of the library that is not anti-tearing, for EWLF areas as for regular
ones: a cut in its erase leaves ghosts (T4) of whatever the area held, which the next boot
may show and a later boot may not find; an area whose format was cut must be formatted again
before use, and the application has to carry that fact itself (a mark outside the area). A
ghost of foreign content passes the checks with probability 2^-64 per entry.

`lftl_erase_all` is the anti-tearing way to give the content up: an ordinary update whose
record keeps its data erased, with the checksum of erased data in its meta field. The on-flash
format is v0.4.0; EWLF areas of earlier formats must be formatted again; regular areas are
unchanged.
