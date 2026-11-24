This file will be used once we reach v1.0.0

Template:
- [Fix issue X in feature Y](https://github.com/sebastien-riou/lean-ftl/pull/XXX) (by [YYY](https://github.com/YYY))

Next
---------------------
- The flash accessors have one home, the board support packages of
  [lean-bsp](https://github.com/sebastien-riou/lean-bsp) (`src/bsp_nvm.c`); `target/stm32` is removed. **Breaking** for
  users of the `stm32u5` and `stm32l5` libraries of a release:
  - they are replaced by the device libraries `stm32u5a5xj` and `stm32l552xe`, named after the part, because the bank
    size is now built in (4 MB and 512 KB of flash); another part of the family needs its own build;
  - the erase and program functions run from the section `.RamFunc` instead of `.flash_write_funcs`;
  - `liblean-ftl-test` is no longer built for boards by CMake: a board builds the test suite through lean-bsp.
- New device library `m5531h2ljae` (APROM bank 1 of the M5531H2LJAE), and a header `lftl-device.h` next to each device
  library, with the geometry and the prototypes of the accessors.
- `./fetch-bsps` clones the BSPs a release is built from, at the commits of `lean-bsp/bsps.lock`.


