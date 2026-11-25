User guide
=======================

Header to include
---------------------

The ``lean-ftl.h`` header includes all the symbols. 
This file is the same no matter the build type that you use.
It is therefore safe to use ``liblean-ftl/include/lean-ftl.h`` from
``liblean-ftl-debug.tar.gz`` even if you link against the ``minSizeRel`` library.

Enabling helper macros
---------------------------------------
``lean-ftl.h`` contains few helper macros that ease the 
declaration of your LFTL areas.
To use those helper macros, you must define 3 macros before including
``lean-ftl.h``:

- LFTL_DEFINE_HELPERS: Indicates that you want to use the helper macros. Value does not matter.
- LFTL_WU_SIZE: Defines the size for "write unit". Value shall be the minimum write size in the NVM or a multiple of it.
- LFTL_PAGE_SIZE: Defines the size for "LFTL page". Value shall be the minimum erase size in the NVM or a multiple of it and LFTL_WU_SIZE.

The constraints on both values (power of two, divisibility, maximum) are R1 and R2 of
:ref:`flash-required`.

.. code-block:: c
  :linenos:
  :caption: Example: Enabling helper macros for STM32U5
  :name: Example: Enabling helper macros for STM32U5

  #define LFTL_DEFINE_HELPERS
  #define LFTL_PAGE_SIZE (8*1024)
  #define LFTL_WU_SIZE 16 
  #include "lean-ftl.h"

.. note:: This user guide assumes that you use the helper macros

Declaring variables in the NVM
----------------------------------
The helper macros are assuming that you declare all your LFTL areas within a 
``struct`` called ``nvm``.

Within the ``nvm`` struct, you can declare one or more LFTL areas using 
the macro :c:macro:`LFTL_AREA`.

.. code-block:: c
  :linenos:
  :caption: Example: Declaring variables in two LFTL areas
  :name: Example: Declaring variables in two LFTL areas

  typedef struct data_flash_struct {

    LFTL_AREA(a,
      uint64_t data0[4];
      uint64_t data1[4];
      ,2)
    
    LFTL_AREA(b,
      uint64_t data2[4];
      uint64_t data3[4];
      ,2)
      
  } __attribute__ ((aligned (LFTL_PAGE_SIZE))) data_flash_t;
  data_flash_t nvm;

.. note:: In the example above, ``data0`` is accessible via ``nvm.data0``,
  or ``nvm.a_data.data0``.

.. note:: Members declared inside an LFTL area can be of any type however,
  in an area declared with :c:macro:`LFTL_AREA`, together they must occupy a
  multiple of ``LFTL_WU_SIZE``: pad them if needed, :c:func:`lftl_register_area`
  raises :c:macro:`LFTL_ERROR_BAD_CONFIG` otherwise.

Choosing the wear leveling factor
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Typical NVMs have a limited "endurance", i.e., a given page supports a 
limited number of erase/write cycles. This typically ranges from 1k to 100k,
Please refer to the NVM datasheet.

*lean-ftl* implements "wear-leveling" to remove this barrier however 
this feature does not allow unlimited erase/write, it merely multiplies
the endurance of the NVM. The multiplier is what we call the "wear-leveling-factor".
In other words, if you use 2 (which is the minimum), the native endurance is 
multiplied by 2.

Why not setting it to 1 billion once and for all ? Well, it also 
multiplies the size of the area, so a factor of 3 use one third more than a factor of 2.
It is therefore desirable to set the wear leveling factor with the minimum 
value that provides the endurance required by the use case.

.. note:: Some other implementation are more efficient than *lean-ftl*
  in term of area consumption for a given wear-leveling-factor however
  they are typically prone to fragmentation and they are more dependant on
  the usage pattern.

Extreme Wear Leveling Factor (EWLF)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
With :c:macro:`LFTL_AREA`, each unit of wear-leveling-factor costs one full
page, even if the area content is much smaller than a page. When the desired
wear-leveling-factor is large, this can waste a lot of NVM space.

