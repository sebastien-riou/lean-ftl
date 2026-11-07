.. _chasing-ghosts:

Chasing ghosts
==============

A ghost is a record that reads valid at one boot and not at a later one. This page works through
a proposal made on 2026-11-05 against the last ghost of EWLF areas, the stuck state of
:ref:`design-stuck-state`: a second checksum, so that a record keeps its content and version once
its flags are gone. It uses the notation of that section, and it stays until the question is
settled, to be removed then.

The proposal
------------

The update programs the meta field, version and checksum, *before* the data, and the checksum
once more after it; the flags stay as they are. The second row gives the size of each field, the
last one the order in which an update programs them:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="6"><tr><td border="0" colspan="6" align="left"><b>one record, with the second checksum</b></td></tr><tr><td width="84" height="42">start marker<br/><font point-size="9">all zeros</font></td><td width="150" height="42">meta field<br/><font point-size="9">32-bit version, 64-bit checksum</font></td><td width="110" height="42">second checksum<br/><font point-size="9">the same 64 bits</font></td><td width="66" height="42">flag-a<br/><font point-size="9">all zeros</font></td><td width="66" height="42">flag-b<br/><font point-size="9">all zeros</font></td><td width="170" height="42">data<br/><font point-size="9">data_size bytes</font></td></tr><tr><td border="0"><font point-size="9">one write unit</font></td><td border="0"><font point-size="9">12 B, rounded up to whole write units</font></td><td border="0"><font point-size="9">8 B, one write unit</font></td><td border="0"><font point-size="9">one write unit</font></td><td border="0"><font point-size="9">one write unit</font></td><td border="0"><font point-size="9">rounded up to whole write units</font></td></tr><tr><td border="0">1</td><td border="0">2</td><td border="0">4</td><td border="0">5</td><td border="0">6</td><td border="0">3</td></tr></table>>];
   }

Three claims come with it:

1. A bit at 0 in the second checksum proves that the program of the data returned, so the meta
   field and the data are solid: both were programmed before it.
2. When the flags read erased, the meta field and the data still give the content and the
   version, checked by the first checksum, which no longer sits behind the data.
3. A record with the second checksum is solid, and a record without it, in the stuck state,
   marks the unit to erase.

Two states join the four of the scan: *pre-only*, the meta field valid and the data passing its
checksum, the second checksum erased; *posted*, a bit at 0 in the second checksum and both flags
erased, which is where today's *aborted* goes. In the diagrams ``c`` is the second checksum,
shown like ``m`` and ``d`` where a cut or a pin touched it.

What it gives
-------------

Claims 1 and 2 hold. The data is programmed after the meta field and before the second checksum,
so a bit in the second checksum means that the data program returned, which means that the meta
field's did: a record that ever showed a bit there keeps its content and version for good. Its
flags may drift, its second checksum may drift, and the content stays readable, as a pre-only
record at worst. In the histories of the stuck state, boot 4 then finds ``J`` and ``J'`` posted,
with solid content, and shows ``J'``: the rollback of those histories is gone, and the attacker
needs one more cut, below.

What it does not give
---------------------

