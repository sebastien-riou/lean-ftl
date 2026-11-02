/*
 * asan_harness.c - reproduces LF-1 .. LF-5 and LF-8 .. LF-12 from the
 * lean-ftl threat model.
 * Target: lean-ftl, liblean-ftl/source/ftl.c and ftl_internal.h, from commit
 * 501de63 (2026-08-31) on
 *
 * NVM model: 8 pages x 256 bytes in RAM, memcpy callbacks, error handler that
 * longjmps (as the lean-ftl docs suggest). The callbacks print a line whenever
 * they are handed a range outside the area under test, then do the access
 * anyway so ASan reports it. Tests 7 to 10 also give the write callback NOR
 * semantics (programming ANDs bits) or STM32U5 semantics (a non-erased cell
 * cannot be programmed), and tests 7 to 11 cut power by dropping a chosen
 * write or erase.
 *
 * Build and run every test, checking each outcome (from the lean-ftl clone):
 *   test/asan/build-and-run
 * or build by hand:
 *   gcc -g -fsanitize=address,undefined \
 *       -Iliblean-ftl/include \
 *       -DGIT_VERSION=harness -DVERSION_TIMESTAMP=0 -DBUILD_TYPE=debug \
 *       test/asan/asan_harness.c liblean-ftl/source/ftl.c -o asan_harness
 * and run one test:
 *   ./asan_harness <1..13>
 *
 * Expected at 501de63, before the fixes:
 *   1-6, 13  an ASan report
 *   7        reads back d2 & d3 after writing d3
 *   8, 10    error_handler(0x201) on every write after the reset
 *   9        reads back v5 & v6 after writing v6, then error_handler(0x2) after a reset
 *   11       reads back the forged record, neither d1 nor d2
 *   12       a write 16 bytes below the area
 * Expected since 5517f9a, with all of them fixed: every test ends in
 * "error_handler(...)", or reads back the newest data, and no callback reports
 * an access outside the area. Tests 1-3 raise a range error, test 12 is refused
 * at registration (0xc), test 13 finds no valid record (0x2).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include "lean-ftl.h"

#define PG 256 /* erase unit */
#define WU 16  /* write unit */

static uint8_t flash[8 * PG] __attribute__((aligned(PG)));
static jmp_buf jb, pc;
static lftl_ctx_t ctx;
static uint8_t *trk5;
static uintptr_t area_lo, area_hi;
static enum { RAM, NOR_AND, NO_REPROGRAM } mode = RAM;
static int n_ops, cut_at = -1; /* power cut: write or erase number cut_at, counted from n_ops = 0, never happens */

static void chk(const char *op, const void *p, uintptr_t z) {
  const uintptr_t a = (uintptr_t)p;
  if (a < area_lo || a > area_hi || z > area_hi - a)
    printf("  [%s] outside the area: area%+ld, 0x%lx bytes\n", op, (long)(a - area_lo), (unsigned long)z);
}
static uint8_t er(void *b, unsigned n) {
  chk("erase", b, (uintptr_t)n * PG);
  if (n_ops++ == cut_at) { printf("  power cut, erase of area%+ld dropped\n", (long)((uintptr_t)b - area_lo)); longjmp(pc, 1); }
  memset(b, 0xFF, n * PG);
  return 0;
}
static uint8_t rd(void *d, const void *const s, uintptr_t z) { chk("read", s, z); memcpy(d, s, z); return 0; }
static uint8_t wr(void *d, const void *const s, uintptr_t z) {
  chk("write", d, z);
  if (n_ops++ == cut_at) { printf("  power cut, write to area%+ld dropped\n", (long)((uintptr_t)d - area_lo)); longjmp(pc, 1); }
  uint8_t *d8 = d; const uint8_t *s8 = s;
  if (mode == NO_REPROGRAM)
    for (uintptr_t i = 0; i < z; i++) if (d8[i] != 0xFF) return 1; /* PROGERR */
  for (uintptr_t i = 0; i < z; i++) d8[i] = mode == NOR_AND ? d8[i] & s8[i] : s8[i];
  return 0;
}
static void eh(uint32_t e) { printf("  error_handler(0x%x)\n", e); longjmp(jb, 1); }

static lftl_nvm_props_t props = { .base = flash, .size = sizeof(flash), .write_size = WU, .erase_size = PG,
                                  .erase = er, .write = wr, .read = rd, .error_handler = eh,
                                  .next = LFTL_INVALID_POINTER };

