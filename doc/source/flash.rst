.. _flash:

Flash requirements and assumptions
==================================

This page states in one place what *lean-ftl* requires from the NVM an area lives in, what it
deliberately does not rely on, and what it assumes about a write or an erase interrupted by a
power loss. The statements are numbered (R, N, T) so that the other documents can cite them.

They apply per area, that is per the :c:type:`lftl_nvm_props_t` the area is registered with. A
flash with sectors of several sizes is used through a region where they are all the same, or
through callbacks that present one. What each supported target provides, read from its vendor
documentation, is in `FLASH.md <https://github.com/sebastien-riou/lean-ftl/blob/main/FLASH.md>`_
in the repository.

Terms: the *write unit* and the *erase unit* are the smallest program and erase operations the
NVM supports (see the key concepts of the :doc:`introduction`); the *erased value* is what a byte
reads after an erase; a *weak unit* is a write unit or an erase unit whose program or erase a
power loss interrupted.

.. _flash-required:

Required
--------

**R1. One erase size for the whole area.**
``erase_size`` is the erase unit of the whole area. The area starts on an erase unit and is made
of whole erase units; :c:type:`nvm_erase_t` is asked to erase ``n_pages`` consecutive units in one
call. Enforced by :c:func:`lftl_register_area` (:c:macro:`LFTL_ERROR_BAD_CONFIG`); whether the
value matches the hardware is not checkable (R8).

**R2. One write size.**
``write_size`` is the write unit of the whole area: a power of two, dividing ``erase_size``, at
most :c:macro:`LFTL_WU_MAX_SIZE` (128 bytes). The data of a regular area is whole write units. On
flash with ECC, ``write_size`` is at least the ECC codeword, so that each meta-data item is one
codeword. Enforced by registration (:c:macro:`LFTL_ERROR_BAD_CONFIG`,
:c:macro:`LFTL_ERROR_WU_SIZE_TOO_LARGE`), except the ECC part.

**R3. An erased write unit accepts one program of any value.**
*lean-ftl* programs each write unit at most once between two erases, whole units only, aligned,
possibly several units per call. The source may be unaligned RAM, or NVM that does not overlap
the destination (:c:type:`nvm_write_t`).

**R4. Reads of any byte range.**
Any size at any alignment, through the memory map or :c:type:`nvm_read_t`, without side effect on
the content.

**R5. Completed operations are durable and independent.**
A program or an erase that returned success reads its value until the next erase of its unit and
is not disturbed by operations on other units. Failures are reported by the return code of the
callback: *lean-ftl* does not retry, verify or time out. Where the hardware does not verify, the
callback should (the STM32 callbacks of the repository read back what they programmed).

**R6. EWLF areas only: erased bytes read 0xFF.**
Free record positions, the ``prev_erased`` flag and the erased-data checksum of
:c:func:`lftl_erase_all` are recognised by that value. Regular areas do not need it (N1).

**R7. Flash with ECC.**
The write unit is at least the ECC codeword (R2). A double-bit error raises an NMI on the STM32L5
and STM32U5: the handler clears the flag and returns, so that the read completes and *lean-ftl*
rejects the content; a handler that loops or resets hangs the device at every boot. On the STM32U5
a pending single-bit correction flag masks the next NMI and must be cleared promptly. See T5.

**R8. The configuration is static and matches the hardware.**
``write_size``, ``erase_size``, the area and its data size are compile-time constants, never read
from the NVM. Registration checks that they are consistent with each other, not that they are
true.

.. _flash-not-relied-on:

Not relied on
-------------

**N1. The erased value, in regular areas.**
Any stable value works: 0x00, 0xFF or another. Checksums are computed from the data as it reads
back, so an erased slot never passes as valid whatever its erased value. Two places look at
``0xFF`` and are safe with any other value: a version word reading ``0xFFFFFFFF`` is skipped as
"no version" (a shortcut, the checksum would reject it anyway), and the repair of a torn slot only
acts when its last meta-data word reads all ``0xFF`` (otherwise the slot is kept, the safe
side). EWLF areas do need ``0xFF`` (R6).

**N2. Bit- or byte-granular programming.**
Whole write units only, up to 128 bytes. A flash that programs 16 bytes at a time is as good as
one that programs single bits.

