.. _design:

Anti-tearing and wear leveling
==============================

*lean-ftl* keeps, per area, one block of data that the application updates atomically: after a
power cut at any instant, the block reads as the last committed version or as the one before it,
never a mix of the two and never nothing. It also spreads the erases over the area, so that the
data can be updated many more times than one page endures. This page describes how both are done
for the three kinds of areas, regular, EWLF and large: the layout on the flash, how an update is written, how the first
access after a reset locates the current data (the *scan*), what a power cut can leave behind and
what the scan does about it, and what each update costs in wear. What the library relies on from
the flash is on the :ref:`flash` page, whose statements R1 to R8, N1 to N5 and T1 to T5 are cited
below; what an attacker can do with all this is in the
`threat model <https://github.com/sebastien-riou/lean-ftl/blob/main/THREAT-MODEL.md>`_ of the
repository, whose objectives O1 to O5 and threats T1.5 to T1.13 are cited too.

.. _design-common:

Common ground
-------------

**Versions and checksums.** Every copy of the data on the flash carries a 32-bit *version* and a
*checksum*: in a regular area a 32-bit CRC of the data plus the version (the CRC32C core with the
reciprocal form of its polynomial, which leaves its top 5 bits unused, ISSUES.md LFTL-014; the
format of regular areas is frozen), in an EWLF area the 64-bit CRC-64/NVME of the version then
the data. :c:func:`lftl_format` writes version 1; every update writes the current version plus
one. A copy is *valid* when its checksum matches its data and version. In a regular area the
valid copy with the highest version is the current one; in an EWLF area the current copy is the
one its log names, and the version only ties the two together. The version is not expected to
wrap: 4.3 billion updates exceed the endurance of any area.

The CRC-64 is computed four bits at a time through a table of 16 entries, 128 bytes of read-only
data. The table is read at an index that depends on the data: on a processor where the time of a
read depends on its address, a data cache, the time of the checksum depends on the data. A
library built with ``LFTL_CRC64_BITWISE`` defined computes the same CRC-64 one bit at a time,
without a table and in a time that depends on the length alone, at about four times the time per
byte. Both builds read and write the same areas. The CRC-32 of regular areas is bitwise.

**Meta-data of a regular area.** The version, the checksum and a third item, the *end marker*
``checksum2``, a second copy of the checksum, each occupy one write unit, 4 bytes at least (R2 of
the flash page): the value is in the first 4 bytes and the bytes next to it are programmed to zero
with it. A whole item therefore reads one of three ways: all erased, when it was never
programmed; a value, with some bits still erased when a power cut interrupted its program; or all
zeros. An update programs the data first, then the version and the checksum, then the end marker,
last and in its own write unit: a copy whose end marker is complete is complete. An EWLF area
keeps what decides apart from the data, in a log of its own: :ref:`design-ewlf`.

**What a power cut leaves.** Of all the words an update programs, only the one being programmed at
the instant of the cut can be left *weak*: the ones before it are complete, the ones after it
erased (T1 and T2 of the flash page). A weak word may read any value, differently from one read
to the next, and may later read all erased. A cut during the data, or during the version and the
checksum, leaves a copy whose checksum does not verify: the scan skips it and never trusts a word
of it. A cut during the word after the checksum leaves a copy whose checksum is complete. The one
case that needs care is a cut during the checksum word itself: the checksum may be weak, read as
valid at one boot and not at the next, and the only sign of it is a word after it that reads all
erased.

**The scan reads and chooses; it never erases.** The first access to an area after a reset locates
its current copy. The scan reads the flash and chooses. In a regular area what it programs is a
word of zeros, on a flash that allows it; in an EWLF area it programs nothing, and what it finds
to repair is done by a separate call, when the application asks. It never erases, so that a power
cut at every boot never costs an erase: wear stays what the application's own updates cost (T1.5
of the threat model). In a regular area the slot with the highest valid version is taken, and its
end marker, read once, decides the rest:

- complete, or all zeros: the slot is current. This is every boot in normal operation;
- programmed in part: the update got to its last word, so the checksum before it is complete and
  the slot is stable. It is current. On a flash with ``zero_overwrite`` the end marker is *pinned*
  first, so that it cannot read all erased at a later boot and bring the previous version back;
- all erased: the cut came during the checksum word at the latest, which may be weak. The slot is
  not taken: the previous version is current, and it is stable, since it was current when the
  interrupted update began. On a flash with ``zero_overwrite`` the slot not taken is *tombstoned*
  first: its version item is pinned to zeros, so that its checksum can never verify again and the
  slot cannot come back at a later boot as a "new" version once its end marker reads differently.

In an EWLF area the log decides, and a decision whose last word may be weak is not trusted in
place but copied above it before it is shown: :ref:`design-ewlf-scan`.

**Pinning.** To pin a write unit is to program it to all zeros, whatever it holds. Every cell gets
a full programming pulse, so a pinned unit reads zeros for good, and on flash with ECC it should
leave a valid codeword in place of the inconsistent one a torn program left (T1.13 of the threat
model says what remains to be measured there). Pinning needs a flash that accepts an all-zero
program over a partially programmed unit, N3 of the flash page: an opt-in capability, declared
per NVM with the ``zero_overwrite`` member of :c:type:`lftl_nvm_props_t`. The library never pins a
unit that already reads all zeros: the STM32L5 and STM32U5 refuse that program, measured on both.
Only regular areas pin; EWLF areas program nothing but write units that read erased. A pin or a
tombstone is one word program, made once, since the pinned word then reads zeros for good; a power
cut during it leaves a word closer to settled, decided again at the next boot. Without
``zero_overwrite`` the scan pins nothing and, in a regular area, makes the same choices again at
every boot, on the word as it reads then, until the next update settles them:
:ref:`design-without-pinning` says what that changes.

**Bounded time.** Every loop in the library runs over a quantity fixed by the configuration: the
number of slots, of pages, of records in a page, of pairs and certificates in a log unit. Each operation therefore has a worst case that
follows from the configuration alone (O5 of the threat model). Within that bound the scan's time
depends on what the flash holds; an update's does not, except where noted below.

.. _design-std:

Regular areas
-------------

Layout
^^^^^^

A regular area, declared with :c:macro:`LFTL_AREA`, is ``ns`` slots of ``np`` pages each: ``np``
is the smallest number of pages that holds the data plus the three meta-data items, and ``ns``,
the wear leveling factor, is at least 2. The data sits at the start of a slot and the meta-data at
its very end; the rest of the slot, between the two, stays erased. In this diagram and the
following ones a grey cell reads erased, and the numbers under the cells give the order in which an
update programs them:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="6"><tr><td border="0" colspan="5" align="left"><b>one slot</b>, np pages</td></tr><tr><td width="190" height="42">data<br/><font point-size="9">data_size bytes, whole write units</font></td><td width="120" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td width="72">version</td><td width="72">checksum</td><td width="72">checksum2</td></tr><tr><td border="0" align="left"><font point-size="9">slot base</font></td><td border="0"></td><td border="0" colspan="3"><font point-size="9">meta-data items, one write unit each, at the end of the slot</font></td></tr><tr><td border="0">1</td><td border="0"></td><td border="0">2</td><td border="0">2</td><td border="0">3</td></tr></table>>];
   }

