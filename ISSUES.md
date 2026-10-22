# lean-ftl - issues

This file is lean-ftl's issue tracker. Each entry has a permanent `LFTL-nnn` identifier under an
explicit `<a id="lftl-nnn">` anchor, so links to it survive edits; numbers are never reused, and an
entry is removed once its fix is in (git history keeps it).

**Status tags:** **[demonstrated]** reproduced and quoted; **[confirmed]** read line by line in the
source; **[inferred]** follows from the source and a hardware rule, but has not been observed on a
board.

| ID | Issue | Severity | Status |
|---|---|---|---|
| [LFTL-004](#lftl-004) | The tearing sweep never writes after recovering from a tear, so it cannot find bugs in the write that follows a power cut | Low | confirmed |

---

<a id="lftl-004"></a>
## LFTL-004 - The tearing sweep never writes after recovering from a tear

**Severity:** Low (test coverage, no known bug) · **[confirmed]** · moved from `THREAT-MODEL.md`
(2026-10-22)

**Where.** `exception_handler()` and `tearing_sim_check_nvm()` in
`liblean-ftl-test/source/test.c`, and the Linux simulator in `target/linux/linux-accessors.c`.

After each simulated tear, the test simulates a reset, reads the regular and EWLF areas back and
compares them with the reference model, then formats every area before the next tear. No write
ever lands on a state a tear left behind, so the sweep cannot find bugs in the first write after a
power cut. LFTL-002 was one: the write after a torn EWLF record programmed over it. Tests 24 and 29
cover the known cases by building those states by hand, but nothing covers the class in general.

The simulator also models a single kind of flash: it refuses to program a write unit that is not
erased, like STM32U5-class flash. On NOR flash, where programming can only clear bits, the same bug
gives silent corruption instead of an error, and nothing in the suite models that.

**Fix.** In `exception_handler()`, after `tearing_sim_check_nvm()`, write new data to every area
through the reference model, then check again, before formatting. Optionally, give the simulator a
mode where programming ANDs bits instead of refusing. Both make the sweep slower; the write after
recovery could be limited to tests that already run with full tearing coverage.
