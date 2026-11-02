.. _design:

Anti-tearing and wear leveling
==============================

*lean-ftl* keeps, per area, one block of data that the application updates atomically: after a
power cut at any instant, the block reads as the last committed version or as the one before it,
never a mix of the two and never nothing. It also spreads the erases over the area, so that the
data can be updated many more times than one page endures. This page describes how both are done
for the two kinds of areas: the layout on the flash, how an update is written, how the first
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
one. A copy is *valid* when its checksum matches its data and version, and the valid copy with
the highest version is the current one. The version is not expected to wrap: 4.3 billion updates
exceed the endurance of any area.

**Meta-data of a regular area.** The version, the checksum and a third item, the *end marker*
``checksum2``, a second copy of the checksum, each occupy one write unit, 4 bytes at least (R2 of
the flash page): the value is in the first 4 bytes and the bytes next to it are programmed to zero
with it. A whole item therefore reads one of three ways: all erased, when it was never
programmed; a value, with some bits still erased when a power cut interrupted its program; or all
zeros. An update programs the data first, then the version and the checksum, then the end marker,
last and in its own write unit: a copy whose end marker is complete is complete. An EWLF record
has its own header, with two flags after a single meta field: :ref:`design-ewlf`.

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
its current copy. The scan reads the flash and chooses; what it programs is a word of zeros, on a
flash that allows it, or in an EWLF area a new record copied from a torn one, and it never
erases, so that a power cut at every boot never costs an erase: wear stays what the application's
own updates cost (T1.5 of the threat model). In a regular area the slot with the highest valid
version is taken, and its end marker, read once, decides the rest:

- complete, or all zeros: the slot is current. This is every boot in normal operation;
- programmed in part: the update got to its last word, so the checksum before it is complete and
  the slot is stable. It is current. On a flash with ``zero_overwrite`` the end marker is *pinned*
  first, so that it cannot read all erased at a later boot and bring the previous version back;
- all erased: the cut came during the checksum word at the latest, which may be weak. The slot is
  not taken: the previous version is current, and it is stable, since it was current when the
  interrupted update began. On a flash with ``zero_overwrite`` the slot not taken is *tombstoned*
  first: its version item is pinned to zeros, so that its checksum can never verify again and the
  slot cannot come back at a later boot as a "new" version once its end marker reads differently.

In an EWLF area the two flags of a record decide, and a record whose last word may be weak is not
trusted in place but copied: :ref:`design-ewlf-scan`.

**Pinning.** To pin a write unit is to program it to all zeros, whatever it holds. Every cell gets
a full programming pulse, so a pinned unit reads zeros for good, and on flash with ECC it should
leave a valid codeword in place of the inconsistent one a torn program left (T1.13 of the threat
model says what remains to be measured there). Pinning needs a flash that accepts an all-zero
program over a partially programmed unit, N3 of the flash page: an opt-in capability, declared
per NVM with the ``zero_overwrite`` member of :c:type:`lftl_nvm_props_t`. The library never pins a
unit that already reads all zeros, except where the build says the flash accepts that
(``LFTL_EWLF_PIN_OVER_ZEROS``, see the stuck state of :ref:`design-ewlf-scan`). A pin or a
tombstone is one word program, made once, since the pinned word then reads zeros for good; a power
cut during it leaves a word closer to settled, decided again at the next boot. Without
``zero_overwrite`` the scan pins nothing and, in a regular area, makes the same choices again at
every boot, on the word as it reads then, until the next update settles them:
:ref:`design-without-pinning` says what that changes.

**Bounded time.** Every loop in the library runs over a quantity fixed by the configuration: the
number of slots, of pages, of records in a page. Each operation therefore has a worst case that
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
into each erase unit: the data and its meta-data travel together, and an erase is paid once per
unit's worth of updates instead of once per update. It is meant for data small compared to the
page, and it gives up transactions: every update writes a whole record. The area is at least 2
erase units, at least 2 records must fit in a unit, and the erased bytes of the flash must read
``0xFF`` (R6 of the flash page): free record positions are recognized by that value.

Layout
^^^^^^