An area is a row of such slots, each one starting on a page boundary and spanning whole pages:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="0" cellspacing="0" cellpadding="0"><tr><td colspan="7" align="left"><b>one area</b>, ns slots of np pages</td></tr><tr><td><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="4" align="left"><b>slot 0</b></td></tr><tr><td width="56">page 0</td><td width="28">...</td><td colspan="2" width="110">page np-1</td></tr><tr><td colspan="3">data</td><td width="66">meta-data</td></tr></table></td><td border="0" width="16"></td><td><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="4" align="left"><b>slot 1</b></td></tr><tr><td width="56">page 0</td><td width="28">...</td><td colspan="2" width="110">page np-1</td></tr><tr><td colspan="3">data</td><td width="66">meta-data</td></tr></table></td><td border="0" width="16"></td><td>...</td><td border="0" width="16"></td><td><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="4" align="left"><b>slot ns-1</b></td></tr><tr><td width="56">page 0</td><td width="28">...</td><td colspan="2" width="110">page np-1</td></tr><tr><td colspan="3">data</td><td width="66">meta-data</td></tr></table></td></tr><tr><td align="left"><font point-size="9">area base</font></td><td colspan="6"></td></tr></table>>];
   }

The data of a regular area is whole write units, which registration checks, so that a transaction
can commit it unit by unit.

Format
^^^^^^

:c:func:`lftl_format` erases the whole area and writes the meta-data of version 1 in slot 0, over
the erased data. It is not protected against a power cut: a cut during it leaves an area with no
valid version, which the application must detect with a flag of its own, kept in another area, and
format again (T1.6 of the threat model).

Update
^^^^^^

An update outside a transaction, :c:func:`lftl_write`, :c:func:`lftl_basic_write` or
:c:func:`lftl_erase_all`, goes to the *next slot*, the one after the current slot, wrapping from
the last slot to slot 0:

1. the next slot is erased, whatever it holds;
2. the new data is programmed into it: the bytes before and after the written range are copied
   from the current slot, the range itself comes from the caller's buffer, and a write unit that
   the range only partly covers is assembled in RAM from both. Every write unit is programmed once
   (R3). :c:func:`lftl_erase_all` programs nothing inside its range, which then reads as erased;
3. the version, the current one plus one, and the checksum are programmed in one call, then
   ``checksum2`` in its own write unit, last;
4. the context now points at the new slot.

An update costs one slot erase plus one slot's worth of programming, whatever the flash holds and
whatever a power cut left: it erases its target unconditionally, and the scan never leaves anything
in its way. That is the fixed cost O5 of the threat model promises, and the reason a counter
updated at every boot does not belong in a regular area.

Transactions
^^^^^^^^^^^^

A transaction updates several ranges of the data atomically, without a RAM copy of the whole data.
It uses a caller-owned bitmap, the *transaction tracker*, one bit per write unit of the data
(:c:macro:`LFTL_TRANSACTION_TRACKER_SIZE`):

- :c:func:`lftl_transaction_start` erases the next slot and clears the tracker;
- :c:func:`lftl_transaction_write` programs its range straight into the next slot, whole write
  units, and marks them in the tracker; a second write to a unit already marked is refused with
  :c:macro:`LFTL_ERROR_TRANSACTION_OVERWRITE`, since a unit is programmed once between two erases.
  :c:func:`lftl_transaction_write_any` accepts unaligned ranges and assembles the partial units
  from the current data;
- :c:func:`lftl_transaction_commit` copies every unit the tracker does not mark from the current
  slot, then programs the meta-data as an update does. The new slot is current from its end marker
  on;
- :c:func:`lftl_transaction_abort` forgets the tracker. The next slot stays as it is, erased or
  partly programmed, and the next update erases it again.

Until the commit, :c:func:`lftl_read` returns the current, committed data; :c:func:`lftl_read_newer`
and :c:func:`lftl_transaction_read` return the new value of the units written so far and the
current one elsewhere. An error raised inside a transaction aborts it before the error handler
runs. Each start costs a slot erase, committed or not.

.. _design-std-scan:

Scan
^^^^

The first access after a reset runs the scan of the area, once:

1. every slot's version is read. A slot whose version reads all ones, never programmed, is
   skipped, and so is a slot whose checksum does not match its data and version, whatever its
   version word reads: a slot whose erase was interrupted can read as any version, the current one
   included. The highest valid version wins. Two valid slots with the same version is
   :c:macro:`LFTL_ERROR_VERSION_COLLISION`; no valid slot at all is
   :c:macro:`LFTL_ERROR_NO_VALID_VERSION`, the state of an area never formatted, or whose format
   was interrupted;
2. the winner's ``checksum2`` unit is read, once, and decides as in :ref:`design-common`:

   - equal to the checksum, or all zeros: the slot is current;
   - programmed in part: the slot is current; with ``zero_overwrite`` its ``checksum2`` unit is
     pinned first. A unit that reads all zeros is one that was pinned, or a checksum that happens
     to be 0 written in full, and is never pinned again; the tombstone below uses the version
     item, which is never 0 since format writes 1, so no checksum value can be mistaken for one;
   - all erased, and another slot is valid: the previous version is current. With
     ``zero_overwrite`` the torn slot is tombstoned, its version item pinned to zeros; without it
     the slot is left as it is. The scan then runs again, leaving that slot out;
   - all erased, and no other slot is valid: the slot is taken as it is. Nothing in normal
     operation produces that state, and taking it beats having nothing.

The scan never erases, and sets the context from the index of the slot it chose, never from a
value read on the flash. The torn slot, when there is one, is the slot after the current one: it
is the target of the next update, which erases it.

Wear leveling
^^^^^^^^^^^^^

Updates use the slots in turn, so each page of the area is erased once per ``ns`` updates: the
area endures ``ns`` times what one page endures, and ``ns`` is the wear leveling factor given to
:c:macro:`LFTL_AREA`. The price is ``ns`` times the pages the data needs, one slot per unit of the
factor, even when the data is a few bytes. That is what EWLF areas are for.

.. _design-ewlf:

EWLF areas
----------

An EWLF area, for Extreme Wear Leveling Factor, declared with :c:macro:`LFTL_AREA_EWLF` and
registered with :c:func:`lftl_register_area_ewlf`, packs many copies of the data, the *records*,
into each erase unit, and keeps apart, in a *log*, the word that says which record is current. An
erase is paid once per unit's worth of updates instead of once per update. It is meant for data
small compared to the page, and it gives up transactions: every update writes a whole record.

Its anti-tearing argument has no residual against the data: after any sequence of power cuts, at
any instant, a boot shows the last version an update acknowledged or a newer one, a version shown
at one boot is never taken back at a later one, a write that was never acknowledged is never shown
after an older version was, and the data is never lost. What remains is a loss of availability,
the *stuck* state of :ref:`design-stuck-state`, reached only by a run of power cuts each inside
the same short window, and which the area reports before it happens.

