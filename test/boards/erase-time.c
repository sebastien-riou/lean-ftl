/* Measures how long a page erase takes on an STM32U5A5 as a function of what the page holds:
 * how many of its 512 write units are programmed, where they are, and with what. FLASH.md,
 * section 4, point 5 found 0.88 ms for an erased page and 1.56 ms for a page programmed whole;
 * this says what lies between.
 *
 * Not built by CMake. Paste it into Core/Src/main.c of the lean-ftl-stm32u5 testbench, just before
 * "USER CODE END 0", and call et_measure() in place of test_callbacks() and
 * test_main(0,NULL,NULL). test/HWBRIDGE.md says how to build and run it through the bridge.
 * It uses the last page of bank 2, about 85 erases per run, runs again at every reset, prints
 * one line per erase, a table, then "ERASE-TIME DONE".
 *
 * Cases, ET_REPEAT erases each, timed from STRT to the end of BSY:
 *   n write units programmed with a pattern, the first n of the page, n from 0 to 512;
 *   one write unit programmed, at the start, in the middle, at the end of the page;
 *   one write unit with a single bit programmed;
 *   the whole page programmed to all zeros;
 *   every other write unit programmed (256, spread over the page).
 */
#include <stdio.h>
#include <string.h>
#if !defined(STM32U5A5xx)
#error "erase time measurement: written for the STM32U5A5"
#endif
#ifndef ET_REPEAT
#define ET_REPEAT 3u
#endif
#define ET_WU 16u
#define ET_N32 (ET_WU/4u)
#define ET_PAGE_SIZE 0x2000u
#define ET_PAGE_WORDS (ET_PAGE_SIZE/ET_WU)
#define ET_BANK_SIZE 0x200000u
#define ET_FLASH_BASE 0x08000000u
#define ET_PAGE (ET_FLASH_BASE + 2u*ET_BANK_SIZE - 1u*ET_PAGE_SIZE)
#define ET_SR_ERRORS (FLASH_NSSR_OPERR|FLASH_NSSR_PROGERR|FLASH_NSSR_WRPERR|FLASH_NSSR_PGAERR|FLASH_NSSR_SIZERR|FLASH_NSSR_PGSERR|FLASH_NSSR_OPTWERR)
#define ET_BUSY (FLASH_NSSR_BSY|FLASH_NSSR_WDW)
#define ET_KEY1 0x45670123u
#define ET_KEY2 0xCDEF89ABu

static void et_clear(void){
  FLASH_NS->NSSR = ET_SR_ERRORS | FLASH_NSSR_EOP;
  FLASH->ECCR |= FLASH_ECCR_ECCC | FLASH_ECCR_ECCD;
}

static void et_icache_off(void){
  WRITE_REG(ICACHE->FCR, ICACHE_FCR_CBSYENDF);
  CLEAR_BIT(ICACHE->CR, ICACHE_CR_EN);
  while(READ_BIT(ICACHE->CR, ICACHE_CR_EN) != 0u);
}

/* a whole erase of the page; returns the status register, and the cycles it took */
static uint32_t et_erase(uint32_t*cycles){
  const uint32_t off = ET_PAGE - ET_FLASH_BASE;
  const uint32_t bank = off / ET_BANK_SIZE;
  const uint32_t page = (off % ET_BANK_SIZE) / ET_PAGE_SIZE;
  const uint32_t swap = (FLASH->OPTR & FLASH_OPTR_SWAP_BANK) ? 1u : 0u;
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  et_clear();
  FLASH_NS->NSCR = FLASH_NSCR_PER | (page << FLASH_NSCR_PNB_Pos) | ((bank ^ swap) ? FLASH_NSCR_BKER : 0u);
  const uint32_t t0 = DWT->CYCCNT;
  FLASH_NS->NSCR |= FLASH_NSCR_STRT;
  __DSB();
  while(FLASH_NS->NSSR & ET_BUSY);
  *cycles = DWT->CYCCNT - t0;
  const uint32_t sr = FLASH_NS->NSSR;
  FLASH_NS->NSCR = 0;
  et_clear();
  NVIC_ClearPendingIRQ(FLASH_IRQn);
  __set_PRIMASK(primask);
  return sr;
}

