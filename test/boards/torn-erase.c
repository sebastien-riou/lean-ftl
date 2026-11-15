/* Measures what an interrupted page erase leaves on an STM32U5A5: on a page that was programmed,
 * and above all on a page that was ALREADY ERASED. EWLF.md, "Physics the design relies on": the
 * design of EWLF areas assumes that a cut erase of a unit whose cells were already erased leaves
 * them erased, and the model checker loses data in two cuts when that is false.
 *
 * Not built by CMake. Paste it into Core/Src/main.c of the lean-ftl-stm32u5 testbench, just before
 * "USER CODE END 0", call te_measure() in place of test_callbacks() and test_main(0,NULL,NULL), and
 * in Core/Src/stm32u5xx_it.c replace the endless loop of NMI_Handler with "te_nmi();" (declared
 * "void te_nmi(void);"): a read of a cell an interrupted erase left weak may raise the ECC NMI.
 * test/HWBRIDGE.md says how to build and run it through the bridge.
 *
 * The cut is a SYSTEM RESET requested by the firmware a chosen time after the erase starts, not a
 * loss of power: the reset aborts the flash operation (RM0456 7.3.10, and FLASH_OPSR then names
 * the interrupted operation), which is the closest thing to a power cut that can be placed with
 * microsecond precision from the firmware. A real power cut may differ in how the high voltage
 * collapses; this measurement cannot tell.
 *
 * It uses the last two pages of bank 2: the last one is the page under test, the one before it a
 * journal, since nothing in RAM is trusted across the resets. Flashing with the testbench's
 * script erases the whole chip, which starts a fresh run. A run is TE_ROUNDS x TE_STEPS x 2
 * trials, 4 erases of the page under test each: about 330 erases, 3 % of its endurance. Once the
 * journal says DONE, a reset only prints the summary again.
 *
 * Per round, for each delay d from 0 to a little more than an erase takes:
 *   trial P (control): erase the page, program it whole with a pattern, start an erase, reset at d.
 *     After the reset: how many words read erased, the pattern, or anything else. This shows where
 *     in the sweep the reset really interrupts an erase, and what an interrupted erase leaves on
 *     programmed cells (T4 of the flash page).
 *   trial E (the question): erase the page, check that it reads erased, start a second erase,
 *     reset at d. After the reset: how many words do not read erased, over three readings, with
 *     the ECC flags of each word; then every word is programmed with the pattern and read back:
 *     how many programs the flash refuses (PROGERR) and how many words read back wrong.
 * It prints one line per trial, a table, then a verdict and "TORN-ERASE DONE".
 */
#include <stdio.h>
#include <string.h>
#if !defined(STM32U5A5xx)
#error "torn erase measurement: written for the STM32U5A5"
#endif
#ifndef TE_STEPS
#define TE_STEPS 40u  /* delays per round */
#endif
#ifndef TE_ROUNDS
#define TE_ROUNDS 2u
#endif
#define TE_WU 16u
#define TE_N32 (TE_WU/4u)
#define TE_PAGE_SIZE 0x2000u
#define TE_PAGE_WORDS (TE_PAGE_SIZE/TE_WU)
#define TE_BANK_SIZE 0x200000u
#define TE_FLASH_BASE 0x08000000u
#define TE_PAGE_TEST (TE_FLASH_BASE + 2u*TE_BANK_SIZE - 1u*TE_PAGE_SIZE)
#define TE_PAGE_JOURNAL (TE_FLASH_BASE + 2u*TE_BANK_SIZE - 2u*TE_PAGE_SIZE)
#define TE_SR_ERRORS (FLASH_NSSR_OPERR|FLASH_NSSR_PROGERR|FLASH_NSSR_WRPERR|FLASH_NSSR_PGAERR|FLASH_NSSR_SIZERR|FLASH_NSSR_PGSERR|FLASH_NSSR_OPTWERR)
#define TE_BUSY (FLASH_NSSR_BSY|FLASH_NSSR_WDW)
#define TE_KEY1 0x45670123u
#define TE_KEY2 0xCDEF89ABu