A record is a header followed by the data; the second row gives the size of each field, the
last one the order in which an update programs them:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="6"><tr><td border="0" colspan="5" align="left"><b>one record</b></td></tr><tr><td width="84" height="42">start marker<br/><font point-size="9">all zeros</font></td><td width="150" height="42">meta field<br/><font point-size="9">32-bit version, 64-bit checksum</font></td><td width="66" height="42">flag-a<br/><font point-size="9">all zeros</font></td><td width="66" height="42">flag-b<br/><font point-size="9">all zeros</font></td><td width="170" height="42">data<br/><font point-size="9">data_size bytes</font></td></tr><tr><td border="0"><font point-size="9">one write unit</font></td><td border="0"><font point-size="9">12 B, rounded up to whole write units</font></td><td border="0"><font point-size="9">one write unit</font></td><td border="0"><font point-size="9">one write unit</font></td><td border="0"><font point-size="9">rounded up to whole write units</font></td></tr><tr><td border="0">1</td><td border="0">3</td><td border="0">4</td><td border="0">5</td><td border="0">2</td></tr></table>>];
   }

- the *start marker*, one write unit, programmed to all zeros, and the first word of the record to
  be programmed;
- the *meta field*: the 32-bit version then the 64-bit checksum, little endian, 12 bytes rounded
  up to whole write units with zeros, programmed in one call. A version that reads ``0xFFFFFFFF``
  (erased) or ``0`` (a tombstone, below) is no record, and that test comes before the checksum;
- *flag-a*, one write unit programmed to all zeros once the meta field is complete: a bit at 0 in
  it proves the data, the version and the checksum solid;
- *flag-b*, one write unit programmed to all zeros once flag-a is complete: a bit at 0 in it proves
  flag-a solid;
- the data, rounded up to whole write units.

The header is 15, 18, 24, 40 and 64 bytes on 1-, 2-, 4-, 8- and 16-byte write units, and the
record is a multiple of the write unit. There is no magic: with only record positions ever
examined, below, the checksum alone tells a record from anything else, at 2\ :sup:`-64` per
position for content that is not one.

Records only ever start at *positions*: the top of an erase unit, which is the unit's end minus
one record size, and every record size below it. Only positions are ever examined, so the data can
never be mistaken for a record, even when it holds a copy of a header, and every record lies
within one erase unit. Positions fill a unit from its top down, and the units are used from the
highest address down, wrapping from the lowest back to the highest. Three units, four records per
unit, the data at its 11th update of the current cycle; ``U2`` is the unit at the highest address,
``p0`` the top position of a unit, and the blue cell is the current record:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="0" cellspacing="0" cellpadding="0"><tr><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U2</b> (current)</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70">v+9</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70">v+10</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70" bgcolor="#dbeafe"><b>v+11</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70" bgcolor="#eeeeee"><font color="#777777">erased</font></td></tr></table></td><td border="0" width="18"></td><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U1</b> (oldest)</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70">v+1</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70">v+2</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70">v+3</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70">v+4</td></tr></table></td><td border="0" width="18"></td><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U0</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70">v+5</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70">v+6</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70">v+7</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70">v+8</td></tr></table></td></tr></table>>];
   }

Format
^^^^^^

:c:func:`lftl_format` erases every unit and writes the record of version 1 at the top of the
highest unit, over erased data. It is not protected against a power cut, as for regular areas.

Update
^^^^^^

An update, :c:func:`lftl_write`, :c:func:`lftl_basic_write` or :c:func:`lftl_erase_all`, writes a
whole new record:

1. it looks for room below the current record, in the same unit: the first position whose start
   marker reads all erased, and whose meta field and flags read erased too. Nothing after an
   erased start marker was ever programmed, since the start marker is programmed first, unless the
   marker is weak and the position was stepped over and tombstoned since, which the second test
   sees. A started position on the way holds a record a power cut tore, one the scan tombstoned, or
   a copy it made: it is stepped over; when its checksum is valid, the new record is numbered above
   its version, so that two valid records can never share a version whatever that record's flags
   read later; and on a flash with ``zero_overwrite`` its start marker is programmed to zeros again,
   unless it already reads all zeros, so that a weak start marker cannot later read erased and open
   a hole in the run of started positions, and its meta field is tombstoned when it does not read
   valid, so that a weak one cannot later read valid with a version the new record is about to
   take;
