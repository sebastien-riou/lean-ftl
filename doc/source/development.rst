Development
==========================

Vision and principles
---------------------------

Consistency and coherence are one of the key characteristics of good software.
While the reality is never black and white, it is important that
contributors are working towards the same high-level goal. This document
attempts to set out the basic principles and the rationale behind
them. If you are contributing or looking to evaluate whether *lean-ftl*
is the right choice for your project, it might be worthwhile to skim through the
section below.

Mission statement
~~~~~~~~~~~~~~~~~~~~~~

*lean-ftl* is the compact and safe library easily portable.


Goals
~~~~~~~~~~~~~~~~~~~~~~

Robustness
^^^^^^^^^^^^^^^^^^^^^^^^^

*lean-ftl* shall cope with sudden power loss at any point except for a few, well identified functions.

**Rational** Embedded systems are typically deployed in a harsh environment: planet Earth, full of careless and/or mal attentioned humans.

Simplicity
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*lean-ftl* shall be as simple as possible to keep the code base minimal.

**Rational** 

- Minimal code base ease code reviews.
- Complexity is the main source of bugs and security vulnerabilities. 

Self-contained
^^^^^^^^^^^^^^^^^^^^^^

*lean-ftl* has no dependencies, it can run on bare-metal.


Portability
^^^^^^^^^^^^^^^^^^^^^^

*lean-ftl* aim at being usable on any MCU.

That said, *lean-ftl* has been tested so far only on little-endian devices.
It has been tested on STM32 MCUs only but it is designed to be portable to 
other MCUs and even handle external NVMs such as QSPI flash, SDcards...

Stable API
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*lean-ftl* will not break backward compatibility unexpectly.

**Rational** *lean-ftl* use `Industry-standard <https://semver.org/>`_ versioning is a basic
requirement for production-quality software. This is especially relevant in IoT
environments where updates may be costly.


Development dependencies
---------------------------

- CMake_ 3.28 or newer
- gcc 11.4.0 or newer
- arm-none-eabi-gcc 13.2.1 or newer

.. _CMake: http://cmake.org/

All other dependencies are related to documentation:

- `Python <https://www.python.org/>`_ and `pipenv <https://pipenv.pypa.io/en/latest/>`_ 
- `Doxygen <http://www.stack.nl/~dimitri/doxygen/>`_
- `Sphinx <http://sphinx-doc.org/>`_ 

Installing *sphinx*
~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

  pipenv install

Further instructions on configuring advanced features can be found at `<http://read-the-docs.readthedocs.org/en/latest/install.html>`_.


Tips and tricks
---------------------------

Live preview of docs
~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

  pipenv run make -C doc livehtml

This updates the html and refresh it in the browser each time something within `doc` folder changes.
If you change something in the sources, use ``pipenv run make -C doc`` to see the effect.


Testing and code coverage
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Please refer to :doc:`tests`

Debug preprocessor macros
~~~~~~~~~~~~~~~~~~~~~~~~~
.. code-block:: bash

  make -C build/linux/ test/source/main.i

This generate the file ``build/linux/CMakeFiles/lean-ftl-test.dir/test/source/main.c.i``

Type the following to see the full list of available targets:

.. code-block:: bash

  make -C build/linux/ help
