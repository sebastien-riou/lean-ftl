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
``zero_overwrite``, so the scan never pins; ``testit`` runs the suite both ways. The Linux
build also defines ``LFTL_SCAN_CROSS_CHECK``: every scan of an EWLF area runs both the fast search
and the linear pass of :ref:`design-ewlf-scan`, and raises ``LFTL_INTERNAL_ERROR`` if they
disagree.

The simulator models a *weak word* in the EWLF areas, T2 of :ref:`flash-tearing` without T3: the
write unit a cut lands in reads differently at every read, as programmed, all erased, all zeros
or under a random mask, for ever, until a program or an erase makes it solid again; the units of
the operation the cut never reached stay as they were. It is on by default when ``zero_overwrite``
is on and off under ``--no-pinning``, where the stuck state of the EWLF scan is a documented
residual that the sweep would report as a rollback; ``--weak-words`` and ``--no-weak-words``
force it, ``--weak-seed=N`` seeds its readings, which are reproducible from the seed and the cut.
With it, every cut is followed by two boots, and an area that reads the new state at one boot and
the old one at a later boot fails the run as a rollback; and when the boots that recover from a cut
program anything (a tombstone, a copy, a pin), the same cut is run again with a second cut at each
of those programs, at its first write unit and at its last. ``--tear-at=N`` runs one cut only, to
reproduce a failure the run names. The regular areas keep the older model, a fixed value past the
cut, since they rely on T3. Tests 39 and 40 build the sequences of cuts and readings that the
residual table of :ref:`design-residual` rests on, and a random sequence of writes, cuts and
boots.

Compatibility with v0.1.0
--------------------------

The on-flash format of regular areas has not changed since lean-ftl v0.1.0 (EWLF areas came
later, and their format changed in v0.3.0: an EWLF area written by v0.2.0 must be formatted again).
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
