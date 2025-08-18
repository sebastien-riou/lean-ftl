Getting started
==========================

Using pre-built binaries
------------------------------
Pre built binaries are available in the `release page <https://github.com/sebastien-riou/lean-ftl/releases>`_.
The release package is designed to support cross platform, multi configuration, projects out of the box:

- For each of the platforms below:

  - Linux 
  - STM32U5
  - STM32L5

- The following binaries are included:

  - Debug 
  - Size optimized 


Building
------------------------------

Prerequisites:
 - Recent compilers (tested on ``gcc (Ubuntu 11.4.0-1ubuntu1~22.04) 11.4.0`` and ``arm-none-eabi-gcc (xPack GNU Arm Embedded GCC x86_64) 13.2.1``)
 - CMake_ 3.28 or newer (might also be called ``cmakesetup``, ``cmake-gui`` or ``ccmake`` depending on the installed version and system)
 - C build system CMake can target (make, Apple Xcode, MinGW, ...)

.. _CMake: http://cmake.org/

This builds for all supported platforms, result is in "dist" folder:

.. code-block:: bash

  ./build_release

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
    printf("Hello from lean-ftl %lu\n", lftl_version_timestamp());
  }


.. code-block:: bash

  7z x  ~/Downloads/lean-ftl-v0.0.4.7z
  gcc examples/hello/main.c -o hello -Idist/debug/liblean-ftl/include \
    -Ldist/debug/liblean-ftl/build/linux/liblean-ftl -llean-ftl 
  ./hello

You should get something like:

.. code-block:: bash

  Hello from lean-ftl 0.0.4


Troubleshooting
---------------------

**lean-ftl.h: No such file or directory**: ``lean-ftl.h`` is probably not in your include path. 
If you are using cmake, the folder which contains ``lean-ftl.h`` shall appear in the call to ``target_include_directories``.

**cannot find -llean-ftl: No such file or directory**: ``liblean-ftl.a`` is probably not in your library include path.
If you are using cmake, the folder which contains ``liblean-ftl.a`` shall appear in the call to ``target_link_libraries``.