/* program one write unit; returns the status register */
static uint32_t et_program(unsigned word, const uint32_t*v){
  volatile uint32_t*d = (volatile uint32_t*)(ET_PAGE + word*ET_WU);
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  et_clear();
  FLASH_NS->NSCR = FLASH_NSCR_PG;
  for(unsigned i=0;i<ET_N32;i++) d[i] = v[i];
  __DSB();
  while(FLASH_NS->NSSR & ET_BUSY);
  const uint32_t sr = FLASH_NS->NSSR;
  FLASH_NS->NSCR = 0;
  et_clear();
  NVIC_ClearPendingIRQ(FLASH_IRQn);
  __set_PRIMASK(primask);
  return sr;
}

/* what a case programs into write unit `word`: 0 nothing, else the content */
typedef enum { ET_FIRST_N, ET_ONE_AT, ET_ONE_BIT_AT, ET_ALL_ZEROS, ET_EVERY_OTHER } et_shape_t;
typedef struct { et_shape_t shape; unsigned arg; const char*name; } et_case_t;

static int et_content(const et_case_t*c, unsigned word, uint32_t*v){
  for(unsigned k=0;k<ET_N32;k++) v[k] = 0x5A5A0000u ^ ((word*ET_N32 + k) * 0x9E3779B9u);
  v[0] = (v[0] & 0xFFFF0000u) | 0xA500u | (word & 0xFFu);
  switch(c->shape){
    case ET_FIRST_N: return word < c->arg;
    case ET_ONE_AT: return word == c->arg;
    case ET_ONE_BIT_AT:
      for(unsigned k=0;k<ET_N32;k++) v[k] = 0xFFFFFFFFu;
      v[0] = 0xFFFFFFFEu;
      return word == c->arg;
    case ET_ALL_ZEROS:
      for(unsigned k=0;k<ET_N32;k++) v[k] = 0u;
      return 1;
    default: return 0 == (word & 1u);
  }
}

static const et_case_t et_cases[] = {
  {ET_FIRST_N, 0, "erased"},
  {ET_ONE_BIT_AT, 0, "1 bit, first unit"},
  {ET_ONE_BIT_AT, 511, "1 bit, last unit"},
  {ET_ONE_AT, 0, "1 unit, first"},
  {ET_ONE_AT, 256, "1 unit, middle"},
  {ET_ONE_AT, 511, "1 unit, last"},
  {ET_FIRST_N, 2, "first 2 units"},
  {ET_FIRST_N, 4, "first 4 units"},
  {ET_FIRST_N, 8, "first 8 units"},
  {ET_FIRST_N, 16, "first 16 units"},
  {ET_FIRST_N, 32, "first 32 units"},
  {ET_FIRST_N, 64, "first 64 units"},
  {ET_FIRST_N, 128, "first 128 units"},
  {ET_FIRST_N, 192, "first 192 units"},
  {ET_FIRST_N, 256, "first 256 units"},
  {ET_EVERY_OTHER, 0, "every other unit (256)"},
  {ET_FIRST_N, 320, "first 320 units"},
  {ET_FIRST_N, 384, "first 384 units"},
  {ET_FIRST_N, 448, "first 448 units"},
  {ET_FIRST_N, 480, "first 480 units"},
  {ET_FIRST_N, 496, "first 496 units"},
  {ET_FIRST_N, 504, "first 504 units"},
  {ET_FIRST_N, 508, "first 508 units"},
  {ET_FIRST_N, 510, "first 510 units"},
  {ET_FIRST_N, 511, "first 511 units"},
  {ET_FIRST_N, 512, "all 512 units"},
  {ET_ALL_ZEROS, 0, "all 512 units, zeros"},
  {ET_FIRST_N, 0, "erased, again"},
};
#define ET_N_CASES (sizeof(et_cases)/sizeof(et_cases[0]))