**Claim 3 fails at the reading.** In the history of alternative 2, the second checksum of ``J''``
and ``J'''`` is weak, a cut landed in its program. At boot 4 it reads what a weak word reads (T2),
and the attacker who built the history lets it read valid, so that ``U0`` and ``U1`` look the
same; the reading that would point at the right unit is the one the attacker does not give.
Which unit to erase stays a guess.

**The first checksum validates the reading, not the solidity.** Programmed before the data, it
says that the data *as read at this boot* is what the writer intended, and nothing about the next
boot. Two histories give the same pre-only reading:

.. list-table::
   :header-rows: 1
   :widths: 44 12 16 14 14

   * - History
     - Meta field
     - Data
     - Second checksum
     - Later
   * - A cut in the second checksum, which drifted to erased
     - solid
     - solid
     - erased
     - readable for good
   * - A cut in the last word of the data, which reads right at this boot
     - solid
     - last word weak
     - erased
     - the checksum fails once the word drifts: no record

"Reads right" is the event of the stuck state under another name: every programmed cell of the
weak word reads programmed and the erased cells always read erased, the same as a flag reading
all zeros. A pre-only record may therefore be a torn write about to vanish, and the scan cannot
show one as it is:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0"></td><td border="0" colspan="2"><b>U1</b></td><td border="0" width="14"></td><td border="0" colspan="2"><b>U0</b></td></tr><tr><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td></tr><tr><td border="0" align="left"><b>Before</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">K  v<br/><font face="Courier" point-size="10">d ~, c ., a ., b .</font><br/><font point-size="9" color="#777777">cut</font></td><td bgcolor="#eeeeee" width="156"><font point-size="9" color="#777777">erased</font></td></tr><tr><td border="0" align="left"><b>Boot 1</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td bgcolor="#dbeafe" width="156"><b>K  v</b><br/><font face="Courier" point-size="10">d ~valid, c ., a ., b .</font><br/><font point-size="9" color="#777777">pre-only</font></td><td bgcolor="#eeeeee" width="156"><font point-size="9" color="#777777">erased</font></td></tr><tr><td border="0" align="left"><b>Boot 2</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td bgcolor="#dbeafe" width="156"><b>I  v-1</b><br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">K<br/><font face="Courier" point-size="10">d ~., c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td bgcolor="#eeeeee" width="156"><font point-size="9" color="#777777">erased</font></td></tr></table>>];
   }

- *Before.* ``I`` is current. The update to ``K`` is cut in the last word of its data.
- *Boot 1.* The weak word reads right: ``K`` passes its first checksum and is the newest record.
  Shown as it is, it gives the application ``K``.
- *Boot 2.* The word reads erased: ``K`` fails its checksum and is no record; the application
  reads ``I`` after ``K``.

One cut, and a write that never returned is shown, then taken back (O2). So the rule has to be
the half record's: a pre-only record is copied forward when a position is free and left alone
when none is, with the complete record shown, which is no rollback since a pre-only record was
never shown. And the scan never programs the second checksum of a record in place: its bit is the
proof that the data program returned, which a program made after *reading* the data does not
give, the pin's broken order again.

Where the residual goes
-----------------------

Under that rule the shown states, half, complete and posted, all carry a bit in the second
checksum, so a shown record can lose its content only if that bit was weak: the attacker has to
cut the program of the second checksum, then take the record through the flags as today. A
posted record is shown once its flags are pinned in place, flag-a then flag-b, so that it is
complete for good when the pin returns; the attacker has to cut that pin too. The cheapest
history starts like the minimal one of the stuck state, a torn first attempt that the
application's retry steps over:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0"></td><td border="0" colspan="2"><b>U1</b></td><td border="0" width="14"></td><td border="0" colspan="2"><b>U0</b></td></tr><tr><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td></tr><tr><td border="0" align="left"><b>Before</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">c ~, a ., b .</font><br/><font point-size="9" color="#777777">cut</font></td></tr><tr><td border="0" align="left"><b>Boot 1</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">c ~valid, a ~, b .</font><br/><font point-size="9" color="#777777">posted, pinned</font></td></tr><tr><td border="0" align="left"><b>Boot 2</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">c ~valid, a ~0, b ~</font><br/><font point-size="9" color="#777777">half, pinned</font></td></tr><tr><td border="0" align="left"><b>Boot 3</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td bgcolor="#dbeafe" width="156"><b>J  v</b><br/><font face="Courier" point-size="10">c ~valid, a ~0, b ~x</font><br/><font point-size="9" color="#777777">complete</font></td></tr><tr><td border="0" align="left"><b>Boot 4</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td bgcolor="#dbeafe" width="156"><b>I  v-1</b><br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">c ~., a ~., b ~.</font><br/><font point-size="9" color="#777777">pre-only</font></td></tr></table>>];
   }

- *Before.* The retry lands ``J`` at the last position of ``U0``; the cut lands in its second
  checksum.
- *Boot 1.* The second checksum reads valid: ``J`` is posted, newest, its content solid. The pin
  programs flag-a; the cut lands in that program.
- *Boot 2.* Flag-a reads all zeros: ``J`` is half, no position is left, ``U1`` holds the complete
  record: stuck. Flag-a is skipped, flag-b programmed; the cut lands in that program.
- *Boot 3.* Flag-b reads some zeros: ``J`` is complete and the application reads ``J``.
- *Boot 4.* The three weak words read erased: ``J`` is pre-only, with no position to be copied
  to and never shown as it is; the application reads ``I`` after ``J``.

The same rollback, for the cut in the retried write, then three cuts inside programs of one
write unit instead of two, and three weak words instead of two, on a flash that refuses zeros
over zeros as before.

Dropping the posted record instead
----------------------------------

A posted record was never acknowledged, so at boot 1 of the history above the scan could drop it
instead of pinning its flags: tombstone every position of ``U0``, erase ``U0``, show ``I``. In
that history it works. ``J`` was never shown, so showing ``I`` is no rollback, and O1 allows a
write that never returned to be lost. The tombstones before the erase also fix what made the
erase of alternative 1 unsafe: after a cut erase a tombstoned meta field reads a mix of zeros and
erased cells, never a version with a matching checksum, so no ghost comes back with a version
above later writes.

It fails on the other history that ends in a posted record. A record shown at a boot carries a
bit in flag-b; when that bit was the pin's and the pin was cut, both flags are weak, drift to
erased, and the record reads posted again, its second checksum solid. The flash reads the same
in both cases, the rule applies to both, and dropping that one is today's rollback at today's
price:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0"></td><td border="0" colspan="2"><b>U1</b></td><td border="0" width="14"></td><td border="0" colspan="2"><b>U0</b></td></tr><tr><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td></tr><tr><td border="0" align="left"><b>Before</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">a ~, b .</font><br/><font point-size="9" color="#777777">cut</font></td></tr><tr><td border="0" align="left"><b>Boot 1</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">a ~0, b ~</font><br/><font point-size="9" color="#777777">half, pinned</font></td></tr><tr><td border="0" align="left"><b>Boot 2</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td bgcolor="#dbeafe" width="156"><b>J  v</b><br/><font face="Courier" point-size="10">a ~0, b ~x</font><br/><font point-size="9" color="#777777">complete</font></td></tr><tr><td border="0" align="left"><b>Boot 3</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">a ~., b ~.</font><br/><font point-size="9" color="#777777">posted</font></td></tr><tr><td border="0" align="left"><b>Boot 3, dropped</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td bgcolor="#dbeafe" width="156"><b>I  v-1</b><br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td bgcolor="#eeeeee" width="156"><font point-size="9" color="#777777">erased</font></td><td bgcolor="#eeeeee" width="156"><font point-size="9" color="#777777">erased</font></td></tr></table>>];
   }

- *Before.* The retry lands ``J`` at the last position of ``U0``; the cut lands in its flag-a.
- *Boot 1.* Flag-a reads all zeros: ``J`` is half, no position is left: stuck. Flag-a is skipped,
  flag-b programmed; the cut lands in that program.
- *Boot 2.* Flag-b reads some zeros: ``J`` is complete and the application reads ``J``.
- *Boot 3.* Both weak flags read erased: ``J`` is posted. Dropped: ``U0`` is tombstoned and
  erased, and the application reads ``I`` after ``J``.

The cut in the retried write, then two cuts inside flag programs: the cut in the second checksum
is not needed, and the layout bought nothing. The pin re-programs a flag that reads erased, keeps
that record shown, and is what forces the attacker to cut the second checksum. Nor does the erase
on its own remove the imprecise cut: a copy cut anywhere before its flag-a leaves a dead position
beside the half record it was copying, and a unit holding a half record cannot be erased, since
that record may be the one a previous boot showed.

Dropping the half record instead
--------------------------------

One boot earlier in that history ``J`` is half, and the same move offers itself: at boot 1,
tombstone every position of ``U0``, erase it, show ``I``. ``J`` was never shown there either, so
it works there too. It is alternative 1 of :ref:`design-stuck-state` with its ghosts fixed and
its rollback intact: a half record is also what a record shown on a weak flag-b reads once that
flag drifts, and the scan has only the reading:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0"></td><td border="0" colspan="2"><b>U1</b></td><td border="0" width="14"></td><td border="0" colspan="2"><b>U0</b></td></tr><tr><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td></tr><tr><td border="0" align="left"><b>Before</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">a 0, b ~</font><br/><font point-size="9" color="#777777">cut</font></td></tr><tr><td border="0" align="left"><b>Boot 1</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td bgcolor="#dbeafe" width="156"><b>J  v</b><br/><font face="Courier" point-size="10">a 0, b ~x</font><br/><font point-size="9" color="#777777">complete</font></td></tr><tr><td border="0" align="left"><b>Boot 2</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">a 0, b ~.</font><br/><font point-size="9" color="#777777">half</font></td></tr><tr><td border="0" align="left"><b>Boot 2, dropped</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td bgcolor="#dbeafe" width="156"><b>I  v-1</b><br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td bgcolor="#eeeeee" width="156"><font point-size="9" color="#777777">erased</font></td><td bgcolor="#eeeeee" width="156"><font point-size="9" color="#777777">erased</font></td></tr></table>>];
   }

- *Before.* The retry lands ``J`` at the last position of ``U0``; the cut lands in its flag-b.
- *Boot 1.* Flag-b reads some zeros: ``J`` is complete and the application reads ``J``.
- *Boot 2.* Flag-b reads erased: ``J`` is half, no position is left: stuck. Dropped: ``U0`` is
  tombstoned and erased, and the application reads ``I`` after ``J``.

The cut in the retried write, then one cut inside a flag program and one ordinary weak word,
with nothing asked of the flash: cheaper than the residual, on every flash. The second checksum
changes nothing here, solid in both histories since both cuts came after it, and the flags read
the same. Telling a record that was shown from one that was not takes a mark written at the boot
that shows it, one more program a cut can leave weak: the flags are that mark already, and the
log of alternative 4 is the only form of it that escapes the cut.

A finer rule suggests itself: drop when flag-a reads partially programmed, since a solid flag-a
reads all zeros and a partial reading proves that the cut landed in flag-a, and keep when it
reads all zeros. The classification is right and the inference is not. A partial flag-a proves
that the cut was in flag-a, not that the record was never shown: the pin shows records whose
flag-a was cut, that is the stuck state itself, leaving flag-a alone when it reads all zeros and
programming flag-b; and a weak word reads differently from one boot to the next (T2), so the
flag-a that read all zeros at the pin boot can read partially programmed two boots later:

.. graphviz::
   :align: center

   digraph {
     graph [pad="0.05" margin="0"];
     node [shape=plaintext fontname="Helvetica" fontsize=11];
     d [label=<
       <table border="0" cellborder="1" cellspacing="0" cellpadding="5"><tr><td border="0"></td><td border="0" colspan="2"><b>U1</b></td><td border="0" width="14"></td><td border="0" colspan="2"><b>U0</b></td></tr><tr><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td><td border="0"></td><td border="0"><font point-size="9" color="#777777">p0</font></td><td border="0"><font point-size="9" color="#777777">p1</font></td></tr><tr><td border="0" align="left"><b>Before</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">a ~, b .</font><br/><font point-size="9" color="#777777">cut</font></td></tr><tr><td border="0" align="left"><b>Boot 1</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">a ~0, b ~</font><br/><font point-size="9" color="#777777">half, pinned</font></td></tr><tr><td border="0" align="left"><b>Boot 2</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td bgcolor="#dbeafe" width="156"><b>J  v</b><br/><font face="Courier" point-size="10">a ~0, b ~x</font><br/><font point-size="9" color="#777777">complete</font></td></tr><tr><td border="0" align="left"><b>Boot 3</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td width="156">I  v-1<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td width="156">J<br/><font face="Courier" point-size="10">m 0, d ~x, c ., a ., b .</font><br/><font point-size="9" color="#777777">no record</font></td><td width="156">J  v<br/><font face="Courier" point-size="10">a ~x, b ~.</font><br/><font point-size="9" color="#777777">half</font></td></tr><tr><td border="0" align="left"><b>Boot 3, dropped</b></td><td width="156">older<br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td bgcolor="#dbeafe" width="156"><b>I  v-1</b><br/><font face="Courier" point-size="10">a 0, b 0</font><br/><font point-size="9" color="#777777">complete</font></td><td border="0"></td><td bgcolor="#eeeeee" width="156"><font point-size="9" color="#777777">erased</font></td><td bgcolor="#eeeeee" width="156"><font point-size="9" color="#777777">erased</font></td></tr></table>>];
   }

- *Before.* The retry lands ``J`` at the last position of ``U0``; the cut lands in its flag-a.
- *Boot 1.* Flag-a reads all zeros: ``J`` is half, no position is left: stuck. Flag-a is skipped,
  flag-b programmed; the cut lands in that program.
- *Boot 2.* Flag-b reads some zeros: ``J`` is complete and the application reads ``J``.
- *Boot 3.* Flag-b reads erased and flag-a some zeros: ``J`` is half, with a partial flag-a.
  Dropped: ``U0`` is tombstoned and erased, and the application reads ``I`` after ``J``.

The residual's rollback at the residual's price, without even the all-zeros reading at the last
boot. The pin re-programs that flag-a instead, which the flash accepts, then flag-b, and ``J`` is
complete for good: the partial reading is a case the pin closes, and where it comes from a record
never shown, the drop would only add an erase. The all-zeros reading, where the two histories
coincide, is the residual, and there the finer rule keeps, as today. Without ``zero_overwrite``
the inference fails sooner, since half records are shown as they are, partial flag-a included.

Verdict
-------

One more cut, bought with one more write unit per record, a fifth state, a pin for posted records
and a checksum computed before the data goes out. It is the row "one pin mark per record" of the
table in alternative 3 of :ref:`design-stuck-state`, with the mark written by the update instead of
the pin; the pattern flags of the same table raise the price by the same cut and cost nothing.
Dropping a posted record instead of pinning it, with an erase of its unit, is safe and gives that
cut back; dropping a half one is alternative 1 again. The layout keeps the content alive through
the flags and loses it through the second checksum instead: the ghost moved by one word. Not
adopted.