The area is at least 4 erase units, 2 for the log and from 2 to 256 for the data; at least 2
records must fit in a unit; the erased bytes of the flash must read ``0xFF`` (R6 of the flash
page); and the write callback must tell a program the flash refused from any other failure
(:c:macro:`LFTL_NVM_STATUS_REFUSED`, N3). EWLF areas do not use ``zero_overwrite``: they only
program write units that read erased.

**The principle: a reading proves nothing, a returned program does.** A word a power cut left
weak may read valid today and erased tomorrow (T2 of the flash page, without T3). So nothing a
later boot could contradict is decided on a reading: what is shown, what is erased and the order
of the versions rest only on programs that *returned*, in the current session or, for earlier
ones, through a *chain proof*: when word B is programmed only after the program of word A
returned, B reading programmed proves that A is solid and reads its value for good. The last word
of a chain is never proven, and the design is arranged so that nothing depends on it.

Layout
^^^^^^

The data units come first, then the two log units:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="0" cellspacing="0" cellpadding="0"><tr><td colspan="9" align="left"><b>one area</b>, N data units and 2 log units</td></tr><tr><td><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td width="90" height="36">D0<br/><font point-size="9">records</font></td></tr></table></td><td border="0" width="16"></td><td>...</td><td border="0" width="16"></td><td><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td width="90" height="36">D(N-1)<br/><font point-size="9">records</font></td></tr></table></td><td border="0" width="16"></td><td><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td width="90" height="36">L0<br/><font point-size="9">pairs</font></td></tr></table></td><td border="0" width="16"></td><td><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td width="90" height="36">L1<br/><font point-size="9">pairs</font></td></tr></table></td></tr><tr><td align="left"><font point-size="9">area base</font></td><td colspan="8"></td></tr></table>>];
   }

A *record* is a start marker, the data, then a meta field; the second row gives the size of each
field, the last one the order in which an update programs them:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="6"><tr><td border="0" colspan="3" align="left"><b>one record</b></td></tr><tr><td width="90" height="42">start marker<br/><font point-size="9">all zeros</font></td><td width="190" height="42">data<br/><font point-size="9">data_size bytes</font></td><td width="170" height="42">meta field<br/><font point-size="9">32-bit version, 64-bit checksum</font></td></tr><tr><td border="0"><font point-size="9">one write unit</font></td><td border="0"><font point-size="9">rounded up to whole write units</font></td><td border="0"><font point-size="9">12 B, rounded up to whole write units</font></td></tr><tr><td border="0">1</td><td border="0">2</td><td border="0">3</td></tr></table>>];
   }

- the *start marker*, one write unit programmed to all zeros, first: a position is *started* once
  its start marker does not read erased, and nothing after an erased start marker was ever
  programmed;
- the data, rounded up to whole write units;
- the *meta field*: the 32-bit version then the CRC-64/NVME of the version and the data, little
  endian, 12 bytes rounded up to whole write units with zeros, programmed in one call.

A record has no flag: whether it is current is the log's business. Records only ever start at
*positions*: the top of a data unit, which is the unit's end minus one record size, and every
record size below it, 256 positions per unit at most. Positions fill a unit from its top down, and
the data units are used from the highest address down, wrapping from the lowest back to the
highest. A record whose words are weak is never repaired: no entry names it once a boot has run,
and it stays dead in place until its unit is erased.

The log holds *pairs*. A pair is three words, programmed in this order, each only after the
previous one returned:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="6"><tr><td border="0" colspan="3" align="left"><b>one pair</b></td></tr><tr><td width="90" height="42">marker<br/><font point-size="9">all zeros</font></td><td width="290" height="42">entry<br/><font point-size="9">generation, data unit, position, version, CRC</font></td><td width="90" height="42">confirm<br/><font point-size="9">all zeros</font></td></tr><tr><td border="0"><font point-size="9">one write unit</font></td><td border="0"><font point-size="9">16 B, rounded up to whole write units</font></td><td border="0"><font point-size="9">one write unit</font></td></tr><tr><td border="0">1</td><td border="0">2</td><td border="0">3</td></tr></table>>];
   }

- the *marker* first, so that a position whose entry may be weak is visibly started at every
  boot: a word of zeros never reads as anything but zeros, in part or in full, or erased;
- the *entry* names a record: the 16-bit generation of its log unit, the data unit and the
  position of the record, one byte each, the record's 32-bit version, then the CRC-64/NVME of
  those 8 bytes, little endian. An entry is *valid* when its CRC verifies, its generation has the
  parity of its log unit, and it names a position that exists;
- the *confirm*, after the entry returned: a confirm that reads all zeros proves the entry solid,
  by the chain proof, and the entry is then *proven*. All zeros, not any programmed bit: on flash
  with ECC an erased line with two flipped bits goes undetected (T5), so a few programmed bits in
  a word that should be erased prove nothing.

A log unit holds, from its base up, ``P`` regular pair positions, ``R`` reserved pair positions,
then ``X`` *certificates*, one write unit each, programmed to all zeros:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="6"><tr><td border="0" colspan="9" align="left"><b>one log unit</b></td></tr><tr><td width="64" height="36">pair 0</td><td width="28">...</td><td width="64">pair P-1</td><td width="64" bgcolor="#dbeafe">pair P</td><td width="28" bgcolor="#dbeafe">...</td><td width="72" bgcolor="#dbeafe">pair P+R-1</td><td width="56" bgcolor="#fef3c7">cert 0</td><td width="28" bgcolor="#fef3c7">...</td><td width="64" bgcolor="#fef3c7">cert X-1</td></tr><tr><td border="0" colspan="3"><font point-size="9">regular: the application's updates</font></td><td border="0" colspan="3"><font point-size="9">reserved: the repairs and the switches</font></td><td border="0" colspan="3"><font point-size="9">certificates</font></td></tr><tr><td border="0" align="left" colspan="9"><font point-size="9">unit base</font></td></tr></table>>];
   }

- generation ``g`` of the log lives in log unit ``g mod 2``. Generations are compared as
  successors modulo 2\ :sup:`16`: the two units always hold adjacent generations;
- regular pairs take the application's updates. Reserved pairs are never taken by an update's
  ordinary commit: they serve the scan's repairs and the switches, so that a full unit still has
  room to make a decision solid;
- a certificate of a log unit, once it reads all zeros, means: *the erase of the other log unit
  returned in the session that programmed this word, and content of the next generation may have
  entered the other unit since*. An interrupted erase cannot be seen from inside the erased unit
  (T4), so that information is kept outside it.

``R`` is :c:macro:`LFTL_EWLF_RESERVED_PAIRS`, 8, a build-time constant. ``X`` is a parameter of
the area, the ``ewlf_certificates`` member of :c:type:`lftl_ctx_t`, with
:c:macro:`LFTL_EWLF_CERTIFICATES`, 32, for the areas that leave it at zero. Both are part of the
on-flash format of the area, like the data size: nothing on the flash records them, and an area
whose value changes must be formatted again. ``P`` follows: what the certificates and the
reserved pairs leave. On the STM32U5, 16-byte write units and 8 KB erase units, a pair is 48
bytes and a log unit holds 152 regular pairs with the defaults; 3 certificates cost one regular
pair.