static void et_measure(void){
  static uint32_t us_min[ET_N_CASES], us_max[ET_N_CASES];
  static unsigned programmed[ET_N_CASES];
  setvbuf(stdout, NULL, _IONBF, 0);
  HAL_Delay(2000); /* let the programmer detach first */
#ifdef DCB
  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
#else
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#endif
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  const uint32_t mhz = SystemCoreClock / 1000000u;
  printf("\nET START STM32U5A5 erase time against content: %u cases, %u erases each, page 0x%08lx\n",
    (unsigned)ET_N_CASES, (unsigned)ET_REPEAT, (unsigned long)ET_PAGE);
  printf("ET IDCODE=0x%08lx OPTR=0x%08lx ACR=0x%08lx NSSR=0x%08lx SystemCoreClock=%lu\n", (unsigned long)DBGMCU->IDCODE,
    (unsigned long)FLASH->OPTR, (unsigned long)FLASH->ACR, (unsigned long)FLASH_NS->NSSR, (unsigned long)SystemCoreClock);
  if(FLASH->OPTR & FLASH_OPTR_TZEN){ printf("ET ABORT: TrustZone enabled\n"); return; }
  if(0u == (FLASH->OPTR & FLASH_OPTR_DUALBANK)){ printf("ET ABORT: not in dual-bank mode\n"); return; }
  if(0u == mhz){ printf("ET ABORT: core clock below 1 MHz\n"); return; }
  et_icache_off();
  FLASH->NSKEYR = ET_KEY1;
  FLASH->NSKEYR = ET_KEY2;
  uint32_t cyc;
  uint32_t sr = et_erase(&cyc);
  if(sr & ET_SR_ERRORS){ printf("ET ABORT: first erase refused, sr=0x%08lx\n", (unsigned long)sr); return; }
  for(unsigned c=0;c<ET_N_CASES;c++){
    for(unsigned r=0;r<ET_REPEAT;r++){
      /* the page is erased here: by the erase before the loop, or by the last one timed */
      unsigned n = 0;
      for(unsigned w=0;w<ET_PAGE_WORDS;w++){
        uint32_t v[ET_N32];
        if(!et_content(&et_cases[c], w, v)) continue;
        sr = et_program(w, v);
        if(sr & ET_SR_ERRORS){ printf("ET ABORT: program of unit %u refused, sr=0x%08lx\n", w, (unsigned long)sr); return; }
        n++;
      }
      sr = et_erase(&cyc);
      unsigned erased = 1;
      for(uint32_t a = ET_PAGE; a < ET_PAGE + ET_PAGE_SIZE; a += 4) if(0xFFFFFFFFu != *(volatile uint32_t*)a) erased = 0;
      const uint32_t us = cyc / mhz;
      printf("ET %-24s programmed=%3u erase %5lu us sr=0x%08lx erased=%u\n", et_cases[c].name, n, (unsigned long)us,
        (unsigned long)sr, erased);
      if((sr & ET_SR_ERRORS) || !erased){ printf("ET ABORT: the erase failed\n"); return; }
      programmed[c] = n;
      if((0 == r) || (us < us_min[c])) us_min[c] = us;
      if((0 == r) || (us > us_max[c])) us_max[c] = us;
    }
  }
  FLASH->NSCR |= FLASH_NSCR_LOCK;
  printf("\nET TABLE case | units programmed | erase us min | max\n");
  for(unsigned c=0;c<ET_N_CASES;c++){
    printf("ET ROW %-24s | %3u | %5lu | %5lu\n", et_cases[c].name, programmed[c], (unsigned long)us_min[c], (unsigned long)us_max[c]);
  }
  printf("ERASE-TIME DONE\n");
}
