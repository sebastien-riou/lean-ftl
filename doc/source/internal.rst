Internals
==========================

Description of technical choices.


Conventions
--------------

- API symbols start with ``lftl_`` or ``LFTL_`` prefix.
- "size" is in bytes, "width" is in bits.


Coding style
-------------
Tabs are spaces, and they are 2 characters wide.

Error reporting
---------------
All user functions directly call the error call back when they detect an error, this ensures 
all errors are treated.
This does not force the user to use ``set_jump``: 
the error handler can also just wait for reset or trigger an internal reset.

Expansion of variables to full write units
------------------------------------------
The helper macro ``LFTL_ARRAY`` expand each array item to the width of a write unit.
This wastes storage area but that allows to update each variable independantly without any
buffer in RAM.
If the user desire to optimize storage footprint, the way to go is to use ``LFTL_COMPACT_ARRAY``
and use a buffer in RAM to group write operations as demonstrated in the ``single_area`` example.

Memory layout
---------------

The diagram below present the structure of an LFTL area.
The area contains ``ns`` slots, each containing ``np`` pages.
The data size supported by the area is ``np`` pages minus the size of the meta-data.

.. image:: lean-ftl-area.drawio.svg