:c:macro:`LFTL_AREA_EWLF` addresses this: it packs several copies of the content,
the *records*, into each page whenever the content is small enough, so a large
wear-leveling-factor no longer means one page per unit. It is declared the
same way as :c:macro:`LFTL_AREA`, just replace the macro name:

.. code-block:: c
  :linenos:
  :caption: Example: Declaring an EWLF LFTL area
  :name: Example: Declaring an EWLF LFTL area

  typedef struct data_flash_struct {

    LFTL_AREA_EWLF(c,
      uint64_t counter;
      ,1000)

  } __attribute__ ((aligned (LFTL_PAGE_SIZE))) data_flash_t;
  data_flash_t nvm;

The macro reserves the pages that hold the records, 2 at least, plus 2 pages for the log that
says which record is current: an EWLF area is 4 pages at least. The log also bounds the factor
an area reaches, whatever the number of its other pages: about 300 on the STM32U5 with the
defaults (:ref:`design-ewlf`).

An area declared with :c:macro:`LFTL_AREA_EWLF` must be registered with
:c:func:`lftl_register_area_ewlf` instead of :c:func:`lftl_register_area`.
Formatting, reading and writing work the same way as for a regular area, with three differences:

- EWLF areas do not support transactions: :c:func:`lftl_transaction_start`
  raises :c:macro:`LFTL_ERROR_EWLF_TRANSACTION_UNSUPPORTED`;
- at least two records, the area content plus a start marker and a meta field each
  (:c:macro:`LFTL_EWLF_RECORD_SIZE`), must fit in one page, the NVM's erased bytes must read
  ``0xFF`` and its write callback must report a refused program as such (R6 and R9 of
  :ref:`flash-required`);
- after a reset, the area may have a *repair* to do before it can be read or written, when the
  last session was cut by a power loss. Nothing is written until the application asks for it.

After a reset, set the context's ``data`` to :c:macro:`LFTL_INVALID_POINTER`, as for a regular
area, then call :c:func:`lftl_complete_scan` before the first read or write. It does the
repair if there is one and nothing otherwise, so it can be called unconditionally:

.. code-block:: c
  :linenos:
  :caption: Example: An EWLF area after a reset
  :name: Example: An EWLF area after a reset

  lftl_register_area_ewlf(&ewlf_c);
  lftl_complete_scan(&ewlf_c); //does the repair a power cut left to do, if any
  lftl_read(&ewlf_c, &counter, &nvm.counter, sizeof(counter));

Without that call, :c:func:`lftl_read` and the write functions raise
:c:macro:`LFTL_ERROR_SCAN_PENDING` when a repair is pending, and work as usual when none is. On
an area that was never formatted the call raises :c:macro:`LFTL_ERROR_NO_VALID_VERSION`, as a
read does.

An application that wants to decide before the flash is written asks first.
:c:func:`lftl_scan_status` reads the area without writing it and reports the pending repair and
``remaining_cuts``: the number of further power cuts, each at the worst instant, that the area
can take before it is *stuck* (:ref:`design-stuck-state`). That number is the number of
certificates of the area, 32 by default, in normal operation, and goes down by one for each log
switch a power cut interrupts in a particular 256 us window:

.. code-block:: c
  :linenos:
  :caption: Example: Deciding before the repair
  :name: Example: Deciding before the repair

  lftl_scan_status_t status;
  lftl_scan_status(&ewlf_c, &status);
  if(status.stuck){
    //no room left for the repair: salvage what the scan chose, then format
    lftl_read_unproven(&ewlf_c, &counter, &nvm.counter, sizeof(counter));
    lftl_format(&ewlf_c);
  } else {
    if(status.remaining_cuts < 8){
      //the power supply keeps failing at the wrong time: ask the user before going on
    }
    lftl_complete_scan(&ewlf_c);
  }