static void init_area(uint32_t write_size, uint8_t *area, unsigned pages, uintptr_t data_size, int ewlf) {
  props.write_size = write_size;
  area_lo = (uintptr_t)area; area_hi = area_lo + pages * PG;
  ctx = (lftl_ctx_t){ .nvm_props = &props, .area = area, .area_size = pages * PG,
                      .data = LFTL_INVALID_POINTER, .data_size = data_size,
                      .transaction_tracker = LFTL_INVALID_POINTER,
                      .next = LFTL_INVALID_POINTER };
  lftl_init_lib();
  if (ewlf) lftl_register_area_ewlf(&ctx); else lftl_register_area(&ctx);
}
static void init(uint32_t write_size) { init_area(write_size, flash, 8, 64, 0); }

/* same CRC core as ftl_internal.h: CRC-64/NVME; an EWLF record's checksum is
   ~crc64(crc64(-1, version little endian), data) */
static uint64_t crc64(uint64_t crc, const uint8_t *b, unsigned n) {
  while (n--) { crc ^= *b++; for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0x9A6C9329AC4BC9B5ull & -(crc & 1)); }
  return crc;
}
static uint64_t ewlf_checksum(uint32_t version, const uint8_t *data, unsigned n) {
  uint8_t v[4] = { (uint8_t)version, (uint8_t)(version >> 8), (uint8_t)(version >> 16), (uint8_t)(version >> 24) };
  return ~crc64(crc64(~(uint64_t)0, v, 4), data, n);
}
/* a meta field: version, checksum, little endian, zeros up to the write unit */
static void ewlf_meta(uint8_t *m, unsigned size, uint32_t version, uint64_t checksum) {
  memset(m, 0, size);
  for (int i = 0; i < 4; i++) m[i] = (uint8_t)(version >> (8 * i));
  for (int i = 0; i < 8; i++) m[4 + i] = (uint8_t)(checksum >> (8 * i));
}
static void dump(const char *name, const uint8_t *b, unsigned n) {
  printf("  %s:", name); for (unsigned i = 0; i < n; i++) printf(" %02x", b[i]); printf("\n");
}

/* EWLF tests 7 to 11 use 8-byte write units and 64-byte data in 2 pages: a
   record is 8 (start marker) + 16 (meta field) + 8 (flag-a) + 8 (flag-b) + 64
   = 104 bytes, 2 per page. format puts v1 at area+408, the next write v2 at
   area+304, and the one after wraps to the top of page 0, area+152. A write
   programs the start marker, the data, the meta field, flag-a, then flag-b; a
   wrap erases the unit it enters first. */