Format
^^^^^^

:c:func:`lftl_format` erases every unit of the area, writes the record of version 1 at the top
of the highest data unit, its data left erased, then the pair that names it at the first position
of log unit 1, generation 1. Log unit 0 stays erased. It is not protected against a power cut, as
for regular areas: a cut in its erase leaves cells that a later boot may read differently (T4),
so an area whose format was cut must be formatted again before use.

.. _design-ewlf-scan:

Scan
^^^^

The scan reads; it programs and erases nothing. It runs once, at the first call that needs the
area after a reset: :c:func:`lftl_scan_status`, :c:func:`lftl_complete_scan`,
:c:func:`lftl_read_unproven`, a read or a write. Registration reads nothing: an area is
registered before it is formatted. Every word of both log units is read once, and from that one
reading:

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Name
     - Definition
   * - started position
     - a pair position where any byte reads programmed
   * - valid entry
     - its CRC verifies, its generation has the parity of the unit, the position it names exists
   * - proven entry
     - valid, and its confirm reads all zeros
   * - closed unit
     - a reserved position is started, or a certificate reads all zeros: no ordinary commit goes
       to it any more
   * - certified unit
     - one of its certificates reads all zeros
   * - live unit ``L``
     - the unit of the higher generation among those holding a valid entry; ``O`` is the other
   * - decision ``D``
     - the newest valid entry of ``L``: the one at the highest position
   * - ``O`` is uncertain
     - ``L`` is certified, ``O`` has a started position and no valid entry: a first entry of the
       next generation, possibly weak, may be there
   * - the top is solid
     - ``D`` is proven, nothing is started above it in ``L``, and ``O`` is not uncertain

The certificates of ``O`` are never read as certificates: they belong to its previous life.
Without a certificate of ``L`` nothing entered ``O`` since its erase, and what ``O`` reads is not
consulted. ``D`` is the newest *valid* entry, not the newest proven one, on purpose: a weak
confirm that read all zeros at one boot may have shown that entry, and a later boot that reads it
erased must not fall back to the entry before it.

Then:

1. no valid entry in either unit: :c:macro:`LFTL_ERROR_NO_VALID_VERSION`, the state of an area
   never formatted or whose format was interrupted;
2. the record ``D`` names is checked: its meta field holds ``D``'s version and its checksum
   verifies. It always does for an entry the library wrote; otherwise the area has no valid
   version;
3. when the top is solid, ``D``'s record is the current one and the area is readable and
   writable. This is every boot after a session that was not cut, with one exception, below;
4. otherwise a *repair* is pending, and nothing is shown until it is done.

**The repair** is :c:func:`lftl_complete_scan`, a separate call, so that the application
decides when the flash is written. It *buries* ``D``: it programs a copy of ``D`` above every
started position, by the first of these that applies:

- when ``O`` is uncertain, a pair in ``O`` above its started positions, of the next generation,
  without erasing: a copy in ``L`` would leave a possible entry of the next generation above it;
- a regular pair of ``L``, when ``L`` is open and has one;
- a reserved pair of ``L``;
- a log switch with ``D`` as its first entry, below.

The copy's three programs return before anything is shown: ``D`` is then proven and nothing a
power cut left can outrank it at a later boot. The repair decides from the readings of the scan
and reads the log no more. When no repair is pending the call does nothing, and an application
may call it unconditionally after a reset.

**While a repair is pending**, :c:func:`lftl_read` and the write functions raise
:c:macro:`LFTL_ERROR_SCAN_PENDING` on the area, so that no version that is not vouched for reaches
the normal path. Two calls work in that state:

- :c:func:`lftl_scan_status` gives the pending repair, ``D``'s version, and the number of further
  power cuts, each at the worst instant, that the area can take before it is stuck
  (:ref:`design-stuck-state`). An application applies the threshold it wants before it calls the
  repair, for instance to ask the user for a stable power supply first;
- :c:func:`lftl_read_unproven` reads ``D``'s record. The record itself is solid: its programs
  returned before its entry was started, and its unit is not erased while an entry can name it.
  Within the session it is exactly the version the repair makes current. What is not proven is
  that a later boot chooses the same one, if the session ends before the repair: the other
  candidate is the version written just before or just after it. An application that uses this
  read to decide whether to go on must keep the state of that decision, a retry counter for
  instance, in another area: this one takes no write before the repair.

**A write at a boot that follows no cut.** A boot after a session that was not cut finds nothing
to repair, with one exception inherited from T2: a word that an older cut left weak and that
reads differently than it did at the boot that followed that cut. A weak confirm of ``D`` that
read all zeros then reads erased, or a weak marker above ``D`` that read erased then reads
programmed, makes the top not solid at that later boot: one repair, three words in most cases.
The decision does not change, and the repair is reported like any other.

Update
^^^^^^

An update, :c:func:`lftl_write`, :c:func:`lftl_basic_write` or :c:func:`lftl_erase_all`, writes a
whole new record, then the pair that makes it current:

1. room for the record: the first position of the current record's unit above the last started
   one. A position whose start marker the flash refuses to program, a weak marker that reads
   erased at that instant, counts as started and is stepped over;
2. when the unit has no position left, the *rollover*: the next data unit, never the one of the
   current record, is erased, whatever it reads, right before its first record (T4);
3. the record: the start marker, the data, the bytes outside the written range copied from the
   current record, then the meta field. :c:func:`lftl_erase_all` leaves the data erased. The
   checksum is computed from the data meant to be written, so a record that did not program as
   intended fails its check;
4. the *commit*: a pair at the first regular position of ``L`` above its started ones, when
   ``L`` is open and has one; otherwise a log switch with the new entry as its first entry. A
   position where the flash refuses the marker, the entry or the confirm counts as started and
   the next one is tried: a word is never retried, and nothing valued is programmed over a word
   that was refused;
5. the context points at the new record once the confirm returned: only then is the update
   acknowledged.

A power cut before the confirm returned leaves the next boot free to make either version current:
the previous one if the new entry does not read valid then, the new one if it does. Whichever it
is, that boot buries it before showing it, unless nothing above it is started, and the choice is
never revisited. A cut after the confirm returned leaves the new version current.

Log switch
^^^^^^^^^^

When the live unit ``L``, of generation ``g``, has no position left for an entry, the session
that needs one, an update or a repair, opens generation ``g+1`` in ``O``, with that entry as its
first one:

1. when no certificate of ``L`` reads all zeros and ``L`` holds no proven entry: a reserved pair
   of ``L`` copying ``D`` first, so that a solid decision exists outside ``O`` before ``O`` is
   erased. The previous life of ``O`` may hold the only proven one;
2. when no certificate of ``L`` reads all zeros, or ``O`` has no started position: ``O`` is
   erased, whatever it reads, then a fresh certificate of ``L`` is programmed, both seen
   returning in this session;
3. the entry goes to the first position of ``O`` above its started ones.

