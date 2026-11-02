# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

Add the next issue to this table, under the next free `LFTL-nnn` (LFTL-015).

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-014](#lftl-014) | The checksum of regular areas is a 27-bit CRC | Low | **[demonstrated]** |

LFTL-001 to LFTL-013 are fixed; their entries are in the git history of this file,
the last three (LFTL-010, LFTL-011, LFTL-013) closed by the read-only scan ([documentation](https://lean-ftl.readthedocs.io/en/latest/design.html)).

<a id="lftl-014"></a>
## LFTL-014: the checksum of regular areas is a 27-bit CRC

**Status: [demonstrated]**, 2026-11-02.

`crc32c()` in `ftl_internal.h` is a reflected (LSB-first) CRC loop, which needs the reflected form
of the Castagnoli polynomial, `0x82F63B78`. It uses `0x05EC76F1`, the reciprocal form. The top 5
bits of that constant are zero, so every shift clears the top of the register and the result never
has a bit above bit 26: over random inputs the OR of all outputs is `0x07FFFFFF`, and `"123456789"`
gives `0x04B38042` where CRC32C gives `0xE3069283`. The checksum of a slot is therefore a 27-bit CRC
plus the version, with a non-standard polynomial.

**Effect.** A torn or forged slot passes the check with probability 2^-27 instead of 2^-32, and the
guarantees of a 32-bit CRC on burst errors do not hold. Anti-tearing does not rest on the checksum's
strength (the end marker and the write order do that), so the practical effect is the acceptance
rate of garbage, which stays small.

**Why it stays.** v0.1.0 has the same code, the pqkey fleet holds areas in that format, and test 37
reads them on every build: changing the polynomial is a format change for regular areas, which are
frozen. EWLF areas, whose format changed in v0.3.0, use a CRC-64/NVME instead. A second regular-area
format, with a reader for the first, would fix it; not planned.
