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
