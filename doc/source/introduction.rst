Introduction
===============

Overview
--------
*lean-ftl* is a C library to provide a "Flash Translation Layer"
on bare metal MCU applications. More precisely it provides two 
functionalities:

- wear leveling
- anti tearing


Main features:

- No runtime dependencies, small footprint
- No fragmentation, ideal for system with real time constraints 
- Extensive test suite including simulated tearing tests

.. note:: This documentation and the source code use "LFTL" as an abreviation 
  for *lean-ftl*.

Key concepts
-------------

Write unit 
^^^^^^^^^^^^
That's the minimum size of the write operation supported by the target NVM.
Common values are 4 bytes, 8 bytes and 16 bytes.
This is often called "programming size" or "write width" in datasheets.

Erase unit 
^^^^^^^^^^^^
That's the minimum size of the erase operation supported by the target NVM.
Common values are 2048 bytes, 4096 bytes and 8192 bytes.
This is often called "page" or "sector" in datasheets.

Meta data
^^^^^^^^^^
*lean-ftl* use 3 write units of meta data to perform its function.
If the write unit is smaller than 4 bytes, then meta data take 12 bytes.

Slot
^^^^^^
An LFTL slot contains:

- Application data 
- *lean-ftl* meta data

What application data, and therefore the total size of a slot,
is something decided by the integrator. The only constraint is that  
the size of a slot must be a multiple of the erase unit. 

Area
^^^^^^^
An LFTL area contains 2 or more slots. At any given time, a single
slot is used to store the current data. 
Other slots are spare storage space for the wear level and anti tearing
mechanisms.

.. note:: An LFTL area contains a single type of slot (slots of the same size)
  however an application can use any number of LFTL areas and 
  each can use a different type of slot.