The same number is worth a look before a write, since most of the cuts that count are cuts of a
write: a write that finds no certificate left raises :c:macro:`LFTL_ERROR_STUCK` and the
area keeps showing its current content. :c:func:`lftl_read_unproven` reads the area in any
state; while a repair is pending it returns the version the repair then makes current.

The number of certificates is the ``ewlf_certificates`` member of :c:type:`lftl_ctx_t`; left at
zero, it is :c:macro:`LFTL_EWLF_CERTIFICATES`. It is part of the on-flash format of the area: an
area whose value changes must be formatted again.

Use :c:func:`lftl_is_ewlf` if you need to tell, given a context, whether it refers to an EWLF
area or a regular one. How EWLF areas work is described in :ref:`design-ewlf`. Their on-flash
format changed in v0.4.0: an EWLF area written by an earlier version is not read and must be
formatted again (regular areas are unchanged since v0.1.0).

Large areas
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
A regular area rewrites its whole content at every update, and so does an EWLF area. For data of
several pages this is costly: 500 000 updates of 64 KB need fifty copies of it, whatever the size
of each update. :c:macro:`LFTL_AREA_LARGE` cuts the data into *chunks* of one page each, keeps
them in a *pool* of pages, and rewrites only the chunks an update touches. An update of the whole
data costs what it costs in a regular area, an update of a few bytes one page:

.. code-block:: c
  :linenos:
  :caption: Example: Declaring a large LFTL area
  :name: Example: Declaring a large LFTL area

  typedef struct data_flash_struct {

    LFTL_AREA_LARGE(d,
      uint8_t records[64*1024];
      ,24)

  } __attribute__ ((aligned (LFTL_PAGE_SIZE))) data_flash_t;
  data_flash_t nvm;

The last argument is the number of pages of the pool; the macro adds 2 pages for the log that
says where each chunk is. How large the pool must be (:ref:`design-large`):

- the chunks of the data plus the chunks of the largest update or transaction: 8 + 8 above, so
  that a transaction may rewrite all of it. :c:macro:`LFTL_ERROR_LARGE_NO_SPACE` is raised,
  with nothing changed, by an update that needs more free pages than the pool has;
- for wear, the chunks never rewritten plus the chunk writes over the life of the area divided by
  the endurance of a page: at 10 000 cycles, the 24 pages above allow 160 000 writes of one chunk
  (the rule counts the 8 chunks as never rewritten), or 30 000 updates of the whole data (24
  pages of 10 000 erases, 8 per update).

An area declared with :c:macro:`LFTL_AREA_LARGE` is registered with
:c:func:`lftl_register_area_large`. Formatting, reading, writing and transactions work as for a
regular area; the boot is that of an EWLF area, the same calls (:c:func:`lftl_complete_scan`,
:c:func:`lftl_scan_status`, :c:func:`lftl_read_unproven`) and the same stuck state; the flash must
meet the same requirements (R6 and R9 of :ref:`flash-required`). The differences:

- :c:func:`lftl_transaction_start` erases nothing: each page a transaction touches is taken from
  the pool at its first write. The tracker is larger, :c:macro:`LFTL_TRANSACTION_TRACKER_SIZE`
  for any type of area, :c:macro:`LFTL_TRANSACTION_TRACKER_SIZE_LARGE` for a static buffer;
- the area keeps a CRC-64 of its whole data, and checks the data against it once per reset,
  before the first call that uses the data: a read, a write that does not replace the whole data,
  the start of a transaction. That reads all of it, about 0.2 s per megabyte on the STM32U5A5.
  :c:func:`lftl_check_data` does it at the moment the application chooses instead:

.. code-block:: c
  :linenos:
  :caption: Example: A large area after a reset
  :name: Example: A large area after a reset

  lftl_register_area_large(&large_d);
  lftl_complete_scan(&large_d); //does the repair a power cut left to do, if any
  if(!lftl_check_data(&large_d)){
    //the flash lost some of the data since it was written: no older copy to fall back to
  }
  lftl_read(&large_d, &rec, &nvm.records[i], sizeof(rec));