/* journal entries: one write unit each, w[0] = magic, type, kind, step */
#define TE_MAGIC 0x7Eu
#define TE_HEADER 1u  /* w1 = cycles of an erase of a programmed page, w2 = of an erased page, w3 = SystemCoreClock */
#define TE_ARMED 2u   /* w1 = delay in cycles, w2 = round */
#define TE_RESULT1 3u /* w1 = words erased | words pattern << 16, w2 = words other | unstable words << 16, w3 = OPSR */
#define TE_RESULT2 4u /* w1 = words with ECCD | with ECCC << 16, w2 = programs refused | words read back wrong << 16, w3 = delay in us */
#define TE_DONE 5u
#define TE_KIND_P 0u
#define TE_KIND_E 1u
#define TE_N_TRIALS (TE_ROUNDS*TE_STEPS*2u)

static volatile uint32_t te_nmi_count;

/* The ECC NMI: a double error on a flash read. Count it, clear the flag, go on: the read
 * completes with whatever the flash returned. */
void te_nmi(void){
  te_nmi_count++;
  FLASH->ECCR |= FLASH_ECCR_ECCD;
}

static void te_clear(void){
  FLASH_NS->NSSR = TE_SR_ERRORS | FLASH_NSSR_EOP;
  FLASH->ECCR |= FLASH_ECCR_ECCC | FLASH_ECCR_ECCD;
}

static void te_icache_off(void){
  WRITE_REG(ICACHE->FCR, ICACHE_FCR_CBSYENDF);
  CLEAR_BIT(ICACHE->CR, ICACHE_CR_EN);
  while(READ_BIT(ICACHE->CR, ICACHE_CR_EN) != 0u);
}

static uint32_t te_erase_cr(uint32_t page_addr){
  const uint32_t off = page_addr - TE_FLASH_BASE;
  const uint32_t bank = off / TE_BANK_SIZE;
  const uint32_t page = (off % TE_BANK_SIZE) / TE_PAGE_SIZE;
  const uint32_t swap = (FLASH->OPTR & FLASH_OPTR_SWAP_BANK) ? 1u : 0u;
  return FLASH_NSCR_PER | (page << FLASH_NSCR_PNB_Pos) | ((bank ^ swap) ? FLASH_NSCR_BKER : 0u);
}

/* a whole erase; returns the status register, and the cycles it took */
static uint32_t te_erase(uint32_t page_addr, uint32_t*cycles){
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  te_clear();
  FLASH_NS->NSCR = te_erase_cr(page_addr);
  const uint32_t t0 = DWT->CYCCNT;
  FLASH_NS->NSCR |= FLASH_NSCR_STRT;
  __DSB();
  while(FLASH_NS->NSSR & TE_BUSY);
  *cycles = DWT->CYCCNT - t0;
  const uint32_t sr = FLASH_NS->NSSR;
  FLASH_NS->NSCR = 0;
  te_clear();
  NVIC_ClearPendingIRQ(FLASH_IRQn);
  __set_PRIMASK(primask);
  return sr;
}

/* an erase interrupted by a system reset `delay` cycles after its start: never returns. Runs
 * from RAM, so that nothing depends on fetching code from the flash while it erases. */