Content thus enters a log unit only after an erase that returned in the same session and a
certificate that session saw return, or above a word that entered that way. Without the
certificate a boot could not tell a first entry of generation ``g+1``, possibly weak, from what an
interrupted erase left of the previous life of ``O``, and would have to choose between erasing
``O``, where an interrupted erase lets that weak entry come back later above the live content,
and writing into ``O``, into cells an interrupted erase may have left weak. The second case of
step 2 covers a certificate that reads all zeros but is weak: the session cannot know, and an
erase and a certificate it sees return cost one erase of a unit that holds nothing.

Why there is no rollback and no loss
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

1. *What is shown is solid.* ``D`` is proven, or was copied by three programs that returned in
   this session, and it is shown after the last of them. A solid entry reads valid at every later
   boot until its unit is erased, and its record was whole before the entry was started.
2. *The order is the generation and the position, never a reading.* An entry is written only
   above every started position of its unit. Nothing can outrank a ``D`` that was shown:
   everything started above it at the time was buried under a solid copy of it, and later entries
   are newer by construction. This is also why a write that was cut is never shown after an older
   version was.
3. *What an interrupted erase leaves is neither elected nor written into.* A log unit whose
   certificate does not read all zeros is erased again before use, which is safe since nothing
   entered it; what it holds of its previous life is of a lower generation than the live one.
4. *Erasing a log unit destroys nothing elected.* ``O`` is erased only when nothing entered it,
   and then only once ``L`` holds a proven entry, or when it has no started position.
5. *Erasing a data unit destroys nothing elected.* The unit of the current record is never
   erased, and a data unit is erased only in a session whose scan left ``D`` solid above
   everything started, so every entry whose record goes is below a solid ``D``.

What the next boot makes of a cut, by the word it lands in:

.. list-table::
   :header-rows: 1
   :widths: 30 34 36

   * - The cut lands in
     - The next boot sees
     - Outcome
   * - the record: start marker, data or meta field
     - a started record position that no entry names
     - the previous version; the position is stepped over
   * - the pair's marker, or the entry, which reads invalid
     - a started position above ``D``: a repair
     - the previous version stands, solid
   * - the entry, which reads valid
     - a ``D`` that is not proven: a repair
     - the new version stands, never taken back
   * - the confirm
     - the entry is solid: shown when the confirm reads all zeros, else a repair
     - the new version stands
   * - a repair or a switch's first entry
     - the same state one position higher
     - repeated until one returns
   * - the erase of a data unit
     - what is left of its records, which no entry names
     - erased again before its first record
   * - the erase of ``O``
     - what is left of its previous life, of a lower generation
     - erased again before use
   * - a certificate, or the entry right after it
     - ``O`` erased and empty, or started under a certificate
     - erased again with a new certificate, or the entry above the started position

Wear leveling
^^^^^^^^^^^^^

Each data unit is erased once per ``N`` x ``K`` updates, ``K`` records per unit and ``N`` data
units, and each log unit once per 2 x ``P`` updates. The wear leveling factor of the area is the
smaller of the two, and :c:macro:`LFTL_AREA_EWLF` sizes the data units so that ``N`` x ``K``
reaches the factor asked for. For small data the log is the limit: on the STM32U5 with the
defaults, 2 x ``P`` is 304, about 3 million updates at 10 000 erase cycles, whatever the number
of data units. The old records of a unit stay physically readable until the write order comes
back to it (T1.8 of the threat model); nothing reads them through the API.

What an update and a boot cost on the STM32U5, 128 us per write unit programmed, an erase of
1.56 ms for a unit that holds data (``FLASH.md``, section 4):

.. list-table::
   :header-rows: 1
   :widths: 40 15 15 15 15

   * - Data size
     - 4 B
     - 16 B
     - 64 B
     - 128 B
   * - Records per data unit
     - 170
     - 170
     - 85
     - 51
   * - Updates on 4 erase units, 10 000 cycles
     - 3.04 M
     - 3.04 M
     - 1.7 M
     - 1.02 M
   * - Update, usual: the record and its pair
     - 0.77 ms
     - 0.77 ms
     - 1.15 ms
     - 1.66 ms
   * - Update, worst: a rollover and a log switch
     - 4.0 ms
     - 4.0 ms
     - 4.4 ms
     - 4.9 ms
   * - Repair, worst: a log switch
     - 2.5 ms
     - 2.5 ms
     - 2.5 ms
     - 2.5 ms

The scan reads the two log units, 16 KB, and checks one record, whatever the size of the area: a
fraction of a millisecond, estimated, not measured.

Limitations
^^^^^^^^^^^

- one area holds one data of fixed size, and every update writes all of it;
- no transactions: :c:func:`lftl_transaction_start` raises
  :c:macro:`LFTL_ERROR_EWLF_TRANSACTION_UNSUPPORTED`;
- two erase units of every area are its log, and the area needs two data units at least;
- after a reset, an application that reads or writes the area without calling
  :c:func:`lftl_complete_scan` first gets :c:macro:`LFTL_ERROR_SCAN_PENDING` when the last
  session was cut;
- the update that rolls over or switches costs one or two erases and the others none, so the
  update time is not uniform;
- the design rests on two properties of the flash beyond the tearing model, in
  :ref:`design-ewlf-checked`.

A possible extension, not implemented: for data of a few tens of bytes the record could live in
the pair itself, without data units, which halves the smallest area. Another: an EWLF area could
serve as the journal of transactions that span several areas.

.. _design-large:

Large areas
-----------

A large area, declared with :c:macro:`LFTL_AREA_LARGE` and registered with
:c:func:`lftl_register_area_large`, is meant for data of several erase units. A regular area and
an EWLF area rewrite the whole data at every update; a large area cuts it into *chunks* of one
erase unit each, keeps them in a *pool* of erase units, and rewrites only the chunks an update
touches. A *map* says which pool unit holds each chunk; the map is replaced atomically by the log
of an EWLF area, whose pairs hold it. It keeps:

- the guarantee of EWLF areas: after any sequence of power cuts at any instant, a boot shows the
  data the last acknowledged update left or a newer one, never takes back data it showed, never
  shows an update that was never acknowledged after older data was shown, and loses nothing. An
  update is atomic whatever the number of chunks it touches; the stuck state of
  :ref:`design-stuck-state` is the one residual, and it is the log's;
- the transactions of regular areas, with the same calls;
- a checksum of the whole data, checked once per reset.

The pool must hold the chunks and one more unit, 65535 units at most, and a log unit must hold
its certificates, its reserved pairs and 2 regular pairs of the map. The flash must meet what an
EWLF area needs: R6 and R9 of the flash page.

Layout
^^^^^^

.. code-block:: text

   area = U pool units, then 2 log units

   +---------+---------+-------+---------+---------+---------+
   |   P0    |   P1    |  ...  | P(U-1)  |   L0    |   L1    |
   |  chunk  |  chunk  |       |  chunk  |  pairs  |  pairs  |
   +---------+---------+-------+---------+---------+---------+

   pair = marker | map | meta | confirm
   map  = the unit of each chunk (16 bits each) | head unit (16 bits) | CRC-64 of the data
   meta = generation (16 bits) | 0 (16 bits) | version (32 bits) | CRC-64 of the map and meta