A mismatch is an error, never a fallback to older data: there is none, and a fallback would be a
rollback. From then on every call that uses the data raises :c:macro:`LFTL_ERROR_DATA_CHECKSUM`.
:c:func:`lftl_read_unchecked` reads the data all the same, and :c:func:`lftl_repair_checksum`
makes the checksum that of the data as it reads, in one commit, after which the application
repairs the chunks it knows are wrong by ordinary writes, one page at a time:

.. code-block:: c
  :linenos:
  :caption: Example: Repairing a large area
  :name: Example: Repairing a large area

  if(!lftl_check_data(&large_d)){
    lftl_read_unchecked(&large_d, &rec, &nvm.records[i], sizeof(rec)); //what is still right
    lftl_repair_checksum(&large_d); //the data as it reads is accepted
    lftl_write(&large_d, &nvm.records[i], &good_rec, sizeof(good_rec));
  }

A write of the whole data and :c:func:`lftl_erase_all` need no check and leave the area sound.

Use :c:func:`lftl_is_large` to tell, given a context, whether it refers to a large area.
``ewlf_certificates`` sets the certificates of its log as for an EWLF area. How large areas work
is described in :ref:`design-large`; their on-flash format is new in v0.5.0.

Single LFTL area vs many
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Using a single LFTL area is fine for simple applications.
This section highlight cases where it make sens to declare more
than one LFTL area.

For applications which have a large amount of mostly static data
and few data with high endurance requirements, it make sense to 
declare 2 LFTL areas:

- one for the mostly static data, with wear-level factor = 2
- one for the high endurance data, with wear-level factor > 2

For applications which have several independant processes, 
it make sense to declare one LFTL area for each process:

- *lean-ftl* is not thread safe.
- A single transaction is supported at any time for a given LFTL area.

.. note:: Even when using one LFTL area for each process, the 
  user need to take care about synchronization either at the 
  call back level or at the application level.

One downside of having multiple LFTL areas is that transactions 
are limited to one area, so it is not possible to cover all NVM 
changes with a single transaction anymore. Another downside is 
the potential overhead incurred for each LFTL area, especially
if the target NVM as large pages: declaring an LFTL area consumes 
at least 2 NVM pages, even if the data is much smaller.

Declaring NVM properties
--------------------------
*lean-ftl* needs to know few basic properties of the target(s) NVM(s).
The integrator shall declare one :c:type:`lftl_nvm_props_t` for each targeted NVM.

.. code-block:: c
  :linenos:
  :caption: Example: Declaring NVM properties
  :name: Example: Declaring NVM properties

  lftl_nvm_props_t nvm_props = {
    .base = &nvm,
    .size = sizeof(nvm),
    .write_size = nvm_write_size,
    .erase_size = nvm_erase_size,
    .zero_overwrite = 1, //the STM32L5 and STM32U5 document it
    .erase = nvm_erase,
    .write = nvm_write,
    .read = nvm_read,
    .error_handler = throw_exception,
  };

.. note:: ``base`` and ``size`` can be a subset of the physical NVM.

What ``write_size`` and ``erase_size`` may be, and what the NVM behind them must provide, is on
the :ref:`flash` page (R1, R2). ``zero_overwrite`` says whether the flash accepts programming a
write unit to all zeros whatever it currently holds (N3 there; the STM32L5 and STM32U5 document
it): *lean-ftl* then pins, at the first access after a reset, the word a power cut left weak in
a regular area, instead of deciding about it again at every boot. Leave it false when the manual
does not say. EWLF areas do not use it. :ref:`design` describes the pins, and
:ref:`design-without-pinning` what changes without them.

Implementing the callbacks
-----------------------------
In order to use LFTL, the following callbacks needs to be implemented
on your target platform:

- :c:type:`nvm_erase_t`
- :c:type:`nvm_write_t`
- :c:type:`nvm_read_t`
- :c:type:`error_handler_t`