2. with no room left, the update *rolls over*: the unit immediately below, wrapping from the lowest
   to the highest, is erased, whatever it reads, and the record goes to its top. The unit left
   behind is not erased: it keeps its records until the write order comes back to it. A unit is
   therefore erased exactly once per cycle, right before its first record is programmed, and never
   at boot; an erase a power cut interrupted is redone by the next attempt to use the unit, and
   cells a torn record left behind are never programmed over without an erase (T4 of the flash
   page);
3. the record is programmed in this order, each step completed before the next starts: the start
   marker; the data, the current data with the written range replaced, or left erased by
   :c:func:`lftl_erase_all`; the meta field, in one call, its checksum computed from the data meant
   to be written and not from what reads back, so that a record that did not program as intended
   fails its check; flag-a; flag-b, last.

In the example above, the update to ``v+13`` finds ``U2`` full after ``v+12`` and rolls over: it
erases ``U1``, the next unit in write order and always the oldest, then programs ``v+13`` at its
top:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="0" cellspacing="0" cellpadding="0"><tr><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U2</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70">v+9</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70">v+10</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70">v+11</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70">v+12</td></tr></table></td><td border="0" width="18"></td><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U1</b> (current)</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70" bgcolor="#dbeafe"><b>v+13</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70" bgcolor="#eeeeee"><font color="#777777">erased</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70" bgcolor="#eeeeee"><font color="#777777">erased</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70" bgcolor="#eeeeee"><font color="#777777">erased</font></td></tr></table></td><td border="0" width="18"></td><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U0</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70">v+5</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70">v+6</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70">v+7</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70">v+8</td></tr></table></td></tr></table>>];
   }

A cut during the erase of ``U1`` leaves it partly erased and nothing else changed: the scan finds
``v+12`` in ``U2``, and the next update erases ``U1`` again before programming it. A cut during the
programming of ``v+13`` leaves a torn record at the top of ``U1``, and its flags say how far it
got (:ref:`design-ewlf-scan`): before flag-a, it is not taken and ``v+12`` stays current; in or
after flag-a, its data is complete and it is taken, through a copy when the cut may have left a
flag weak. ``U2`` is full, so the next update erases ``U1`` again and starts over. In both cases
the erase is paid for by an update the application asked for.

An update costs one record's worth of programming, and one page erase every *records per unit*
updates. The search for room adds, only when torn, tombstoned or copied positions lie below the
current record, one start marker read and one checksum per such position, and with
``zero_overwrite`` one or two word programs, at most a unit's worth; a rollover is exactly one
erase in every state.

.. _design-ewlf-scan:

Scan
^^^^

**What a position holds.** From one reading of its meta field and both flags, a position is one
of four things. *No record*: its version reads erased or 0, or its checksum fails; erased
positions, writes torn before or during the meta field, tombstones and foreign content. *Aborted*:
checksum valid, both flags all erased; a write cut before flag-a, or whose flag-a is weak and
reads erased. *Half*: checksum valid, a bit at 0 in flag-a, flag-b all erased; the data is solid,
since flag-a was programmed after the meta field completed, but flag-a itself may be the weak word
of a cut during it, or during flag-b with flag-b reading erased. *Complete*: a bit at 0 in flag-b,
which was programmed after flag-a completed, so flag-a is solid. Every decision about a position
is made on that one reading. Inside a unit the newest record is the lowest one, since positions
fill from the top. Two complete records sharing a version is
:c:macro:`LFTL_ERROR_VERSION_COLLISION`; a half or aborted record sharing one with a complete
record is reachable after a cut tombstone and harmless, the complete one wins.

**The choice, then the loop.** The scan collects the newest complete record, the newest half one
and the newest aborted one, with the highest version of every valid record it saw, and then, at
most once per record position plus two:

- an aborted record newer than both others is the abort of a write that never reached flag-a, or
  whose flag-a is weak and read erased this time. With ``zero_overwrite`` it is *tombstoned*: its
  meta field is pinned to zeros, so that its version reads 0 and it is no record for good, and
  the choice runs again. Without, it is ignored: it was never shown, since a record is shown only
  once a bit at 0 in flag-a is solid, and such a bit never reads erased again;