static void ewlf_two_writes_then_torn(const uint8_t *d1, const uint8_t *d2) {
  memset(flash, 0xFF, sizeof(flash));
  lftl_format(&ctx);
  lftl_write(&ctx, flash, d1, 64);
  n_ops = 0; cut_at = 1; /* op #0 writes the start marker, #1 the data, #2 the meta field */
  if (!setjmp(pc)) lftl_write(&ctx, flash, d2, 64);
  cut_at = -1;
  ctx.data = LFTL_INVALID_POINTER; /* reset */
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <1..13>\n", argv[0]); return 2; }
  const int t = atoi(argv[1]);
  uint8_t buf[64] = {0};
  setvbuf(stdout, NULL, _IONBF, 0); /* keep our lines ahead of the ASan report */

  switch (t) {
  case 1: { /* LF-1: tracker indexed before the range check */
    init(WU); if (setjmp(jb)) return 0;
    lftl_format(&ctx);
    uint8_t *trk = malloc(LFTL_TRANSACTION_TRACKER_SIZE(&ctx));
    lftl_transaction_start(&ctx, trk);
    printf("T1/LF-1: transaction_write with dst = area + 4096*WU\n");
    lftl_transaction_write(&ctx, flash + 4096 * WU, buf, WU);
    break;
  }
  case 2: { /* LF-2: offset + size wraps in translate_addr() */
    init(WU); if (setjmp(jb)) return 0;
    lftl_format(&ctx);
    const uintptr_t size = (uintptr_t)0 - 16; /* offset 16 + size wraps to 0 */
    printf("T2/LF-2: lftl_read src = area + 16, size = -16\n");
    lftl_read(&ctx, buf, flash + 16, size);
    break;
  }
  case 3: { /* LF-3: lftl_transaction_read() has no range check */
    init(WU); if (setjmp(jb)) return 0;
    lftl_format(&ctx);
    uint8_t *trk = malloc(LFTL_TRANSACTION_TRACKER_SIZE(&ctx));
    lftl_transaction_start(&ctx, trk);
    printf("T3/LF-3: transaction_read of 256 bytes into a 64-byte buffer\n");
    lftl_transaction_read(&ctx, buf, flash, 4 * sizeof(buf));
    break;
  }
  case 4: { /* LF-5: metadata buffer sized for 16-byte write units */
    init(32); if (setjmp(jb)) return 0;
    printf("T4/LF-5: lftl_format with write_size = 32\n");
    lftl_format(&ctx);
    break;
  }
  case 5: { /* LF-4: stale transaction_tracker after the handler unwinds */
    init(WU);
    if (setjmp(jb)) {
      free(trk5); /* recovery path releases the tracker, as a stack frame would */
      printf("  recovered; ctx.transaction_tracker %s\n", ctx.transaction_tracker == LFTL_INVALID_POINTER ? "cleared" : "still set");
      if (setjmp(jb)) return 0;
      lftl_write(&ctx, flash, buf, WU); /* routed to transaction_write -> UAF */
      break;
    }
    lftl_format(&ctx);
    trk5 = malloc(LFTL_TRANSACTION_TRACKER_SIZE(&ctx));
    lftl_transaction_start(&ctx, trk5);
    printf("T5/LF-4: duplicate write in a transaction, then a plain write after recovery\n");
    lftl_transaction_write(&ctx, flash, buf, WU);
    lftl_transaction_write(&ctx, flash, buf, WU); /* TRANSACTION_OVERWRITE */
    break;
  }
  case 6: { /* LF-8: transaction_start uses ctx->data before anything located the current slot */
    init(WU); if (setjmp(jb)) return 0;
    lftl_format(&ctx);
    ctx.data = LFTL_INVALID_POINTER; /* reset: ctx is back to its documented initial state */
    uint8_t *trk = malloc(LFTL_TRANSACTION_TRACKER_SIZE(&ctx));
    printf("T6/LF-8: lftl_transaction_start as the first call on the area after a reset\n");
    lftl_transaction_start(&ctx, trk);
    break;
  }
  case 7: case 8: { /* LF-9: the write after a torn write programs over the torn cells */
    mode = t == 7 ? NOR_AND : NO_REPROGRAM;
    init_area(8, flash, 2, 64, 1);
    uint8_t d1[64], d2[64], d3[64];
    memset(d1, 0x11, 64); memset(d2, 0x0F, 64); memset(d3, 0x3C, 64);
    if (setjmp(jb)) {
      if (setjmp(jb)) return 0;
      printf("  second attempt:\n");
      lftl_write(&ctx, flash, d3, 64);
      break;
    }
    printf("T%d/LF-9: EWLF on %s flash: write d1, power cut in d2 before its meta field, reset, write d3\n",
           t, t == 7 ? "NOR (program = AND)" : "STM32U5-like (no reprogramming)");
    ewlf_two_writes_then_torn(d1, d2);
    lftl_read(&ctx, buf, flash, 64); dump("after reset ", buf, 8);
    lftl_write(&ctx, flash, d3, 64);
    lftl_read(&ctx, buf, flash, 64); dump("after d3    ", buf, 8);
    printf("  expected d3 (3c ..), d2 & d3 = %02x\n", 0x0F & 0x3C);
    break;
  }
  case 9: case 10: { /* LF-9: power cut in the wrap from page 0 back to the top page */
    mode = t == 9 ? NOR_AND : NO_REPROGRAM;
    init_area(8, flash, 2, 64, 1);
    uint8_t d[64];
    if (setjmp(jb)) {
      if (setjmp(jb)) return 0;
      printf("  second attempt:\n");
      lftl_write(&ctx, flash, d, 64);
      break;
    }
    printf("T%d/LF-9: EWLF on %s flash: v2..v4, power cut in v5 before its flag-b, reset, write v6\n",
           t, t == 9 ? "NOR (program = AND)" : "STM32U5-like (no reprogramming)");
    memset(flash, 0xFF, sizeof(flash));
    lftl_format(&ctx);                                     /* v1 at area+408 */
    for (int v = 2; v <= 4; v++) { memset(d, 0x11 * v, 64); lftl_write(&ctx, flash, d, 64); } /* +304, +152, +48 */
    memset(d, 0x5A, 64);
    n_ops = 0; cut_at = 5; /* v5 wraps to area+408: erase page 1, start marker, data, meta field, flag-a, then flag-b;
                               v5 is left half, and the scan after the reset copies it forward to area+304 */
    if (!setjmp(pc)) lftl_write(&ctx, flash, d, 64);
    cut_at = -1;
    ctx.data = LFTL_INVALID_POINTER; /* reset */
    lftl_read(&ctx, buf, flash, 64); dump("after reset ", buf, 8);
    memset(d, 0xA5, 64);
    lftl_write(&ctx, flash, d, 64);
    lftl_read(&ctx, buf, flash, 64); dump("after v6    ", buf, 8);
    ctx.data = LFTL_INVALID_POINTER; /* reset */
    printf("  reset, then lftl_read\n");
    lftl_read(&ctx, buf, flash, 64); dump("after reset ", buf, 8);
    break;
  }
  case 11: { /* LF-10: attacker data + one power cut forge a record */
    init_area(8, flash, 2, 64, 1); if (setjmp(jb)) return 0;
    uint8_t d1[64], d2[64];
    memset(d1, 0x11, 64);
    /* d2 is torn at area+152; its data starts at area+192. A record forged at
       d2[0] has its header in d2[0..40) and its data at area+232..296, i.e.
       d2[40..64) followed by 40 bytes of page 1 that are still erased. */
    uint8_t forged[64];
    memset(forged, 0xFF, sizeof(forged));
    memcpy(forged, "attacker-chosen 24 bytes", 24);
    const uint32_t version = 1000;
    memset(d2, 0, 64);                               /* start marker, flag-a and flag-b: zeros */
    ewlf_meta(d2 + 8, 16, version, ewlf_checksum(version, forged, 64));
    memcpy(d2 + 40, forged, 24);
    printf("T11/LF-10: EWLF, d2 holds a forged record, power cut in d2 before its meta field, reset\n");
    ewlf_two_writes_then_torn(d1, d2);
    lftl_read(&ctx, buf, flash, 64);
    dump("read", buf, 32);
    printf("  matches d1: %s, matches d2: %s, at area%+ld\n", memcmp(buf, d1, 64) ? "no" : "yes",
           memcmp(buf, d2, 64) ? "no" : "yes", (long)((uintptr_t)ctx.data - area_lo));
    break;
  }
  case 12: { /* LF-11: EWLF record larger than a page */
    /* 16-byte write units: header 16+16+16+16 = 64, data 192 -> 256-byte record
       on 256-byte pages, one per page: LFTL_AREA_EWLF would refuse this content
       at compile time, registration refuses it at run time. */
    if (setjmp(jb)) return 0; /* registration may refuse the configuration */
    init_area(WU, flash + PG, 3, 192, 1);
    uint8_t d[192];
    memset(d, 0x11, sizeof(d));
    memset(flash, 0xA5, PG); /* page 0: whatever precedes the area */
    printf("T12/LF-11: EWLF record of 256 bytes on 256-byte pages, format then one write\n");
    lftl_format(&ctx);
    lftl_write(&ctx, flash + PG, d, sizeof(d));
    unsigned changed = 0;
    for (unsigned i = 0; i < PG; i++) changed += flash[i] != 0xA5;
    printf("  %u bytes changed in the page before the area\n", changed);
    lftl_read(&ctx, buf, flash + PG, 16);
    break;
  }
  case 13: { /* LF-12: a header near the end of an EWLF area makes the scan read past it */
    if (setjmp(jb)) return 0; /* registration may refuse the configuration */
    init_area(WU, flash + 6 * PG, 2, 32, 1); /* 96-byte records, 2 per page */
    memset(flash, 0xFF, sizeof(flash));
    /* header is 16 + 16 + 16 + 16 = 64 bytes: before LF-12 the scan read the
       metadata inside the NVM through the read callback, then checksummed 32
       bytes past its end with memcpy, without a callback line. end - 64 is not
       a record position, so this header is never read */
    uint8_t *const header = flash + sizeof(flash) - 64;
    memset(header, 0, 16);                           /* start marker */
    ewlf_meta(header + 16, 16, 1, 0x0123456789ABCDEFull);
    memset(header + 32, 0, 32);                      /* both flags */
    printf("T13/LF-12: EWLF area holding only a header 64 bytes before its end, then lftl_read\n");
    lftl_read(&ctx, buf, flash + 6 * PG, 16);
    break;
  }
  default:
    fprintf(stderr, "unknown test %d\n", t); return 2;
  }
  printf("  no error raised\n");
  return 0;
}
