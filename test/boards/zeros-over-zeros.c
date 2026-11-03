/* Measures whether the flash of an STM32U5A5 or STM32L552 accepts all zeros programmed over a
 * write unit that already reads all zeros: FLASH.md, section 4, point 2, measured on 2026-11-03.
 *
 * Not built by CMake. Paste it into Core/Src/main.c of the lean-ftl-stm32u5 or lean-ftl-stm32l5
 * testbench, just before "USER CODE END 0", and call op2_measure() in place of test_callbacks()
 * and test_main(0,NULL,NULL). test/HWBRIDGE.md says how to build and run it through the bridge
 * (the L552 also needs its USART3 console). It prints one line per program, a summary, then
 * "OP2 DONE"; it overwrites the last two pages of bank 2.
 */
/* ---- FLASH.md open point 2: all zeros programmed over a write unit that already reads all zeros ----
 * Erases the last two pages of bank 2, then on each page:
 *   Z  units 0-3: erased -> zeros, then zeros x3      (question: zeros over zeros)
 *   PZ units 4-7: erased -> A5, A5 -> zeros (the pin), then zeros x3 (question again)
 *   PP unit 8:    erased -> A5, A5 -> 05              (control: the manual says PROGERR)
 *   ZX unit 9:    erased -> zeros, zeros -> 12345678  (control: PROGERR)
 *   FZ unit 10:   erased -> FF.., FF.. -> zeros       (information)
 * Each program: flags cleared, PG|EOPIE, the unit written word by word, then BSY (and WDW) polled.
 * Recorded: the status register, PROGERR, EOP (set only on a successful operation with EOPIE),
 * whether BSY was seen, the cycles from the last word to the end, the read-back and ECCR.
 * Interrupts masked and ICACHE off for every flash access. */
#include <stdio.h>
#include <string.h>
#if defined(STM32U5A5xx)
#define OP2_CHIP "STM32U5A5"
#define OP2_WU 16u
#define OP2_PAGE_SIZE 0x2000u
#define OP2_BANK_SIZE 0x200000u
#define OP2_SR_ERRORS (FLASH_NSSR_OPERR|FLASH_NSSR_PROGERR|FLASH_NSSR_WRPERR|FLASH_NSSR_PGAERR|FLASH_NSSR_SIZERR|FLASH_NSSR_PGSERR|FLASH_NSSR_OPTWERR)
#define OP2_EOP FLASH_NSSR_EOP
#define OP2_PROGERR FLASH_NSSR_PROGERR
#define OP2_BSY FLASH_NSSR_BSY
#define OP2_BUSY (FLASH_NSSR_BSY|FLASH_NSSR_WDW)
#define OP2_CR_PG FLASH_NSCR_PG
#define OP2_CR_PER FLASH_NSCR_PER
#define OP2_CR_BKER FLASH_NSCR_BKER
#define OP2_CR_PNB_Pos FLASH_NSCR_PNB_Pos
#define OP2_CR_STRT FLASH_NSCR_STRT
#define OP2_CR_EOPIE FLASH_NSCR_EOPIE
#define OP2_CR_LOCK FLASH_NSCR_LOCK
#define OP2_DUAL() (0u != (FLASH->OPTR & FLASH_OPTR_DUALBANK))
#elif defined(STM32L552xx)
#define OP2_CHIP "STM32L552"
#define OP2_WU 8u
#define OP2_PAGE_SIZE 0x800u
#define OP2_BANK_SIZE 0x40000u
#define OP2_SR_ERRORS (FLASH_NSSR_NSOPERR|FLASH_NSSR_NSPROGERR|FLASH_NSSR_NSWRPERR|FLASH_NSSR_NSPGAERR|FLASH_NSSR_NSSIZERR|FLASH_NSSR_NSPGSERR|FLASH_NSSR_OPTWERR)
#define OP2_EOP FLASH_NSSR_NSEOP
#define OP2_PROGERR FLASH_NSSR_NSPROGERR
#define OP2_BSY FLASH_NSSR_NSBSY
#define OP2_BUSY FLASH_NSSR_NSBSY
#define OP2_CR_PG FLASH_NSCR_NSPG
#define OP2_CR_PER FLASH_NSCR_NSPER
#define OP2_CR_BKER FLASH_NSCR_NSBKER
#define OP2_CR_PNB_Pos FLASH_NSCR_NSPNB_Pos
#define OP2_CR_STRT FLASH_NSCR_NSSTRT
#define OP2_CR_EOPIE FLASH_NSCR_NSEOPIE
#define OP2_CR_LOCK FLASH_NSCR_NSLOCK
#define OP2_DUAL() (0u != (FLASH->OPTR & FLASH_OPTR_DBANK))
#else
#error "open point 2 measurement: unknown chip"
#endif
#define OP2_FLASH_BASE 0x08000000u
#define OP2_N32 (OP2_WU/4u)
#define OP2_KEY1 0x45670123u
#define OP2_KEY2 0xCDEF89ABu