static void __attribute__((noinline, noreturn, section(".RamFunc"))) te_erase_then_reset(uint32_t cr, uint32_t delay){
  __disable_irq();
  FLASH_NS->NSSR = TE_SR_ERRORS | FLASH_NSSR_EOP;
  FLASH_NS->NSCR = cr;
  const uint32_t t0 = DWT->CYCCNT;
  FLASH_NS->NSCR = cr | FLASH_NSCR_STRT;
  while((DWT->CYCCNT - t0) < delay);
  SCB->AIRCR = (0x5FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
  for(;;);
}

/* program one write unit; returns the status register (PROGERR when the flash refuses) */
static uint32_t te_program(uint32_t addr, const uint32_t*v){
  volatile uint32_t*d = (volatile uint32_t*)addr;
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  te_clear();
  FLASH_NS->NSCR = FLASH_NSCR_PG;
  for(unsigned i=0;i<TE_N32;i++) d[i] = v[i];
  __DSB();
  while(FLASH_NS->NSSR & TE_BUSY);
  const uint32_t sr = FLASH_NS->NSSR;
  FLASH_NS->NSCR = 0;
  te_clear();
  NVIC_ClearPendingIRQ(FLASH_IRQn);
  __set_PRIMASK(primask);
  return sr;
}

/* the pattern of word `i` of the page under test: never all erased nor all zeros, and
 * different in every word */
static void te_pattern(unsigned i, uint32_t*v){
  for(unsigned k=0;k<TE_N32;k++) v[k] = 0x5A5A0000u ^ ((i*TE_N32 + k) * 0x9E3779B9u) ^ ((uint32_t)(i*TE_N32 + k) << 8);
  v[0] = (v[0] & 0xFFFF0000u) | 0xA500u | (i & 0xFFu); /* neither 0x0000 nor 0xFFFF in its low half */
}

/* read one write unit of the flash, with its ECC flags: bit 0 a corrected error, bit 1 a
 * detected one (the NMI) */
static unsigned te_read_word(uint32_t addr, uint32_t*v){
  const volatile uint32_t*s = (const volatile uint32_t*)addr;
  const uint32_t nmi0 = te_nmi_count;
  FLASH->ECCR |= FLASH_ECCR_ECCC | FLASH_ECCR_ECCD;
  for(unsigned k=0;k<TE_N32;k++) v[k] = s[k];
  __DSB();
  unsigned flags = 0;
  if(FLASH->ECCR & FLASH_ECCR_ECCC) flags |= 1u;
  if((FLASH->ECCR & FLASH_ECCR_ECCD) || (te_nmi_count != nmi0)) flags |= 2u;
  FLASH->ECCR |= FLASH_ECCR_ECCC | FLASH_ECCR_ECCD;
  return flags;
}

typedef struct {
  unsigned erased, pattern, other; /* words of the page, by how they read */
  unsigned eccc, eccd;             /* words whose read set an ECC flag */
} te_reading_t;

static uint32_t te_first[TE_PAGE_WORDS][TE_N32]; /* the first reading of the page, to spot words that change */

static void te_read_page(te_reading_t*r, unsigned*unstable, int first){
  memset(r, 0, sizeof(*r));
  for(unsigned i=0;i<TE_PAGE_WORDS;i++){
    uint32_t v[TE_N32], p[TE_N32];
    const unsigned flags = te_read_word(TE_PAGE_TEST + i*TE_WU, v);
    te_pattern(i, p);
    unsigned all_erased = 1;
    for(unsigned k=0;k<TE_N32;k++) if(0xFFFFFFFFu != v[k]) all_erased = 0;
    if(all_erased) r->erased++;
    else if(0 == memcmp(v, p, TE_WU)) r->pattern++;
    else r->other++;
    if(flags & 1u) r->eccc++;
    if(flags & 2u) r->eccd++;
    if(first) memcpy(te_first[i], v, TE_WU);
    else if(memcmp(te_first[i], v, TE_WU)) (*unstable)++;
  }
}

/* ---- the journal ---- */
static unsigned te_journal_count(void){
  unsigned n = 0;
  while(n < TE_PAGE_WORDS){
    const volatile uint32_t*e = (const volatile uint32_t*)(TE_PAGE_JOURNAL + n*TE_WU);
    if((e[0] >> 24) != TE_MAGIC) break;
    n++;
  }
  return n;
}
static void te_journal_get(unsigned n, uint32_t*w){
  const volatile uint32_t*e = (const volatile uint32_t*)(TE_PAGE_JOURNAL + n*TE_WU);
  for(unsigned k=0;k<TE_N32;k++) w[k] = e[k];
}
static void te_journal_add(unsigned n, unsigned type, unsigned kind, unsigned step, uint32_t w1, uint32_t w2, uint32_t w3){
  uint32_t w[TE_N32];
  w[0] = (TE_MAGIC << 24) | (type << 16) | (kind << 8) | step;
  w[1] = w1; w[2] = w2; w[3] = w3;
  const uint32_t sr = te_program(TE_PAGE_JOURNAL + n*TE_WU, w);
  if(sr & TE_SR_ERRORS){
    printf("TE ABORT: journal entry %u refused, sr=0x%08lx\n", n, (unsigned long)sr);
    for(;;);
  }
}
#define TE_TYPE(w0) (((w0) >> 16) & 0xFFu)
#define TE_KIND(w0) (((w0) >> 8) & 0xFFu)
#define TE_STEP(w0) ((w0) & 0xFFu)

static uint32_t te_us(uint32_t cycles){
  const uint32_t mhz = SystemCoreClock / 1000000u;
  return mhz ? cycles / mhz : 0u;
}

/* the delay of a step: from 0 to 1.15 times the longer of the two erases */
static uint32_t te_delay(unsigned step, uint32_t cyc_p, uint32_t cyc_e){
  const uint32_t longest = cyc_p > cyc_e ? cyc_p : cyc_e;
  const uint64_t span = ((uint64_t)longest * 115u) / 100u;
  return (uint32_t)((span * step) / (TE_STEPS - 1u));
}

static void te_abort(const char*what){
  printf("TE ABORT: %s\n", what);
  for(;;);
}

/* a whole erase of the page under test, checked */
static void te_erase_test_page(void){
  uint32_t cyc;
  const uint32_t sr = te_erase(TE_PAGE_TEST, &cyc);
  if(sr & TE_SR_ERRORS){ printf("TE erase sr=0x%08lx\n", (unsigned long)sr); te_abort("erase of the page under test refused"); }
}

/* what the reset left, for the trial the journal says was armed: measure, print, record */
static void te_after_reset(unsigned n, const uint32_t*armed, uint32_t opsr){
  const unsigned kind = TE_KIND(armed[0]);
  const unsigned step = TE_STEP(armed[0]);
  te_reading_t r, again;
  unsigned unstable = 0;
  te_read_page(&r, &unstable, 1);
  te_read_page(&again, &unstable, 0);
  HAL_Delay(20);
  te_read_page(&again, &unstable, 0);
  unsigned refused = 0, rb_bad = 0;
  if(TE_KIND_E == kind){
    /* program every word, then read the page back */
    for(unsigned i=0;i<TE_PAGE_WORDS;i++){
      uint32_t p[TE_N32];
      te_pattern(i, p);
      if(te_program(TE_PAGE_TEST + i*TE_WU, p) & TE_SR_ERRORS) refused++;
    }
    te_reading_t back;
    unsigned dummy = 0;
    te_read_page(&back, &dummy, 1);
    rb_bad = TE_PAGE_WORDS - back.pattern;
  }
  const unsigned code = (opsr & FLASH_OPSR_CODE_OP) >> FLASH_OPSR_CODE_OP_Pos;
  printf("TE %c round %lu step %2u delay %4lu us: OPSR=0x%08lx (code %u%s) erased=%3u pattern=%3u other=%3u unstable=%3u eccc=%3u eccd=%3u",
    (TE_KIND_E == kind) ? 'E' : 'P', (unsigned long)armed[2], step, (unsigned long)te_us(armed[1]), (unsigned long)opsr, code,
    (3u == code) ? ", page erase interrupted" : "", r.erased, r.pattern, r.other, unstable, r.eccc, r.eccd);
  if(TE_KIND_E == kind) printf(" | then programmed: refused=%3u read back wrong=%3u", refused, rb_bad);
  printf("\n");
  te_journal_add(n, TE_RESULT1, kind, step, r.erased | (r.pattern << 16), r.other | (unstable << 16), opsr);
  te_journal_add(n+1, TE_RESULT2, kind, step, r.eccd | (r.eccc << 16), refused | (rb_bad << 16), te_us(armed[1]));
}

/* start trial number `trial`: prepare the page, record it, erase, reset. Never returns. */
static void te_start_trial(unsigned n, unsigned trial, uint32_t cyc_p, uint32_t cyc_e){
  const unsigned round = trial / (TE_STEPS*2u);
  const unsigned step = (trial % (TE_STEPS*2u)) / 2u;
  const unsigned kind = trial % 2u;
  const uint32_t delay = te_delay(step, cyc_p, cyc_e);
  te_erase_test_page();
  te_reading_t r;
  unsigned dummy = 0;
  if(TE_KIND_P == kind){
    for(unsigned i=0;i<TE_PAGE_WORDS;i++){
      uint32_t p[TE_N32];
      te_pattern(i, p);
      if(te_program(TE_PAGE_TEST + i*TE_WU, p) & TE_SR_ERRORS) te_abort("program of an erased word refused");
    }
    te_read_page(&r, &dummy, 1);
    if(TE_PAGE_WORDS != r.pattern) te_abort("the page does not read the pattern before the trial");
  } else {
    te_read_page(&r, &dummy, 1);
    if((TE_PAGE_WORDS != r.erased) || r.eccc || r.eccd) te_abort("the page does not read erased before the trial");
  }
  te_journal_add(n, TE_ARMED, kind, step, delay, round, 0);
  te_erase_then_reset(te_erase_cr(TE_PAGE_TEST), delay);
}

static void te_summary(unsigned n){
  unsigned e_trials = 0, e_interrupted = 0, e_not_erased = 0, e_ecc = 0, e_refused = 0, e_rb_bad = 0, e_unstable = 0;
  unsigned p_trials = 0, p_interrupted = 0, p_mixed = 0;
  printf("\nTE TABLE kind round step delay_us opsr_code erased pattern other unstable eccc eccd refused rb_wrong\n");
  for(unsigned i=0;i+2<n;i++){
    uint32_t a[TE_N32], r1[TE_N32], r2[TE_N32];
    te_journal_get(i, a);
    if(TE_ARMED != TE_TYPE(a[0])) continue;
    te_journal_get(i+1, r1);
    te_journal_get(i+2, r2);
    if((TE_RESULT1 != TE_TYPE(r1[0])) || (TE_RESULT2 != TE_TYPE(r2[0]))) continue;
    const unsigned kind = TE_KIND(a[0]);
    const unsigned erased = r1[1] & 0xFFFFu, pattern = r1[1] >> 16, other = r1[2] & 0xFFFFu, unstable = r1[2] >> 16;
    const unsigned code = (r1[3] & FLASH_OPSR_CODE_OP) >> FLASH_OPSR_CODE_OP_Pos;
    const unsigned eccd = r2[1] & 0xFFFFu, eccc = r2[1] >> 16, refused = r2[2] & 0xFFFFu, rb_bad = r2[2] >> 16;
    printf("TE ROW %c %lu %2u %4lu %u %3u %3u %3u %3u %3u %3u %3u %3u\n", (TE_KIND_E == kind) ? 'E' : 'P', (unsigned long)a[2],
      (unsigned)TE_STEP(a[0]), (unsigned long)r2[3], code, erased, pattern, other, unstable, eccc, eccd, refused, rb_bad);
    if(TE_KIND_E == kind){
      e_trials++;
      if(3u == code){
        e_interrupted++;
        if(TE_PAGE_WORDS != erased) e_not_erased++;
        if(eccc || eccd) e_ecc++;
        if(refused) e_refused++;
        if(rb_bad) e_rb_bad++;
        if(unstable) e_unstable++;
      }
    } else {
      p_trials++;
      if(3u == code){
        p_interrupted++;
        if((TE_PAGE_WORDS != erased) && (TE_PAGE_WORDS != pattern)) p_mixed++;
      }
    }
  }
  printf("TE SUMMARY control, programmed page: %u trials, %u with an erase interrupted, %u of them left a page neither all erased nor all pattern\n",
    p_trials, p_interrupted, p_mixed);
  printf("TE SUMMARY erased page: %u trials, %u with an erase interrupted; of those:\n", e_trials, e_interrupted);
  printf("TE SUMMARY   %u with words that do not read erased\n", e_not_erased);
  printf("TE SUMMARY   %u with an ECC flag on a read\n", e_ecc);
  printf("TE SUMMARY   %u with words that read differently from one reading to the next\n", e_unstable);
  printf("TE SUMMARY   %u with a program refused afterwards\n", e_refused);
  printf("TE SUMMARY   %u with a word read back wrong after programming\n", e_rb_bad);
  if(0 == e_interrupted){
    printf("TE VERDICT: no erase of an erased page was interrupted: nothing measured, check the delays\n");
  } else if(e_not_erased || e_ecc || e_unstable || e_refused || e_rb_bad){
    printf("TE VERDICT: ASSUMPTION FALSE on this part: an interrupted erase of an erased page does not leave it cleanly erased\n");
  } else {
    printf("TE VERDICT: assumption holds in %u of %u interrupted erases of an erased page (reset as the cut, one part, room temperature)\n",
      e_interrupted, e_interrupted);
  }
  if(p_interrupted && (0 == p_mixed)) printf("TE NOTE: no interrupted erase left a programmed page half erased: the reset may not stop the erase where the delay says\n");
  printf("TORN-ERASE DONE\n");
}

static void te_measure(void){
  /* first of all: what the reset interrupted, before any flash operation */
  const uint32_t opsr = FLASH->OPSR;
  setvbuf(stdout, NULL, _IONBF, 0);
#ifdef DCB
  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
#else
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#endif
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  te_icache_off();
  FLASH->NSKEYR = TE_KEY1;
  FLASH->NSKEYR = TE_KEY2;
  te_clear();

  unsigned n = te_journal_count();
  uint32_t last[TE_N32] = {0};
  if(n) te_journal_get(n-1, last);
  if((0 == n) || (TE_HEADER != TE_TYPE(((const volatile uint32_t*)TE_PAGE_JOURNAL)[0]))){
    /* a fresh run */
    HAL_Delay(2000); /* let the programmer detach first */
    printf("\nTE START STM32U5A5 torn erase: %u rounds of %u delays, page under test 0x%08lx, journal 0x%08lx\n",
      (unsigned)TE_ROUNDS, (unsigned)TE_STEPS, (unsigned long)TE_PAGE_TEST, (unsigned long)TE_PAGE_JOURNAL);
    printf("TE IDCODE=0x%08lx OPTR=0x%08lx ACR=0x%08lx NSSR=0x%08lx OPSR=0x%08lx ECCR=0x%08lx SystemCoreClock=%lu\n",
      (unsigned long)DBGMCU->IDCODE, (unsigned long)FLASH->OPTR, (unsigned long)FLASH->ACR, (unsigned long)FLASH_NS->NSSR,
      (unsigned long)opsr, (unsigned long)FLASH->ECCR, (unsigned long)SystemCoreClock);
    if(FLASH->OPTR & FLASH_OPTR_TZEN) te_abort("TrustZone enabled, the nonsecure registers may not reach bank 2");
    if(0u == (FLASH->OPTR & FLASH_OPTR_DUALBANK)) te_abort("not in dual-bank mode, the page geometry assumed here is wrong");
    if(TE_N_TRIALS*3u + 2u > TE_PAGE_WORDS) te_abort("too many trials for the journal page");
    uint32_t cyc, cyc_p = 0, cyc_e = 0;
    uint32_t sr = te_erase(TE_PAGE_JOURNAL, &cyc);
    if(sr & TE_SR_ERRORS) te_abort("erase of the journal refused");
    /* how long an erase takes, of a programmed page and of an erased one: the longest of 3 */
    for(unsigned k=0;k<3;k++){
      te_erase_test_page();
      for(unsigned i=0;i<TE_PAGE_WORDS;i++){
        uint32_t p[TE_N32];
        te_pattern(i, p);
        if(te_program(TE_PAGE_TEST + i*TE_WU, p) & TE_SR_ERRORS) te_abort("program of an erased word refused");
      }
      sr = te_erase(TE_PAGE_TEST, &cyc);
      if(cyc > cyc_p) cyc_p = cyc;
      printf("TE erase of a programmed page: %lu us, sr=0x%08lx\n", (unsigned long)te_us(cyc), (unsigned long)sr);
      sr = te_erase(TE_PAGE_TEST, &cyc);
      if(cyc > cyc_e) cyc_e = cyc;
      printf("TE erase of an erased page:    %lu us, sr=0x%08lx\n", (unsigned long)te_us(cyc), (unsigned long)sr);
    }
    te_journal_add(0, TE_HEADER, 0, 0, cyc_p, cyc_e, SystemCoreClock);
    n = 1;
    te_journal_get(0, last);
  }
  uint32_t header[TE_N32];
  te_journal_get(0, header);
  if(TE_DONE == TE_TYPE(last[0])){
    printf("\nTE the run is complete; flash the firmware again (chip erase) for a new one\n");
    te_summary(n);
    return;
  }
  if(TE_ARMED == TE_TYPE(last[0])){
    te_after_reset(n, last, opsr);
    n += 2;
  }
  /* trials done so far: one ARMED, RESULT1, RESULT2 triple each */
  const unsigned trial = (n - 1u) / 3u;
  if(trial >= TE_N_TRIALS){
    te_journal_add(n, TE_DONE, 0, 0, 0, 0, 0);
    te_summary(n+1);
    return;
  }
  te_start_trial(n, trial, header[1], header[2]);
}
