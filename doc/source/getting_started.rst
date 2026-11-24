Getting started
==========================

Using pre-built binaries
------------------------------
Pre built binaries are available in the `release page <https://github.com/sebastien-riou/lean-ftl/releases>`_.
The release package is designed to support cross platform, multi configuration, projects out of the box:

- For each of the platforms below:

  - ARM Cortex-M0
  - ARM Cortex-M3
  - ARM Cortex-M4
  - ARM Cortex-M7
  - ARM Cortex-M23
  - ARM Cortex-M33
  - ARM Cortex-M35P
  - ARM Cortex-M52
  - ARM Cortex-M55
  - ARM Cortex-M85
  - RISC-V RV32IMC
  - STM32U5A5xJ, STM32L552xE and M5531H2LJAE: device libraries, which include the accessors of
    the internal flash (see :ref:`using-device-libraries`)
  - Linux (include RAM based accessors, for dev/debug purposes only)

The test suite runs on the NUCLEO-U5A5ZJ-Q, the NUCLEO-L552ZE-Q, the NuMaker-M5531 and the
Raspberry Pi Pico 2 as a `lean-bsp <https://github.com/sebastien-riou/lean-bsp>`__ application
(``app.toml``; ``test/HWBRIDGE.md``). The accessors live in the board support packages of
lean-bsp; the device libraries are built from them.

- The following binaries are included:

  - Debug 
  - Size optimized 


Building
------------------------------

Prerequisites:
 - Recent compilers (tested on ``gcc (Ubuntu 11.4.0-1ubuntu1~22.04) 11.4.0`` and ``arm-none-eabi-gcc (xPack GNU Arm Embedded GCC x86_64) 14.2.1 20241119``)
 - CMake_ 3.30 or newer (might also be called ``cmakesetup``, ``cmake-gui`` or ``ccmake`` depending on the installed version and system)
 - C build system CMake can target (make, Apple Xcode, MinGW, ...)

.. _CMake: http://cmake.org/

This builds for all supported platforms, result is in "dist" folder. The device libraries take
their accessors from the board support packages of lean-bsp, which ``./fetch-bsps`` clones next to
the repository at the commits of ``lean-bsp/bsps.lock`` (Python 3.11 or newer is needed to build
them):

.. code-block:: bash

  ./fetch-bsps
  ./build-release

If you want to build just for linux, result is in "build/linux" folder:

.. code-block:: bash

  ./buildit on/linux debug


Linking with lean-ftl
---------------------

You can verify support for your platform by compiling main.c from the example/hello folder:

.. code-block:: c

  #include <lean-ftl.h>
  #include <stdio.h>

  int main(int argc, char * argv[])
  {
    printf("Hello from lean-ftl %s\n", lftl_version());
  }


.. code-block:: bash

  tar xf ~/Downloads/lean-ftl-v0.2.0.tar.gz
  tar xf dist/liblean-ftl-debug.tar.gz
  gcc examples/hello/main.c -o hello -Iliblean-ftl/include \
    -Lliblean-ftl/build/linux/liblean-ftl -llean-ftl
  ./hello

You should get something like:

.. code-block:: bash

  Hello from lean-ftl 0.2.0


Troubleshooting
---------------------

**lean-ftl.h: No such file or directory**: ``lean-ftl.h`` is probably not in your include path. 
If you are using cmake, the folder which contains ``lean-ftl.h`` shall appear in the call to ``target_include_directories``.

**cannot find -llean-ftl: No such file or directory**: ``liblean-ftl.a`` is probably not in your library include path.
If you are using cmake, the folder which contains ``liblean-ftl.a`` shall appear in the call to ``target_link_libraries``.