**N3. Overwriting or re-programming a write unit.**
Between two erases a write unit is programmed once; nothing is ever written over programmed
cells. This is what makes flash that refuses to program a non-erased word (STM32L5, STM32U5) work
as is. A future repair that programs all zeros over a partially programmed unit would be an
opt-in capability declared per NVM: it is documented for the STM32L5 and STM32U5, unknown for the
M5531 and outside the specification of the W25Q32RV, and never assumed.

**N4. Advanced features and figures.**
Read-while-write, suspend and resume, hardware verify, CRC or all-one checks, program and erase
times, endurance figures. Wear is the application's budget, with the wear-leveling factor it
chooses.

**N5. The data value and the order of operations across units.**
All-``0xFF`` data is written like any other value. Nothing depends on the order in which units
are erased or programmed beyond R5.

.. _flash-tearing:

Tearing
-------

The anti-tearing argument of the repository's ``THREAT-MODEL.md`` rests on this model of what a
power loss leaves. These are assumptions about the physics of flash cells, not statements found in
vendor documentation: the four manuals read for ``FLASH.md`` say only that the content being
accessed is "not guaranteed" (STM32), "may become corrupted" (W25Q32RV) or nothing (M5531).

**T1. A cut affects only the unit being written.**
The write unit(s) being programmed or the erase unit being erased at that instant may end in any
state; every other unit keeps its content.

**T2. A weak write unit may read anything, and may read all erased later.**
Its cells got about the same partial charge, so the unit sits near the read threshold as a whole.
It may read complete, partially programmed or all erased, differently from one read to the next,
and since charge only leaks it drifts toward reading erased: a unit that read partially or even
fully programmed at one boot may read all erased at a later one.

**T3. Exception: at least 16 programmed bits, once read correctly.**
If the data written has at least 16 programmed bits (a Hamming weight of 16 or more in programmed
cells) and the weak unit once read correctly, it may later read incorrectly but never all erased.
Three consequences. A 32-bit checksum alone does not guarantee the weight (about 16 programmed
bits on average, often fewer). The zero padding that fills an 8- or 16-byte meta-data item
supplies it on the STM32L5 and STM32U5; 2-byte write units (CH32V307, the Linux test geometry) get
it only from an all-zero unit. On flash with ECC, "read correctly" includes a corrected single-bit
error: the first cell that flips is invisible to the software.

**T4. A weakly erased unit may read all erased, then not.**
Yes, this is assumed. An interrupted erase can leave cells just over the threshold on the erased
side. Charge loss moves them further toward erased, but programming other words in the same erase
unit (program disturb) and repeated reads (read disturb) push them back, so a unit that read all
erased at one boot may show programmed bits at a later one, especially once words are programmed
around them. *lean-ftl* therefore never trusts a unit because it reads erased: a regular area
erases the next slot before every write; an EWLF rollover erases the unit it leaves and records
the completed erase in the ``prev_erased`` flag of the new record, and when that flag is missing
the scan erases the unit again, even if it reads all erased. A design that drops that second erase
would need the opposite assumption.

**T5. Flash with ECC.**
A weak unit may fault (NMI) instead of returning a value (R7). An erased line with two flipped
bits is not detected (STM32L5 reference manual 6.3.2, STM32U5 7.3.2), so a weakly erased unit can
read a stray value without any fault. A single-bit correction hides one flipped cell (T3).

.. _flash-targets:

Per target
----------

.. list-table::
   :header-rows: 1

   * - Target
     - Write unit
     - Erase unit
     - ECC
     - Programming a non-erased unit
   * - STM32U5 (``target/stm32``)
     - 16 bytes
     - 8 KB
     - 9 bits per 128, NMI on double error
     - Refused, except all zeros
   * - STM32L5 (``target/stm32``)
     - 8 bytes
     - 2 KB (dual-bank mode)
     - 8 bits per 64, NMI on double error
     - Refused, except all zeros
   * - CH32V307 (``target/ch32v307``)
     - 2 bytes
     - 4 KB
     - none documented here
     - not documented here
   * - M5531 (no callbacks yet)
     - 4 bytes (32-bit program)
     - 8 KB
     - none mentioned
     - not specified
   * - Pico 2, W25Q32RV (no callbacks yet)
     - 1 to 256 bytes
     - 4 KB
     - none
     - outside the specification

The evidence, with the sections of each document, is in
`FLASH.md <https://github.com/sebastien-riou/lean-ftl/blob/main/FLASH.md>`_.