- a half record newer than the complete one holds solid data behind a flag-a that may be weak. It
  is *copied forward*: its data is written again as a new record, through the update path with
  the half record as the current one, which ends with a flag-b the scan saw programmed, and the
  choice runs again, finding the copy complete. A half record is never tombstoned: it may be the
  record a previous boot showed, when its flag-b read a bit. The copy's version is counted from
  the choice's own readings, never from a meta field read again, and the copy may not erase the
  unit of the complete record nor one holding a half record newer than it; when no position is
  left under those rules, the *stuck state*, the half record is pinned in place with
  ``zero_overwrite``, flag-a then flag-b, which makes it complete for good once the program of
  flag-b returns, or shown as it is without (:ref:`design-residual`);
- otherwise the complete record is current, or :c:macro:`LFTL_ERROR_NO_VALID_VERSION` when there is
  none.

The scan never erases, programs nothing at a boot that follows no cut, and sets the context once,
at the end, from the position it found: the content of the flash picks among fixed positions
inside the area, never where a record starts, and a cut during a copy leaves the context unset.

**Why a version shown is never taken back.** A record is shown either after a program the scan
completed in this very boot, the copy's flag-b or the pin, or on a reading of flag-b with a bit at
0, which proves flag-a solid. A solid zero never reads all ones again, so a record once shown is
half or complete at every later boot; it is never tombstoned, its unit is never erased by a copy,
the application's updates erase only the unit below the current record's, and a newer record
replaces it only when it is shown under the same rules. The application therefore reads a
version, then the same or a newer one, never an older one, with any number of power cuts and
without the flash assumption T3; the one exception is the stuck state on a flash that refuses
zeros over zeros, in :ref:`design-residual`. An aborted record whose tombstone was cut can come
back once, as a half record that gets copied: the version before it, then it, which is not a
rollback.

Visited unit by unit, every unit costs one pass from its bottom up to its first complete record,
and an area with many units would pay for all of them at every boot. The scan instead finds the
current unit, then the newest records inside it, with a number of reads logarithmic in the unit
and record counts; the pass over every unit, the *linear pass*, is its fallback.

**The order of the units.** Call a unit's *key* the version of its top record when its checksum
verifies, whatever its flags, and 0 otherwise. The top record is the first one written into the
unit after its erase, so every record of a newer unit has a higher version than every record of an
older one: keys order the units by age. Four units, four records per unit; the first cycle filled
``U3`` to ``U0`` with versions 1 to 16, the second cycle has reached ``U1``:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="0" cellspacing="0" cellpadding="0"><tr><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U3</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70">17</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70">18</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70">19</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70">20</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">key</font></td><td border="0"><b>17</b></td></tr></table></td><td border="0" width="18"></td><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U2</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70">21</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70">22</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70">23</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70">24</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">key</font></td><td border="0"><b>21</b></td></tr></table></td><td border="0" width="18"></td><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U1</b> (current)</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70">25</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70" bgcolor="#dbeafe"><b>26</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70" bgcolor="#eeeeee"><font color="#777777">erased</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70" bgcolor="#eeeeee"><font color="#777777">erased</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">key</font></td><td border="0"><b>25</b></td></tr></table></td><td border="0" width="18"></td><td border="0"><table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0" colspan="2" align="left"><b>U0</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="70">13</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="70">14</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="70">15</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="70">16</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">key</font></td><td border="0"><b>13</b></td></tr></table></td></tr></table>>];
   }

Read in write order from the current unit, ``U1``, ``U2``, ``U3``, ``U0``, the keys only decrease:
25, 21, 17, 13. Read in address order, ``U0`` to ``U3``, that is 13, 25, 21, 17: a decreasing
sequence with a single jump up, at the current unit. The units without a key, an erased unit of
the first cycle, a unit whose top record is torn or tombstoned, a unit left partly erased by a
torn erase, are the oldest of all and sit at the old end of the cycle, so the shape holds. The
checksum is what makes a key trustworthy: an erase only sets bits to 1, so the top record of a
unit whose erase was cut early can keep a programmed start marker while a few cells of its version
word have flipped, which reads as a larger number; the checksum covers the version and rejects
it. A torn rollover record keys its unit whatever its flags read, so that the copies the scan may
have placed below it are looked for there. A key is read once per scan: a partly erased unit may
read differently from one read to the next, and whichever way it reads it sits at the bottom of
the order.

