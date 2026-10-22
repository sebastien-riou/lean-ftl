# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-005](#lftl-005) | In an EWLF area whose data size is not a whole number of write units, any write that touches the last, partial write unit raises `LAST_NOT_IN_DATA`, including a write of the whole data | Medium | demonstrated |

---

<a id="lftl-005"></a>
## LFTL-005 - EWLF writes cannot reach the last partial write unit of the data

**Severity:** Medium (configuration-dependent; the error is raised, nothing is corrupted) ·
**[demonstrated]** on the Linux simulator · found while running the test suite with 16-byte write
units (2026-10-22)

**Where.** `write_ewlf_core()` in `liblean-ftl/source/ftl_internal.h`, at HEAD and since EWLF areas
were added (501de63).

`write_ewlf_core()` checks the requested range against `data_size`, then extends it to whole write
units and translates that extended range, which `translate_addr_ewlf()` checks against `data_size`
again. When `data_size` is not a multiple of the write unit, a range that ends in the last, partial
write unit is extended past `data_size` and refused with `LFTL_ERROR_LAST_NOT_IN_DATA`. The record
itself has room for it: `ewlf_data_phy_size()` rounds the data up to whole write units. EWLF areas
are written whole, so such an area cannot be written at all; reads work.

Regular areas had the same problem; registration now refuses a regular area whose data is not whole
write units, but accepts such an EWLF area.

**Evidence.** `ewlf_tearing_test` used 1000 bytes of data. With the 16-byte write units of the
STM32U5 build, its first `lftl_write()` of the whole data raised `0x4` (`LAST_NOT_IN_DATA`). The
test now rounds its data size to whole write units.

**Fix.** Either refuse such an EWLF area at registration, as for regular areas (simplest, and the
error shows at boot instead of on the first write), or check the extended range against the
rounded data size (`ewlf_data_phy_size()`) while still checking the requested range against
`data_size`, so that any data size works.
