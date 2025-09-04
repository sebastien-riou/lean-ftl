# lean-ftl
Minimal flash translation layer meant for embedded systems (wear leveling + anti tearing).

Main features:
- Anti-tearing is exhautively tested via a dedicated emulator
- Avoid fragmentation
- Support transactions
- Easily portable to new MCUs / new flash chips

The library is independent of any NVM controller, porting it to a new MCU means providing 3 simple "accessor" functions:
- nvm_erase
- nvm_write
- nvm_read

This repository provides accessors for the following targets:
- STM32U5xx (testbench is [here](https://github.com/sebastien-riou/lean-ftl-stm32u5/))
- STM32L5xx (testbench is [here](https://github.com/sebastien-riou/lean-ftl-stm32l5/))
- Native execution on Linux (for test and development purposes)

## Concept

### Definition
- An `lftl_ctx_t` manages NVM updates of a single, monolithic, data referred as `data`.
- In NVM, `data` is stored along a `meta_data` record to keep track of which copy of `data` is valid.
- The number of NVM pages necessary to store `data` and `meta_data` is referred as a 'slot'.
- At build time, a fixed area of NVM is allocated exclusively to one `lftl_ctx_t`.
- The NVM area must be a multiple of the slot size. 

### NVM update process
1. The next slot is erased.
2. New `data` is written to next slot in the NVM area of that context.
3. New `meta_data` is written to next slot. From that point, the new `data` is the valid one.

### Data access process
The first time one `lftl_ctx_t` is used since reset, a search take place to identify the valid slot within the NVM area.
The valid slot is the one which hash the highest `version` and which pass the integrity check.

## Transaction API
The concept outlined above assumes the application rewrite the entire `data` everytime.
In practice, most application need to update some locations within `data`.
The transaction API allows just that. It use a buffer in RAM to represent the write units which have been updated.
1. The application call `lftl_transaction_start`, the buffer in RAM is initialized and the next slot is erased.
2. The application call `lftl_transaction_write` one or more times: this write the new data directly in the next slot.
3. The application call `lftl_transcation_commit`, then the other part of `data` are copied to the new slot and the `meta_data` is written.

NOTES:
- During a transaction, `lftl_basic_write` is not allowed.
- During a transaction, `lftl_read` reads the current data. The new data is not read until it is commited.

## Developper info

### Local build
You need CMake and arm-none-eabi or riscv-none-elf toolchain in your path.

```
./buildit on/linux debug
```

Notes: 
- replace `linux` by any other file in the `on` folder to target another platform.
- replace `debug` by `minSizeRel` to build with size optimizations.

### Local test
```
./testit on/linux debug
```

Note: this does not work with embedded targets (each is tested via a dedicated repository).

### Make a new release
Release are created for each tag formatted as `v*.*.*`
```
./publish-release v1.2.3
```

### Make the doc
```
./build-doc
```

## Funding

This project is funded through [NGI0 Commons Fund](https://nlnet.nl/commonsfund), a fund established by [NLnet](https://nlnet.nl) with financial support from the European Commission's [Next Generation Internet](https://ngi.eu) program. Learn more at the [NLnet project page](https://nlnet.nl/project/LeanFTL).

[<img src="https://nlnet.nl/logo/banner.png" alt="NLnet foundation logo" width="20%" />](https://nlnet.nl)
[<img src="https://nlnet.nl/image/logos/NGI0_tag.svg" alt="NGI Zero Logo" width="20%" />](https://nlnet.nl/commonsfund)