**The current unit** is the jump in that cyclic sequence, and it is found by halving: keep a range
of units that contains the jump, probe its middle unit and compare its key with the key of the
range's last unit; higher means the jump is at the middle or before it, otherwise after. Each
probe is one header read and one checksum, and the number of probes is the number of times the
unit count can be halved, plus one for the last unit. Eight units, eight records each, the second
cycle has reached ``U2``:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0"></td><td border="0"><b>U7</b></td><td border="0"><b>U6</b></td><td border="0"><b>U5</b></td><td border="0"><b>U4</b></td><td border="0"><b>U3</b></td><td border="0"><b>U2</b> <font point-size="9">(current)</font></td><td border="0"><b>U1</b></td><td border="0"><b>U0</b></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="44">65</td><td width="44">73</td><td width="44">81</td><td width="44">89</td><td width="44">97</td><td width="44">105</td><td width="44">49</td><td width="44">57</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="44">66</td><td width="44">74</td><td width="44">82</td><td width="44">90</td><td width="44">98</td><td width="44" bgcolor="#dbeafe"><b>106</b></td><td width="44">50</td><td width="44">58</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">...</font></td><td width="44">...</td><td width="44">...</td><td width="44">...</td><td width="44">...</td><td width="44">...</td><td width="44" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td width="44">...</td><td width="44">...</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p7</font></td><td width="44">72</td><td width="44">80</td><td width="44">88</td><td width="44">96</td><td width="44">104</td><td width="44" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td width="44">56</td><td width="44">64</td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">key</font></td><td border="0"><b>65</b></td><td border="0"><b>73</b></td><td border="0"><b>81</b></td><td border="0"><b>89</b></td><td border="0"><b>97</b></td><td border="0"><b>105</b></td><td border="0"><b>49</b></td><td border="0"><b>57</b></td></tr></table>>];
   }

1. the range is ``U0`` to ``U7``; the key of ``U7``, its last unit, is 65;
2. probe ``U3``: 97 is higher than 65, so the jump is in ``U0`` to ``U3``;
3. probe ``U1``: 49 is not higher than 97, so the jump is in ``U2`` to ``U3``;
4. probe ``U2``: 105 is higher than 97, so the jump is in ``U2`` to ``U2``: ``U2`` is the current
   unit, after 4 reads.

**The newest records inside it.** Positions fill from the top, so the newest records of the
current unit are its lowest started ones: the start markers are read from the bottom up to the
first one that is not erased, one write unit per read and no checksum, then the records from there
up to the first complete one. A search by halving on start markers would be cheaper, but it needs
the started positions to form one run from the top, and a start marker a power cut left weak can
read all zeros when the next update steps over it, which leaves it alone, and erased at a later
boot: a hole in the run, above newer records, which a halving would take for the end of the run. A
search from the bottom cannot be fooled by it. An orange cell is a torn record; the last column
lists the scan's probes, one read of one write unit each:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0"></td><td border="0" colspan="3" align="left"><b>U2</b> (current)</td></tr><tr><td border="0"></td><td border="0"><font point-size="9">start marker</font></td><td border="0"><font point-size="9">record</font></td><td border="0" align="left"><font point-size="9">the scan's probes, in order</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p0</font></td><td width="56">started</td><td width="170">105</td><td border="0"></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p1</font></td><td width="56">started</td><td width="170">106</td><td border="0"></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p2</font></td><td width="56">started</td><td width="170" bgcolor="#fde7c8">data, no meta field: a torn write</td><td border="0" align="left"><font point-size="9">stepped over by the update to 107, which programmed its start marker again</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p3</font></td><td width="56">started</td><td width="170" bgcolor="#dbeafe"><b>107</b></td><td border="0" align="left"><font point-size="9">probe 5: started, the lowest started position: the records are read from here up</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p4</font></td><td width="56" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td width="170" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td border="0" align="left"><font point-size="9">probe 4: not started</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p5</font></td><td width="56" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td width="170" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td border="0" align="left"><font point-size="9">probe 3: not started</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p6</font></td><td width="56" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td width="170" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td border="0" align="left"><font point-size="9">probe 2: not started</font></td></tr><tr><td border="0" align="right"><font point-size="9" color="#777777">p7</font></td><td width="56" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td width="170" bgcolor="#eeeeee"><font color="#777777">erased</font></td><td border="0" align="left"><font point-size="9">probe 1: not started</font></td></tr></table>>];
   }