A pool unit holds one chunk, which fills it: no marker, no flag, no checksum, and nothing says
which chunk it is; only the map says. The last chunk may hold less than a unit. A map entry of
``0xFFFF`` is a chunk never written, which reads erased. The *head* is the pool unit the update
that wrote the map filled last. The log units are those of an EWLF area, pairs then certificates;
a pair is the marker, the map, the meta field and the confirm, each programmed only after the one
before it returned, so that a meta field that reads valid proves its map solid. The map of 64 KB
on the STM32U5 is 26 bytes, its pair 80 bytes; that of 1 MB is 266 bytes, its pair 320 bytes.

Update
^^^^^^

A plain write covers a run of consecutive chunks:

1. **room**: one free pool unit per chunk the write touches, counted before anything is
   programmed. When the pool has not that many, :c:macro:`LFTL_ERROR_LARGE_NO_SPACE` is raised
   and nothing changes;
2. **the chunks**, in increasing order, each in the next free unit, erased right before: the bytes
   written from the caller, the others from the chunk's current copy, or erased when it has none;
3. **the commit**: a pair whose map names the new units for the chunks written and the old ones
   for the others, the head, and the checksum of the new data, appended to the log as an EWLF area
   appends an entry, with a log switch when the live unit is full.

Every chunk's program returned before the pair was started, so a map that the log names, names
only whole chunks. The map is streamed into the pair, two bytes per chunk, with no copy of it in
RAM: the units of the run are found again as they were taken, the free units after the head in
ring order. :c:func:`lftl_erase_all` is an update whose map names no unit.

The pool
^^^^^^^^

A pool unit is *free* when it holds no chunk of the current map and none of the open transaction;
whatever a free unit holds is garbage: an older copy of a chunk, or what a power cut or an
aborted transaction left. There is no third state: a unit holds a whole chunk or nothing of value,
so nothing is ever copied to make room and there is no garbage collection.

**The head goes round the pool.** For each chunk to write the head moves to the next free unit
after it, in increasing order, wrapping around, the head itself last. It steps over the units that
hold a current chunk, which are left alone.

**A unit is erased when the head enters it, whatever it reads.** A free unit that reads erased may
hold the remains of an erase a power cut interrupted (T4); it is erased again before its chunk is
programmed. Nothing is erased anywhere else, and nothing while a repair of the scan is pending;
the erase is guarded as the rollover of an EWLF area is.

**Wear.** The free units take the writes in turn, so the erases spread over every unit but those
whose chunk never changes, which are never erased nor copied. ``W`` chunk writes over the life of
the area need ``W`` / endurance units on top of the chunks that are not rewritten. A power cut
burns nothing that lasts: the units an interrupted update wrote are free, and are erased again
when the head reaches them.

The checksum of the data
^^^^^^^^^^^^^^^^^^^^^^^^

The map carries the CRC-64/NVME of the whole data, so that the CRC-64 of the pair covers the data
too, through the map. It is not computed again over the whole data at every update: a CRC is
linear, so replacing a run of chunks changes it by the CRC of the old content xor that of the new
one, shifted by the bytes of data after the run, a multiplication by a power of ``x`` modulo the
polynomial, of bounded time. An update reads the old content of the chunks it replaces; an update
that replaces every chunk computes the checksum afresh.

It plays no part in the anti-tearing argument: it is there for what the flash does to the data
after it was written. It is checked once per reset and never by the scan, whose time does not
depend on the size of the data: by :c:func:`lftl_check_data` when the application calls it, or
otherwise before the first call that uses the data the area holds. A mismatch is an error,
:c:macro:`LFTL_ERROR_DATA_CHECKSUM`, never a fallback to older data, which a large area does not
have; :c:func:`lftl_read_unchecked` reads the data whatever its checksum, and
:c:func:`lftl_repair_checksum` commits the checksum of the data as it reads. A write of the whole
data, or of all of its chunks, and an erase of all of it, need no check.

Transactions
^^^^^^^^^^^^

The calls and the rules are those of regular areas. :c:func:`lftl_transaction_start` erases
nothing; the tracker holds, after the bit of each write unit, the pool unit of each chunk. A
transaction write takes a free unit for each chunk it touches for the first time and programs its
write units straight there. The commit completes each chunk touched from its current copy, then
commits the map as an update does. An abort forgets the tracker: the units the transaction took
are free again. A transaction holds the old and the new copy of every chunk it touches until its
commit, so its size is bounded by the free units of the pool.

Scan
^^^^

The scan is that of an EWLF area: both log units read once, the decision is the newest valid pair
of the live unit, a pair being valid when the CRC of its meta field verifies over the map and the
meta field and every unit the map names exists. The pool is not read. The repair, when the scan
leaves one pending, copies a map, never data: the chunks of the map the decision names are intact,
since nothing is erased while a repair is pending.

Why there is no rollback, no loss and no mixed content
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

1. *The map is the data of an EWLF pair.* What is shown is a map that a proven pair holds; the
   order of maps is that of their pairs; a map once shown is never outranked. This is the
   argument of EWLF areas, which does not depend on what the data of a pair is.
2. *A map names only whole chunks*, each programmed into a unit whose erase returned in the same
   session, before the pair was started.
3. *No erase destroys a chunk a boot can show.* A pool unit is erased only when the head enters
   it, in a session where the area is ready: the current map is final, and the unit holds none of
   its chunks, nor of the map being written. After a cut the two candidates are the map that was
   current and the one being committed, and nothing is erased until a boot made one of them final.
4. *The content is never mixed*: a single pair replaces the map.

A cut in a chunk, or in the erase of a pool unit, leaves a unit no map names: the previous data,
and the unit is free. A cut in the pair is a cut in the log, handled as in an EWLF area.

Costs
^^^^^

On the STM32U5A5, for data of 64 KB less 48 bytes, the best case of a regular area, estimates
(``test/ewlf-model/chunked_sim.py --large``):

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * -
     - Regular area
     - Large area
   * - Update of the whole data
     - 8 units, 556 ms
     - 8 units and an 80-byte pair, 551 ms
   * - Update of a few bytes
     - 8 units, 556 ms
     - 1 unit, 71 ms
   * - Smallest area, a transaction of the whole data allowed
     - 128 KB
     - 144 KB
   * - Area for 500 000 updates of the whole data
     - 3.12 MB
     - 3.14 MB
   * - Area for 500 000 updates of one unit or less
     - 3.12 MB
     - 480 KB
   * - Updates the log endures
     - 10 000 per slot
     - 1.76 million, fewer as the map grows: 320 000 for 1 MB
   * - First access after a reset
     - the checksum of half the slots on average
     - 16 KB of log; the check of the data apart, 13 ms
   * - After a power cut
     - the old or the new version; a rollback by one version in the cases of
       :ref:`design-residual`
     - the old or the new data, never taken back

