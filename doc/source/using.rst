User guide
=======================

Header to include
---------------------

The ``lean-ftl.h`` header includes all the symbols. 
This file is the same no matter the build type that you use.
It is therefore safe to use ``dist/debug/lean-ftl.h`` even
if you link against the ``minSizeRel`` library.

Enabling helper macros
---------------------------------------
``lean-ftl.h`` contains few helper macros that ease the 
declaration of your LFTL areas.
To use those helper macros, you must define 3 macros before including
``lean-ftl.h``:

- LFTL_DEFINE_HELPERS: Indicates that you want to use the helper macros. Value does not matter
- FLASH_SW_PAGE_SIZE: Defines the size for "flash software page". Value shall be the minimum erase size.
- WU_SIZE: Defines the size for "write unit". Value shall be the minimum write size in the NVM.

.. code-block:: c
  :linenos:
  :caption: Example: Enabling helper macros for STM32U5
  :name: Example: Enabling helper macros for STM32U5

  #define LFTL_DEFINE_HELPERS
  #define FLASH_SW_PAGE_SIZE (8*1024)
  #define WU_SIZE 16 
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
      
  } __attribute__ ((aligned (FLASH_SW_PAGE_SIZE))) data_flash_t;
  data_flash_t nvm;

.. note:: In the example above, ``data0`` is accessible via ``nvm.a.data0``.

.. note:: Members declared inside an LFTL area can be of any type however 
  they should be padded to occupy a multiple of the ``WU_SIZE``.

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
The integrator shall declare one :type:`lftl_nvm_props_t` for each targeted NVM.

.. code-block:: c
  :linenos:
  :caption: Example: Declaring NVM properties
  :name: Example: Declaring NVM properties

  lftl_nvm_props_t nvm_props = {
    .base = &nvm,
    .size = sizeof(nvm),
    .write_size = nvm_write_size,
    .erase_size = nvm_erase_size,
  };

.. note:: ``base`` and ``size`` can be a subset of the physical NVM.

Implementing the callbacks
-----------------------------
In order to use LFTL, the following callbacks needs to be implemented
on your target platform:

- :type:`nvm_erase_t`
- :type:`nvm_write_t`
- :type:`nvm_read_t`
- :type:`error_handler_t`

You can find an implementation of those callbacks for STM32U5 and STM32L5 in 
https://github.com/sebastien-riou/lean-ftl/tree/main/target/stm32

Declaring LFTL areas 
----------------------
Each LFTL area has its volatile context maintained in a
:type:`lftl_ctx_t` struct.

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
    .erase = nvm_erase,
    .write = nvm_write,
    .read = nvm_read,
    .error_handler = throw_exception,
    .transaction_tracker = LFTL_INVALID_POINTER
  };
  lftl_ctx_t nvmb = {
    .nvm_props = &nvm_props,
    .area = &nvm.b_pages,
    .area_size = sizeof(nvm.b_pages),
    .data = LFTL_INVALID_POINTER,
    .data_size = sizeof(nvm.b_data),
    .erase = nvm_erase,
    .write = nvm_write,
    .read = nvm_read,
    .error_handler = throw_exception,
    .transaction_tracker = LFTL_INVALID_POINTER
  };

Initial formatting
------------------------
Each LFTL area must be formatted before being used. This is done using the :func:`lftl_format`.

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

  lftl_write(&nvma,nvm.a.data0,new_data0,sizeof(new_data0));

Updating several variables atomically
--------------------------------------

.. code-block:: c
  :linenos:
  :caption: Example: multiple variables atomic update
  :name: Example: multiple variables atomic update

  uint8_t transaction_tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  lftl_transaction_start(&nvma, transaction_tracker);
  lftl_write(&nvma,nvm.a.data0,new_data0,sizeof(new_data0));
  lftl_write(&nvma,nvm.a.data1,new_data1,sizeof(new_data1));
  lftl_transaction_commit(&nvma);