**The check and the fallback.** The result is checked before it is trusted: the current unit is a
local maximum in write order, so its two neighbours must have lower keys or none, and the complete
record found must have a higher version than both. The neighbour below in write order is then read
whole, from its bottom up to its first complete record: that is where a torn rollover leaves its
record, and where the copies the scan made of it sit below it. When the check fails, which
hostile flash content, a bug, or a current unit whose newest records are all half or aborted (a
rollover write cut in a flag, before the scan repaired it) can cause, the scan falls back to the
linear pass over every unit, which is right whatever the area holds. Unit indices stay inside the
area either way. The Linux and sanitizer test builds define ``LFTL_SCAN_CROSS_CHECK``: every
scan runs both searches and raises :c:macro:`LFTL_INTERNAL_ERROR` if they disagree on the records
that matter.

A boot thus costs a number of header reads and checksums logarithmic in the unit count, one
start-marker read per free position of the current unit, the walk of the next unit, one pass in
normal operation, and nothing programmed; after a cut, one tombstone, one record copy or one flag
pin per torn record, each followed by one more choice.

Wear leveling
^^^^^^^^^^^^^

Each unit is erased once per cycle, and a cycle is *records per unit* times *units* updates: that
product is the wear leveling factor, and :c:macro:`LFTL_AREA_EWLF` sizes the area, at least 2
units, so that the product reaches the factor asked for. The old records of a unit stay physically
readable until the write order comes back to it (T1.8 of the threat model); nothing reads them
through the API.

Limitations
^^^^^^^^^^^

- one area holds one data of fixed size, and every update writes all of it;
- no transactions: :c:func:`lftl_transaction_start` raises
  :c:macro:`LFTL_ERROR_EWLF_TRANSACTION_UNSUPPORTED`;
- at least two records must fit in a page: the layout macro checks it at compile time, registration
  at run time;
- the update that rolls over costs an erase and the others do not, so the update time is not
  uniform.

A possible extension, not implemented: an EWLF area could serve as the journal of transactions
that span several areas, each record describing the state of such a transaction so that the first
access after a reset can roll it back. The updates of the areas involved would then have to keep
the previous version until the journal marks the transaction complete.

.. _design-without-pinning:

Without pinning
---------------

On a flash not declared with ``zero_overwrite`` (N3 of the flash page: the STM32L5 and STM32U5
document the all-zero overwrite, the W25Q32RV specifies programming for erased locations only, the
M5531 and CH32V307 manuals do not say) the library runs without its pins: in a regular area the
pin of a partially programmed end marker and the tombstone of a torn slot; in an EWLF area the
tombstone of an aborted record, the pin of the start markers and meta fields an update steps over,
and the pin of the stuck state. Everything else stands: the scan never erases, a unit is erased at
the rollover into it, the words are written in the same order, the newest version wins, the copy
of a half record needs no pin, and the search for the current unit works unchanged, since it
only reads keys. What changes:

- **in a regular area, a weak end marker is decided again at every boot, until the next update.**
  Programmed in part: the slot is taken, but nothing makes that durable; if the marker reads all
  erased at a later boot, the previous version is current again, a rollback by one version. All
  erased: the previous version is taken and the torn slot left as it is; if its marker reads
  programmed at a later boot, the torn slot is taken then, a "late new" by one version. Both last
  until the application's next update, which erases the torn slot;
- **in an EWLF area, an aborted record is left as it is**, and may come back once as a half record
  that gets copied: a late new by one version, until the next update numbers above it. And when
  the scan has no position left for a copy, the newest half record is shown as it is: that is the
  one residual, in :ref:`design-residual`;
- **a new record is numbered above every record with a valid checksum, taken or not.** With the
  torn record left in place, "current version plus one" would give the next record the torn
  record's version, and if the torn marker later read programmed the scan would find two
  candidates sharing a version. The update reads the torn record's header anyway, on its way to
  the free position, so it numbers above it. The pinning variant uses the same rule; with
  tombstoning it is only a simplification.

