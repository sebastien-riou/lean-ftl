# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

Add the next issue to this table, under the next free `LFTL-nnn` (LFTL-007).

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-006](#lftl-006) | `LFTL_MEMSET_WHOLE_AREA` raises `FIRST_NOT_IN_DATA` unless the current slot is slot 0, and always when the data is a whole number of pages | Low | demonstrated |

---

<a id="lftl-006"></a>
## LFTL-006 - `LFTL_MEMSET_WHOLE_AREA` writes at physical addresses, and past the data

**Severity:** Low (the error is raised before anything is written; nothing is corrupted) ·
**[demonstrated]** on the Linux simulator · found while checking the documentation against the code
(2026-10-22)

**Where.** `LFTL_MEMSET_WHOLE_AREA` in `liblean-ftl/include/lean-ftl.h`, at HEAD.

The macro starts a transaction, then writes page-sized chunks starting at `area.data`. That is the
physical address of the current slot, while `lftl_transaction_write()` takes logical addresses,
from `area.area` to `area.area + data_size`. The two are equal only while the current slot is slot
0, which is the case right after `lftl_format()`. After any update the current slot moves on, and
the first write raises `LFTL_ERROR_FIRST_NOT_IN_DATA`, which aborts the transaction.

The macro then always writes the remainder, `data_size % LFTL_PAGE_SIZE` bytes. When `data_size` is
a whole number of pages, that is a write of 0 bytes at `area.area + data_size`, which
`check_in_data()` also refuses with `FIRST_NOT_IN_DATA`, even right after formatting.

Its documentation promises to "Set the same byte value on an entire LFTL area". The `single_area`
example calls it right after `lftl_format()`, on 40 bytes of data, so it works there.

**Evidence.** A copy of the `single_area` example with a second area of one page of data, on the
Linux simulator (4 KB pages, 2-byte write units):

````
memset right after lftl_format: no error
memset after one lftl_write: error 0x03
memset of a page-sized area right after lftl_format: error 0x03
````

**Fix.** Write from the logical start, `area.area`, and skip the last write when nothing remains. A
regression test can run the macro after a write, and on an area whose data is a whole number of
pages.

---