An update of the whole data takes the same time within 1 %: the pair costs 10 ms, and the checksum
of the data 6 ms less than the CRC-32 of a regular area. With the bitwise CRC-64 of
``LFTL_CRC64_BITWISE`` (see :ref:`design-common`) the three times of a large area are 580 ms,
78 ms and 43 ms. The log limits the commits: with its
two units, 500 000 commits hold up to 83 chunks, 664 KB on the STM32U5.

.. _design-without-pinning:

Without pinning
---------------

On a flash not declared with ``zero_overwrite`` (N3 of the flash page: the STM32L5 and STM32U5
document the all-zero overwrite, the W25Q32RV specifies programming for erased locations only, the
M5531 and CH32V307 manuals do not say) the library runs without its pins, which only regular
areas have: the pin of a partially programmed end marker and the tombstone of a torn slot.
Everything else stands. What changes:

- **in a regular area, a weak end marker is decided again at every boot, until the next update.**
  Programmed in part: the slot is taken, but nothing makes that durable; if the marker reads all
  erased at a later boot, the previous version is current again, a rollback by one version. All
  erased: the previous version is taken and the torn slot left as it is; if its marker reads
  programmed at a later boot, the torn slot is taken then, a "late new" by one version. Both last
  until the application's next update, which erases the torn slot;
- **an EWLF area behaves the same.** It never programs a write unit that does not read erased,
  so it has no pin to lose. On a flash that refuses zeros over a weak word that reads erased, a
  marker, a confirm or a certificate is stepped over where the other flash would have taken it:
  one position or one word, and no change to what is shown.

What the variant gives in exchange is a scan of regular areas that writes nothing at all. The two
variants share the layout and the format; which one runs is the NVM's declaration, and the test
suite runs under both.

.. _design-residual:

What a power cut can still do
-----------------------------

Measured against the objectives of the threat model: O1, after any power cut the data reads as the
old or the new committed version and nothing is lost; O2, no rollback below the last committed
version; O3, no bricking; and the wear threat T1.5. "Until the next update" means the
application's next completed update to the area: from then on the newest version is complete and
ahead of any weak word, and the weak word goes when its slot or unit is reused. In a regular area
the weak word is the end marker of the update a power cut interrupted; in an EWLF area it is a
word of the record or of the pair the cut interrupted, or a word of a repair or of a switch.

.. list-table::
   :header-rows: 1
   :widths: 28 22 25 25

   * - Case
     - Consequence
     - With pinning
     - Without pinning
   * - Regular area: end marker read programmed in part at one boot, all erased at a later one (T2)
     - Rollback by one version (O2); no loss
     - Closed: the first boot that sees it pins it
     - Open until the next update
   * - Regular area: end marker read complete at one boot, all erased at a later one
     - Rollback by one version (O2); no loss
     - Excluded by T3 where ``checksum2`` carries 16 programmed bits once read complete, 8- and
       16-byte write units (its zero padding). Open on 2-byte write units, until the next update
     - Same
   * - Regular area: end marker read all erased at one boot, programmed at a later one
     - A late new by one version: the interrupted update's data shows up after the previous version
       was shown; each boot alone reads old or new
     - Closed: the torn slot is tombstoned at the first boot
     - Open until the next update; the numbering rule keeps it from becoming a version collision
   * - EWLF area: any word of the log or of a record read one way at one boot, another way at a
       later one (T2, T3 not assumed)
     - None against the data: what a boot shows is proven or was copied by programs that returned,
       above everything a cut may have left. One repair at the boot that sees the change
     - Closed
     - Same
   * - EWLF area: a run of log switch attempts, each cut inside its certificate or the marker
       right after it, as many as the area has certificates; or of repairs each cut, as many as a
       log unit has positions (:ref:`design-stuck-state`)
     - The area is stuck: it refuses to write, or to show a version it cannot prove (O3). No
       rollback, no loss: the content is there, and readable by
       :c:func:`lftl_read_unproven`
     - Open; the area reports how many such cuts are left before each of them, and
       :c:func:`lftl_format` brings it back
     - Same
   * - Large area: everything above that applies to EWLF areas
     - The same: its log is the log of an EWLF area, and its pool adds no way to the stuck state
     - Same as for EWLF areas
     - Same
   * - Large area: a cut in a chunk or in the erase of a pool unit
     - None: the unit is free, no map names it, the previous data is shown
     - Closed: the unit is erased again when the head enters it, whatever it reads
     - Same
   * - Large area: an update or a transaction larger than the free units of the pool
     - :c:macro:`LFTL_ERROR_LARGE_NO_SPACE`, nothing changed, the transaction aborted (O3 for
       that update only)
     - Sized away: a pool of the chunks plus the chunks of the largest update never lacks room,
       whatever the power cuts
     - Same
   * - Large area: data the flash changed after it was written
     - Not a power cut: detected once per reset by the checksum of the data,
       :c:macro:`LFTL_ERROR_DATA_CHECKSUM`, never a fallback to older data
     - The application reads what it can with :c:func:`lftl_read_unchecked` and repairs
     - Same
   * - Power cut at every boot
     - Wear, availability (O3)
     - Regular area: one word program per boot, on the same word, until one completes. EWLF area:
       nothing unless the application asks for the repair; then one pair per boot until one
       returns, and an erase only when a log unit has no position left
     - Regular area: nothing is written at boot. EWLF area: same as with pinning
   * - Power cut during a pin, a tombstone, a repair or a switch
     - None by itself
     - The word is left weak and decided again at the next boot; a cut repair is a started
       position the next repair goes above
     - n/a for regular areas; same for EWLF areas
   * - Torn erase, or reuse of cells that merely read erased (T4)
     - Corruption of what is written over them
     - Closed: a slot is erased right before every update, a data unit right before its first
       record, a log unit in the session that first writes to it, under a certificate; reading
       erased is never trusted. EWLF areas assume that an interrupted erase of cells that were
       already erased leaves them erased (:ref:`design-ewlf-checked`)
     - Same
   * - Weak word on flash with ECC (T1.13)
     - An NMI on read; a handler that does not return hangs the device (O3)
     - Regular area: one NMI at the first boot after the torn update, then the pin or the
       tombstone programs a valid codeword. EWLF area: the read callback must survive the fault
       and return a value that is neither all erased nor all zeros (R7); the weak word is read at
       every boot until its unit is erased
     - Regular area: no such target. EWLF area: same as with pinning
   * - Hostile flash content (Case 2 of the threat model)
     - A wrong copy chosen, a version collision, memory safety
     - Slot, record and pair positions stay inside the area; crafted content can make the scan of
       a regular area program one word of zeros, once per crafted word, and the repair of an EWLF
       area program one pair or switch its log
     - Same, without the words of regular areas
   * - Stale data (T1.8)
     - Old versions physically readable until their slot or unit is reused
     - Accepted
     - Same
   * - Update time that depends on the state of the flash (O5)
     - An EWLF update takes longer than usual; bounded
     - An update steps over started positions, at most a unit's worth; a rollover is one erase and
       a log switch at most one erase, in every state; a regular-area update costs the same in
       every state
     - Same
   * - The model itself
     - T2, T3 and T4 are physics assumptions no vendor documents; N3 is documented on the STM32L5
       and STM32U5 only
     - Regular areas rest on T2, T4, N3, and T3 for their second row. EWLF areas rest on T1, T2,
       T4 and on the two properties of :ref:`design-ewlf-checked`; not on T3
     - Regular areas rest on T2, T4, and T3 for their second row. EWLF areas: same as with pinning