What the variant gives in exchange is a scan that writes nothing at all: under a power cut at
every boot the flash is not touched, where the pinning variant programs one word per boot. The two
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
flag of the record the cut interrupted, or a word of the scan's own program, a tombstone, a copy
or a pin, when the cut came at a boot.

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
   * - EWLF area: a flag of the torn record read one way at one boot, another way at a later one (T2,
       T3 not assumed)
     - None: a record whose flag-b read a bit has a solid flag-a and is half or complete at every
       later boot; one whose flag-a alone read a bit is copied before it is shown; one whose flags
       read erased is not shown
     - Closed. An aborted record is tombstoned; one whose tombstone was cut may come back once, as
       a half record that is copied: a late new by one version, never a rollback
     - Closed for rollback; an aborted record is left as it is and may come back once, a late new
       by one version
   * - EWLF area: no position left for a copy (the stuck state: a write cut in a flag at the top of
       a unit, then one cut inside each repair copy, *records per unit* minus one of them in a
       two-unit area, more in a larger one)
     - Rollback by one version (O2) in the residual case; no loss
     - The half record is pinned in place and complete once the program of flag-b returns. Residual:
       a flag that read all zeros is left alone on a flash that refuses zeros over zeros
       (``LFTL_EWLF_PIN_OVER_ZEROS`` unset, FLASH.md open point 2), and if it was weak and later
       reads erased the record can turn aborted after it was shown. None where the flash accepts
       that program
     - The half record is shown as it is; if its flag-a was the weak word and later reads erased,
       the previous version comes back. Open until the next update
   * - Power cut at every boot
     - Wear, availability (O3)
     - Regular area: one word program per boot, on the same word, until one completes. EWLF area:
       one tombstone, one record copy or one flag pin per boot, until one completes; a chain of cut
       copies takes positions, never the unit of the complete record nor one holding a half record
       newer than it, then the pin takes over. Never an erase at boot beyond a copy's rollover into
       a unit it may erase
     - Regular area: nothing is written at boot. EWLF area: the copies, then nothing
   * - Power cut during a pin, a tombstone or a copy
     - None by itself
     - The word is left weak and decided again at the next boot, on a word closer to settled; a torn
       copy is a half or aborted record the next boot handles like any other
     - n/a, or the torn copy
   * - Torn erase, or reuse of cells that merely read erased (T4)
     - Corruption of what is written over them
     - Closed: a slot is erased right before every update, a unit right before its first record;
       reading erased is never trusted
     - Same
   * - Weak word on flash with ECC (T1.13)
     - An NMI on read; a handler that does not return hangs the device (O3)
     - One NMI at the first boot after the torn update, then the pin or the tombstone programs a
       valid codeword, or the copy leaves the torn word behind in a record the scan no longer
       reads past its flags; the handler must return (R7)
     - No such target; it would be one NMI per boot until the word's unit is erased
   * - Hostile flash content (Case 2 of the threat model)
     - A wrong copy chosen, a version collision, memory safety
     - Slot and record positions stay inside the area; crafted content can make the scan program
       one word of zeros, once per crafted word, or copy a crafted half record once, into the
       positions a copy may use
     - Same, without the words
   * - Stale data (T1.8)
     - Old versions physically readable until their slot or unit is reused
     - Accepted
     - Same
   * - Update time that depends on the state of the flash (O5)
     - An EWLF update takes longer than usual; bounded
     - An update steps over torn positions and programs their start markers, at most a unit's
       worth; a rollover is exactly one erase in every state; a regular-area update costs the same
       in every state
     - Same, without the programs
   * - The model itself
     - T2, T3 and T4 are physics assumptions no vendor documents; N3 is documented on the STM32L5
       and STM32U5 only
     - Regular areas rest on T2, T4, N3, and T3 for their second row. EWLF areas rest on T1, T2,
       T4 and N3; not on T3
     - Regular areas rest on T2, T4, and T3 for their second row. EWLF areas rest on T1, T2 and T4

One case depends on timing in both variants, the second row of regular areas: a marginal marker
that reads complete at every boot that looks at it, then all erased before the next one. All its
programmed bits must cross the read threshold between two consecutive boots, with no boot in
between to see the partial state. Its consequence is a rollback by one version of data the
application may have acted on, never a loss, since the previous version is complete. Pinning the
current slot's end marker at every boot would close it, at the price of one word program per boot
and per area; it is not done. EWLF areas do not have it: a flag-b that read complete proved
flag-a solid, and a record with a solid flag-a is copied when flag-b reads erased later.

Unchanged in both variants: wear driven by the application's updates (T1.5), a torn
:c:func:`lftl_format` (T1.6), and everything in the threat model that is not about the scan.