The callbacks of the devices the test suite runs on are part of
`lean-bsp <https://github.com/sebastien-riou/lean-bsp>`__: each board support package holds those
of its flash, in ``src/bsp_nvm.c`` (``lean-bsp-nucleo-u5a5``, ``lean-bsp-nucleo-l552``,
``lean-bsp-numaker-m5531``, ``lean-bsp-pico2``). A RAM based implementation for Linux is in
https://github.com/sebastien-riou/lean-ftl/tree/main/target/linux

.. _using-device-libraries:

Device libraries
^^^^^^^^^^^^^^^^

For some devices the release holds a *device library*: *lean-ftl* with the callbacks of that
device's flash already in it, built from the board support package above. Next to the library,
``lftl-device.h`` gives the geometry and declares the callbacks, so that the integration is one
header and one archive:

.. code-block:: c
  :linenos:
  :caption: Example: Using the device library of the STM32U5A5xJ
  :name: Example: Using the device library of the STM32U5A5xJ

  #include "lftl-device.h" //LFTL_PAGE_SIZE, LFTL_WU_SIZE, nvm_erase, nvm_write, nvm_read
  #include "lean-ftl.h"

  lftl_nvm_props_t nvm_props = {
    .base = &nvm,
    .size = sizeof(nvm),
    .write_size = LFTL_WU_SIZE,
    .erase_size = LFTL_PAGE_SIZE,
    .zero_overwrite = LFTL_ZERO_OVERWRITE,
    .erase = nvm_erase,
    .write = nvm_write,
    .read = nvm_read,
    .error_handler = throw_exception,
  };

.. list-table::
   :header-rows: 1

   * - Library
     - Flash
     - Range the callbacks accept
     - What the application provides
   * - ``stm32u5a5xj``
     - internal flash of the STM32U5A5xJ, 4 MB
     - the whole flash, ``0x08000000`` to ``0x083FFFFF``
     - a ``.RamFunc`` section in RAM (the erase and the program run from it)
   * - ``stm32l552xe``
     - internal flash of the STM32L552xE, 512 KB, dual-bank mode
     - the whole flash, ``0x08000000`` to ``0x0807FFFF``
     - a ``.RamFunc`` section in RAM
   * - ``m5531h2ljae``
     - internal flash of the M5531H2LJAE, 2 MB
     - APROM bank 1 only, ``0x00200000`` to ``0x002FFFFF``: programming the bank the code runs
       from was never tested
     - CMSIS's ``SystemCoreClock``

A library is named after the part number of the MCU, down to what sets the size of its flash, when
the flash is internal, and after the board when the flash is a separate chip. Its callbacks refuse
any address outside their range, and they are weak symbols: an application can replace one. The
bank size is built in, so a part of the same family with another flash size needs its own build
(``on/stm32u5a5xj`` in the repository is the model). On the test boards the suite runs in the
board support package's own region, a part of that range.

The Raspberry Pi Pico 2 has no device library: its callbacks go through pico-sdk
(``hardware_flash``), which a project for that board builds anyway, so they are taken as source.
Add ``src/bsp_nvm.c`` of ``lean-bsp-pico2`` to the project, with the BSP's ``include/bsp_api.h``
and ``src/bsp_nvm.h``, define ``BSP_NVM0_BASE`` and ``BSP_NVM0_SIZE`` to the range given to
*lean-ftl* (both, or neither for the upper 2 MB of the flash), and point the callbacks of
:c:type:`lftl_nvm_props_t` to the region's ``erase``, ``write`` and ``read``, as
``lean-bsp/lftl_device_nvm.c`` in the repository does.

What the callbacks must guarantee is R3 to R5 and R10 of :ref:`flash-required`, and what they
need not provide is in :ref:`flash-not-relied-on`: *lean-ftl* programs each write unit at most once
between two erases, whole units only, and never overwrites; a write of several units programs them
one at a time, from the lowest address up (R10). For EWLF areas the write callback returns
:c:macro:`LFTL_NVM_STATUS_REFUSED` when the flash refused a program and programmed nothing (R9):
the callbacks above do.

