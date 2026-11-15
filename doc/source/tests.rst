.. _tests:

Tests
=============

Unit tests
--------------

There is a comprehensive test suite . You can run all of them on linux using ``testit`` script.

.. code-block:: bash

  ./testit on/linux debug


or, for faster execution if you do not need debug:

.. code-block:: bash

  ./testit on/linux

After the test suite, ``testit`` also builds and runs the examples, then the AddressSanitizer harness
of the threat model (``test/asan/build-and-run``).

As more and more tests are added, single threaded run time is getting longer, so CLI arguments have been added 
to select which tests to run.

Use `--n-tests` to get the total number of tests:

.. code-block:: bash

  build/linux/lean-ftl-test-fw --n-tests


Use `--test=` to select a single test. For example, to run test number 6: 

.. code-block:: bash

  build/linux/lean-ftl-test-fw --test=6


Use `--first=` and `--last=` to specify a range of tests. For example, to run test number 3 to 6:

.. code-block:: bash

  build/linux/lean-ftl-test-fw --first=3 --last=6


Most tests are exhautively tested vs. tearing but a few are not by default.
The CLI argument `--tear-cov=` allow to specify the tearing coverage for the selected tests (between 1 and 100).
For example, to run test number 9 with 50% tearing coverage:

.. code-block:: bash

  build/linux/lean-ftl-test-fw --test=9 --tear-cov=50


.. note::
  When the coverage is less than 100%, the part which is not tested against tearing are the first operations.
  **Rational** All test cases starts from the same state, there is more diversity at the end.

Use `--lib-info` to print the library version and build type, and `--test-callbacks` to check the
NVM callbacks (erase, write and read back the whole test NVM) before the tests, a first check of
R1 to R5 of :ref:`flash-required`.

``--no-pinning`` runs the suite as on a flash without the all-zero overwrite (N3 of
:ref:`flash-not-relied-on`): the simulator refuses it and every test NVM is declared without
``zero_overwrite``, so the scan of a regular area never pins; ``testit`` runs the suite both ways.

The simulator models a *weak word* in the EWLF areas, T2 of :ref:`flash-tearing` without T3: the
write unit a cut lands in reads differently at every boot, as programmed, all erased, all zeros
or under a random mask, for ever, until a program or an erase makes it solid again; the units of
the operation the cut never reached stay as they were, and an interrupted erase leaves every
write unit of its pages weak. A weak unit is not erased, whatever it reads: the simulator
refuses a program over it with :c:macro:`LFTL_NVM_STATUS_REFUSED`, zeros excepted where the
flash has the all-zero overwrite. The model is on in every mode; ``--no-weak-words`` turns it
off, ``--weak-seed=N`` seeds its readings, which are reproducible from the seed and the cut.
With it, every cut is followed by two boots, and an area that reads the new state at one boot and
the old one at a later boot fails the run as a rollback; and when the boots that recover from a cut
program anything, a pin of a regular area or the repair of an EWLF area, the same cut is run again
with a second cut at each of those programs, at its first write unit and at its last.
``--tear-at=N`` runs one cut only, to reproduce a failure the run names. The regular areas keep
the older model, a fixed value past the cut, since they rely on T3.

The tests of EWLF areas, beyond the sweeps:

- test 33 builds by hand, with programs of erased units only, an update cut after each of the
  words it programs, and checks what the scan makes of each: nothing written by the scan, the
  repair that is pending or not, reads and writes refused while it is, the unproven read, the
  version shown, and the boots after it;
- test 39 scripts the readings of weak words: a weak confirm and a weak entry read one way then
  the other, a first entry of a log switch that a boot can see, the sequence of cuts the
  certificates exist for, and the stuck state reached after exactly as many cuts as the area has
  certificates, with the remaining cuts counted down;
- test 40 is a random sequence of writes, cuts, boots and cut repairs, on two geometries: one
  that switches its log every few writes, one with a single certificate that gets stuck. After
  every boot the data read is one of the last writes, never older than one already shown, never
  older than the last write that completed, and never a write that was cut once an older version
  was shown.

Tests 35, 39 and 40 need the simulator and are empty on a board. ``test/ewlf-only <toolchain
file> [preset]`` builds and runs the tests that involve EWLF areas only, on the three flash
models, then test 37 and the sanitizer tests of EWLF areas; ``on/linux-u5`` is the Linux build
with the geometry of the STM32U5, 8 KB erase units and 16-byte write units.

The design of EWLF areas is checked apart from its implementation, by an exhaustive model
checker in ``test/ewlf-model``: :ref:`design-ewlf-checked`. For instance:

.. code-block:: bash

  cd test/ewlf-model
  python3 ewlf_model.py --protocol v4 --cuts 3 --boots 6 --geometry 2,2,2,2,2,2 --strict --ignore stuck

Compatibility with v0.1.0
--------------------------

The on-flash format of regular areas has not changed since lean-ftl v0.1.0 (EWLF areas came
later, and their format changed in v0.3.0 and again in v0.4.0: an EWLF area written by an earlier
version must be formatted again).
Test 37 (``v010_compat_test``) checks it on every build: it rebuilds, in one of the test
areas, images that v0.1.0 formatted and wrote (``liblean-ftl-test/source/v010_images.h``), reads
each one as after a reset and checks that the scan settles on the slot v0.1.0 had current,
programs and erases nothing, and reads back the data v0.1.0 read; then that the area still takes
updates. ``test/v010-compat/gen-images.sh`` regenerates the header: it builds
``test/v010-compat/harness.c`` against the sources of the ``v0.1.0`` tag taken from git and
against the working tree, generates the images for every geometry of the test suite, checks that
the working tree reads them, and writes them out as C. Add a geometry there when the test suite
gains one.

Code coverage
--------------

``./coverage`` builds for linux with gcov instrumentation, runs the test suite with
``--tear-cov=1`` and writes an HTML report to ``build/linux/coverage-html/index.html``
(it needs ``gcovr``, installed by ``pipenv install``).