One case depends on timing in both variants, the second row of regular areas: a marginal marker
that reads complete at every boot that looks at it, then all erased before the next one. All its
programmed bits must cross the read threshold between two consecutive boots, with no boot in
between to see the partial state. Its consequence is a rollback by one version of data the
application may have acted on, never a loss, since the previous version is complete. Pinning the
current slot's end marker at every boot would close it, at the price of one word program per boot
and per area; it is not done. EWLF areas do not have it: a confirm that read all zeros at one
boot and reads erased at a later one costs a repair, which keeps the same version.

Unchanged in both variants: wear driven by the application's updates (T1.5), a torn
:c:func:`lftl_format` (T1.6), and everything in the threat model that is not about the scan.

.. _design-stuck-state:

The stuck state of EWLF and large areas
---------------------------------------

The one state in which an EWLF area refuses to proceed, :c:macro:`LFTL_ERROR_STUCK`. It is a
state of the log, so it is the same for a large area, whose pairs are larger and fewer: the
positions of ``O`` below are about 120 for a small map. It has
two faces:

- *at an update*: the log switch finds no certificate, no reserved pair or no position left. The
  write fails, and every later write fails the same way; the area keeps showing its current
  version, at this boot and at the next ones;
- *at a boot*: the repair has no room. :c:func:`lftl_complete_scan` raises the error before
  programming anything, and reads and writes raise it too.

Both are a loss of availability, not of data. The content is physically present; in the second
case the version the scan chose is readable with :c:func:`lftl_read_unproven`, and
:c:func:`lftl_format` brings the area back empty.

It is reached only by exhausting a reserve, one word or one position per power cut, each cut
inside a particular program:

.. list-table::
   :header-rows: 1
   :widths: 22 42 18 18

   * - Reserve
     - What one cut burns
     - Window per cut on the STM32U5
     - Cuts to stuck
   * - the certificates of ``L``
     - a cut in the certificate's program, or in the marker right after it, that marker then
       reading erased
     - 256 us
     - ``X``, 32 by default
   * - the positions of ``O``
     - a cut in the first entry of a switch, ``O`` then holding a started position
     - 384 us
     - ``P`` + ``R``, 160
   * - the reserved pairs of ``L``
     - reachable only once ``L`` holds no proven entry: a cut in every pair of its life, then one
       per reserved pair
     - 256 to 384 us
     - ``P`` + ``R``, 160

The certificates are the smallest reserve, and each attempt on them is preceded by an erase of
``O``. A brown-out that recurs at a fixed point of a switch, on marginal power, is the realistic
way there, and ``X`` should be sized for it: 64 certificates cost about 1 KB of each log unit on
the STM32U5. The attempts need not be consecutive boots: a boot that does not switch neither
burns a word nor gives one back. What ends the count is a switch that returns: the new live unit
has all its certificates.

**The remaining cuts.** :c:func:`lftl_scan_status` reports, at any time, the smallest number of
further power cuts, each at the worst instant, after which the area is stuck. It is the smallest
of three counts taken from the scan's reading of the log, every word that does not read erased
counting as burnt:

- the certificates of ``L`` that read erased, when the next switch from ``L`` needs one;
- when ``L`` is certified, the positions of ``O`` above its started ones;
- when ``L`` holds no proven entry, the free positions of ``L``.

Outside a bad episode it is the number of free certificates, and it is back to ``X`` after a
switch that returned. Power cuts outside the windows of the table never move it, nor do the
application's own writes. The library applies no threshold: the application does, before a write,
or after a reset before it asks for the repair. One thing the figure cannot see is a burnt word
that a cut left weak and that reads erased at this boot: the figure is right for this session,
and T2 allows it to drop by more than one at a later boot.

.. _design-ewlf-checked:

How the design of EWLF areas was checked
----------------------------------------

**A model checker.** ``test/ewlf-model/ewlf_model.py``, in the repository, explores every
reading of every weak word, every instant of a power cut and every number of updates per session,
depth first over boots, against the tearing model of the flash page: a weak word reads its value,
erased or garbage, drawn again at every boot; a word programmed into cells an interrupted erase
left weak may read garbage for good; the flash refuses a program over a word that is not erased,
zeros over a partially programmed word excepted. It checks every version a boot shows for
rollback, lost acknowledgement, loss, a write shown after an older version was, and the stuck
state. ``proto_v4.py`` is the design of this page, and the library's functions follow it one for
one. No violation was found up to 4 power cuts and 8 boots; the stuck state shows up after exactly
as many cuts as the model has certificates; and each rule, removed alone, gives a violation
within 3 cuts: without the reserved pair of a switch a loss, without the repair before showing a
rollback, without the certificates or without the erase of an empty certified unit a write shown
after an older version was. The checker proves the absence of counter-examples within its bounds
and under its model, nothing more.

**Two properties of the flash the design rests on**, beyond T1 to T5:

- a program of all zeros that returns over a partially programmed word leaves solid zeros. That
  the STM32L5 and STM32U5 accept the program is measured; that the result is solid is an
  inference;
- an interrupted erase of a unit whose cells were already erased leaves them erased. Without it
  the checker finds a loss after two power cuts. It was measured on one STM32U5A5, with a system
  reset as the cut: in 40 interrupted erases of an erased page, at 20 instants, every word read
  erased at three readings without an ECC flag, and all of them then took a program
  (``FLASH.md``, section 4, point 5). Not covered: a loss of power in place of the reset, other
  parts and temperatures, aged cells.

**The library** is tested on the Linux simulator, where a power cut leaves weak words and every
program of an update and of a repair is cut in turn, and on the STM32U5A5: :ref:`tests`.

.. _design-large-checked:

How the design of large areas was checked
-----------------------------------------

The same model checker, with ``test/ewlf-model/chunked_model.py``: ``proto_c2.py`` is the pool,
whose rules the library's large block follows, run with one slot per unit; ``proto_c2p.py`` puts
it over ``proto_v4p.py``, the log with its data inside the pair, which is what a large area is;
``proto_c2a.py`` puts it over the contract of the log instead of the log, which has far fewer
states and explores the pool further. A chunk is one word in the model. No violation was found
with 2 power cuts and 5 boots on the log (162 million sessions), with 2 cuts and 4 boots and a
log switch at every commit, with three chunks, and with two operations per session; nor with 3
cuts and 6 boots over the contract of the log.
Each pool rule, removed alone, gives a violation: without the check that a unit holds no current
chunk, or no chunk of the open transaction, a wrong read; without the erase of a unit that reads
erased, a chunk that reads garbage later (T4). The simulation in ``chunked_sim.py`` plays the pool
at real sizes and finds no update short of room under the sizing rule, and the erases spread as
the rule counts. ``CHUNKED.md`` and ``LARGE.md`` in the repository hold the figures; the checksum
of the data plays no part in this argument and is not in the model.