.. note:: On flash with ECC that raises an NMI on a double-bit error (STM32L5, STM32U5),
  a program or erase interrupted by a power cut can leave a word that faults when read,
  and *lean-ftl* must read such words before it can skip them. The NMI handler must clear
  the ECC error flag and return, so that the read completes; the STM32 callbacks above do
  nothing about it, and a handler that loops or resets hangs the device at every boot.
  For an EWLF area the read must also come back with a value that is neither all ``0xFF``
  nor all ``0x00`` in the word that faulted.
  See the integrator section of ``THREAT-MODEL.md`` in the repository, and R7 and T5 of
  the :ref:`flash` page.

.. note:: Nothing but *lean-ftl* may program inside an area. A tool that writes an image of the
  flash back, such as the restore of a snapshot, the production programming of a pre-built
  image or a firmware update that copies pages, must erase the pages first, then program only
  the write units that are not all ``0xFF``. A plain download programs every byte: on the
  STM32U5 a write unit programmed with all ``0xFF`` reads erased but refuses the next program,
  and an EWLF area fails at its next update with :c:macro:`LFTL_ERROR_LOW_LEVEL_WRITE`. See R3
  of the :ref:`flash` page.

Declaring LFTL areas 
----------------------
Each LFTL area has its volatile context maintained in a
:c:type:`lftl_ctx_t` struct.

.. code-block:: c
  :linenos:
  :caption: Example: Declaring two LFTL areas
  :name: Example: Declaring two LFTL areas

  lftl_ctx_t nvma = {
    .nvm_props = &nvm_props,
    .area = &nvm.a_pages,
    .area_size = sizeof(nvm.a_pages),
    .data = LFTL_INVALID_POINTER,
    .data_size = sizeof(nvm.a_data),
    .transaction_tracker = LFTL_INVALID_POINTER,
    .next = LFTL_INVALID_POINTER
  };
  lftl_ctx_t nvmb = {
    .nvm_props = &nvm_props,
    .area = &nvm.b_pages,
    .area_size = sizeof(nvm.b_pages),
    .data = LFTL_INVALID_POINTER,
    .data_size = sizeof(nvm.b_data),
    .transaction_tracker = LFTL_INVALID_POINTER,
    .next = LFTL_INVALID_POINTER
  };

Library initialization
------------------------
After a power up, the library must be initialized using :c:func:`lftl_init_lib`.
Each area must be registered using :c:func:`lftl_register_area`.

.. code-block:: C
  :linenos:
  :caption: Example: Librairy initialization
  :name: Example: Librairy initialization

  lftl_init_lib();
  lftl_register_area(&nvma);
  lftl_register_area(&nvmb);


Initial formatting
------------------------
Each LFTL area must be formatted before being used. This is done using :c:func:`lftl_format`.

.. code-block:: c
  :linenos:
  :caption: Example: Initial formatting
  :name: Example: Initial formatting

  lftl_format(&nvma);
  lftl_format(&nvmb);


.. note::
  LFTL does not provide a way to know if an area has been already formated or not.
  The application shall track that by maintaining a flag in NVM.

Updating a single variable atomically
--------------------------------------

.. code-block:: c
  :linenos:
  :caption: Example: single variable atomic update
  :name: Example: single variable atomic update

  lftl_write(&nvma,nvm.data0,new_data0,sizeof(new_data0));

Updating several variables atomically
--------------------------------------

.. code-block:: c
  :linenos:
  :caption: Example: multiple variables atomic update
  :name: Example: multiple variables atomic update

  uint8_t transaction_tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  lftl_transaction_start(&nvma, transaction_tracker);
  lftl_write(&nvma,nvm.data0,new_data0,sizeof(new_data0));
  lftl_write(&nvma,nvm.data1,new_data1,sizeof(new_data1));
  lftl_transaction_commit(&nvma);