typedef struct {
  uint32_t sr, cycles, bsy_seen, eccr;
  uint32_t rb[OP2_N32];
} op2_res_t;

static uint32_t op2_v_zero[OP2_N32], op2_v_ones[OP2_N32], op2_v_a5[OP2_N32], op2_v_05[OP2_N32], op2_v_x[OP2_N32];

static void op2_fill(uint32_t*v, uint32_t w){ for(unsigned i=0;i<OP2_N32;i++) v[i]=w; }

static void op2_icache_off(void){
  WRITE_REG(ICACHE->FCR, ICACHE_FCR_CBSYENDF);
  CLEAR_BIT(ICACHE->CR, ICACHE_CR_EN);
  while(READ_BIT(ICACHE->CR, ICACHE_CR_EN) != 0u);
}

static void op2_clear(void){
  FLASH_NS->NSSR = OP2_SR_ERRORS | OP2_EOP;
  FLASH->ECCR |= FLASH_ECCR_ECCC | FLASH_ECCR_ECCD;
}

static uint32_t op2_erase(uint32_t page_addr, uint32_t*cycles){
  const uint32_t off = page_addr - OP2_FLASH_BASE;
  const uint32_t bank = off / OP2_BANK_SIZE;
  const uint32_t page = (off % OP2_BANK_SIZE) / OP2_PAGE_SIZE;
  const uint32_t swap = (FLASH->OPTR & FLASH_OPTR_SWAP_BANK) ? 1u : 0u;
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  op2_clear();
  FLASH_NS->NSCR = OP2_CR_PER | (page << OP2_CR_PNB_Pos) | ((bank ^ swap) ? OP2_CR_BKER : 0u);
  const uint32_t t0 = DWT->CYCCNT;
  FLASH_NS->NSCR |= OP2_CR_STRT;
  __DSB();
  while(FLASH_NS->NSSR & OP2_BSY);
  *cycles = DWT->CYCCNT - t0;
  const uint32_t sr = FLASH_NS->NSSR;
  FLASH_NS->NSCR = 0;
  op2_clear();
  NVIC_ClearPendingIRQ(FLASH_IRQn);
  __set_PRIMASK(primask);
  return sr;
}

static void op2_program(uint32_t addr, const uint32_t*v, op2_res_t*r){
  volatile uint32_t*d = (volatile uint32_t*)addr;
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  op2_clear();
  FLASH_NS->NSCR = OP2_CR_PG | OP2_CR_EOPIE;
  for(unsigned i=0;i+1<OP2_N32;i++) d[i] = v[i];
  uint32_t bsy = 0, sr;
  const uint32_t t0 = DWT->CYCCNT;
  d[OP2_N32-1] = v[OP2_N32-1];
  __DSB();
  while((sr = FLASH_NS->NSSR) & OP2_BUSY) bsy |= sr & OP2_BSY;
  r->cycles = DWT->CYCCNT - t0;
  r->bsy_seen = bsy ? 1u : 0u;
  r->sr = FLASH_NS->NSSR;
  FLASH_NS->NSCR = 0;
  op2_clear();
  for(unsigned i=0;i<OP2_N32;i++) r->rb[i] = d[i];
  r->eccr = FLASH->ECCR;
  op2_clear();
  NVIC_ClearPendingIRQ(FLASH_IRQn);
  __set_PRIMASK(primask);
}

