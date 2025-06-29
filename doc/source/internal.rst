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

Memory layout
---------------
TODO