typedef struct { unsigned n, progerr, eop, bsy, rb_ok; uint32_t cyc_min, cyc_max; } op2_stat_t;
static op2_stat_t op2_st_first, op2_st_pin, op2_st_zz, op2_st_ctrl, op2_st_fz;
static unsigned op2_n_ecc;

static void op2_step(uint32_t page, unsigned unit, const char*kase, unsigned step, const char*what,
                     const uint32_t*v, const uint32_t*expect, op2_stat_t*st){
  op2_res_t r;
  const uint32_t addr = page + unit*OP2_WU;
  op2_program(addr, v, &r);
  const unsigned progerr = (r.sr & OP2_PROGERR) ? 1u : 0u;
  const unsigned eop = (r.sr & OP2_EOP) ? 1u : 0u;
  const unsigned rb_ok = (0 == memcmp(r.rb, expect, OP2_WU)) ? 1u : 0u;
  const uint32_t mhz = SystemCoreClock / 1000000u;
  if(r.eccr & (FLASH_ECCR_ECCC | FLASH_ECCR_ECCD)) op2_n_ecc++;
  st->n++; st->progerr += progerr; st->eop += eop; st->bsy += r.bsy_seen; st->rb_ok += rb_ok;
  if(1 == st->n || r.cycles < st->cyc_min) st->cyc_min = r.cycles;
  if(r.cycles > st->cyc_max) st->cyc_max = r.cycles;
  printf("OP2 %s 0x%08lx u%02u %-2s s%u %-12s sr=0x%08lx progerr=%u eop=%u bsy=%u cyc=%7lu us=%5lu rb0=0x%08lx rb_ok=%u eccr=0x%08lx\n",
    OP2_CHIP, (unsigned long)addr, unit, kase, step, what, (unsigned long)r.sr, progerr, eop, (unsigned)r.bsy_seen,
    (unsigned long)r.cycles, (unsigned long)(mhz ? r.cycles / mhz : 0u), (unsigned long)r.rb[0], rb_ok,
    (unsigned long)r.eccr);
}

static void op2_print_stat(const char*name, const op2_stat_t*st){
  const uint32_t mhz = SystemCoreClock / 1000000u;
  printf("OP2 SUMMARY %s %-34s n=%2u progerr=%2u eop=%2u bsy=%2u rb_ok=%2u us=%lu..%lu\n", OP2_CHIP, name,
    st->n, st->progerr, st->eop, st->bsy, st->rb_ok,
    (unsigned long)(mhz ? st->cyc_min / mhz : 0u), (unsigned long)(mhz ? st->cyc_max / mhz : 0u));
}

static void op2_measure(void){
  setvbuf(stdout, NULL, _IONBF, 0);
  HAL_Delay(2000); /* let the programmer detach first: the ST-LINK/V2-1 VCP drops output meanwhile */
#ifdef DCB
  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
#else
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#endif
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  op2_fill(op2_v_zero, 0u); op2_fill(op2_v_ones, 0xFFFFFFFFu); op2_fill(op2_v_a5, 0xA5A5A5A5u);
  op2_fill(op2_v_05, 0x05050505u); op2_fill(op2_v_x, 0x12345678u);

  printf("\nOP2 START %s write unit %u bytes, page %u bytes\n", OP2_CHIP, (unsigned)OP2_WU, (unsigned)OP2_PAGE_SIZE);
  printf("OP2 %s IDCODE=0x%08lx OPTR=0x%08lx ACR=0x%08lx NSSR=0x%08lx NSCR=0x%08lx ECCR=0x%08lx SystemCoreClock=%lu\n",
    OP2_CHIP, (unsigned long)DBGMCU->IDCODE, (unsigned long)FLASH->OPTR, (unsigned long)FLASH->ACR,
    (unsigned long)FLASH_NS->NSSR, (unsigned long)FLASH_NS->NSCR, (unsigned long)FLASH->ECCR, (unsigned long)SystemCoreClock);
  if(FLASH->OPTR & FLASH_OPTR_TZEN){ printf("OP2 ABORT: TrustZone enabled, the nonsecure registers may not reach bank 2\n"); return; }
  if(!OP2_DUAL()){ printf("OP2 ABORT: not in dual-bank mode, the page geometry assumed here is wrong\n"); return; }

  op2_icache_off();
  FLASH->NSKEYR = OP2_KEY1;
  FLASH->NSKEYR = OP2_KEY2;

  for(unsigned k = 1; k <= 2; k++){
    const uint32_t page = OP2_FLASH_BASE + 2u*OP2_BANK_SIZE - k*OP2_PAGE_SIZE;
    uint32_t cyc;
    const uint32_t sr = op2_erase(page, &cyc);
    unsigned erased = 1;
    for(uint32_t a = page; a < page + OP2_PAGE_SIZE; a += 4) if(0xFFFFFFFFu != *(volatile uint32_t*)a) erased = 0;
    printf("OP2 %s erase 0x%08lx sr=0x%08lx us=%lu erased=%u\n", OP2_CHIP, (unsigned long)page, (unsigned long)sr,
      (unsigned long)(cyc / (SystemCoreClock / 1000000u)), erased);
    if(!erased){ printf("OP2 ABORT: the page does not read erased\n"); return; }

    for(unsigned u = 0; u < 4; u++){
      op2_step(page, u, "Z", 0, "erased->0", op2_v_zero, op2_v_zero, &op2_st_first);
      for(unsigned s = 1; s <= 3; s++) op2_step(page, u, "Z", s, "zero->0", op2_v_zero, op2_v_zero, &op2_st_zz);
    }
    for(unsigned u = 4; u < 8; u++){
      op2_step(page, u, "PZ", 0, "erased->A5", op2_v_a5, op2_v_a5, &op2_st_first);
      op2_step(page, u, "PZ", 1, "A5->0 (pin)", op2_v_zero, op2_v_zero, &op2_st_pin);
      for(unsigned s = 2; s <= 4; s++) op2_step(page, u, "PZ", s, "zero->0", op2_v_zero, op2_v_zero, &op2_st_zz);
    }
    op2_step(page, 8, "PP", 0, "erased->A5", op2_v_a5, op2_v_a5, &op2_st_first);
    op2_step(page, 8, "PP", 1, "A5->05", op2_v_05, op2_v_a5, &op2_st_ctrl);
    op2_step(page, 9, "ZX", 0, "erased->0", op2_v_zero, op2_v_zero, &op2_st_first);
    op2_step(page, 9, "ZX", 1, "zero->X", op2_v_x, op2_v_zero, &op2_st_ctrl);
    op2_step(page, 10, "FZ", 0, "erased->FF", op2_v_ones, op2_v_ones, &op2_st_first);
    op2_step(page, 10, "FZ", 1, "FF->0", op2_v_zero, op2_v_zero, &op2_st_fz);
  }
  FLASH->NSCR |= OP2_CR_LOCK;

  op2_print_stat("first program of an erased unit", &op2_st_first);
  op2_print_stat("zeros over a partial unit (pin)", &op2_st_pin);
  op2_print_stat("ZEROS OVER ZEROS", &op2_st_zz);
  op2_print_stat("controls (rb_ok = content kept)", &op2_st_ctrl);
  op2_print_stat("zeros over all ones (info)", &op2_st_fz);
  printf("OP2 SUMMARY %s ECC flags seen on %u read-backs\n", OP2_CHIP, op2_n_ecc);
  printf("OP2 DONE\n");
}
