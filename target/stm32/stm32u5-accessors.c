#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "lean-ftl.h"
#define STM32U5A5xx
#include <stm32u5xx.h>

/** @defgroup FLASH_Banks FLASH Banks
  * @{
  */
#define FLASH_BANK_1      0x00000001U                   /*!< Bank 1   */
#define FLASH_BANK_2      0x00000002U                   /*!< Bank 2   */
#define FLASH_BANK_BOTH   (FLASH_BANK_1 | FLASH_BANK_2) /*!< Bank1 and Bank2  */

/** @defgroup FLASH_Keys FLASH Keys
  * @{
  */
#define FLASH_KEY1   0x45670123U /*!< Flash key1 */
#define FLASH_KEY2   0xCDEF89ABU /*!< Flash key2: used with FLASH_KEY1
                                      to unlock the FLASH registers access */

#define NVM_PAGE_SIZE32 (FLASH_PAGE_SIZE/4)

#define NVM_PAGE_ADDR_MASK (~(FLASH_PAGE_SIZE-1))
#define NVM_BANK_ADDR_MASK (~(FLASH_BANK_SIZE-1))

#define NVM_PAGE_BASE(addr) ((void*)((uintptr_t)(addr) & NVM_PAGE_ADDR_MASK))
#define NVM_BANK_BASE(addr) ((void*)((uintptr_t)(addr) & NVM_BANK_ADDR_MASK))

#define NVM_SIZE_IN_PAGE(addr) ((uintptr_t)NVM_PAGE_BASE((uintptr_t)(addr)+FLASH_PAGE_SIZE) - (uintptr_t)(addr))

#define FLASH_WRITE_SIZE 16

#define WRITE_BIT(reg,bit,val) do{if(val) SET_BIT(reg,bit); else CLEAR_BIT(reg,bit);}while(0)

uint8_t __attribute__((weak)) nvm_read(void* dst, const void*const src_nvm_addr, uintptr_t size){
  memcpy(dst,src_nvm_addr,size);
  return 0;
}

static bool bank_swapped(){
  const uint32_t swap_bank = READ_BIT(FLASH->OPTR,FLASH_OPTR_SWAP_BANK);
  return swap_bank ? 1 : 0;
}

static bool icache_is_enabled(){
  return ((READ_BIT(ICACHE->CR, ICACHE_CR_EN) != 0U) ? 1UL : 0UL);
}

static void icache_disable(){
  WRITE_REG(ICACHE->FCR, ICACHE_FCR_CBSYENDF);

  CLEAR_BIT(ICACHE->CR, ICACHE_CR_EN);

  // Wait for instruction cache being disabled 
  while (READ_BIT(ICACHE->CR, ICACHE_CR_EN) != 0U);
}

static void icache_enable(){
  SET_BIT(ICACHE->CR, ICACHE_CR_EN);
}

static void __attribute__ ((section (".flash_write_funcs"))) nvm_ll_erase_page(uint64_t*page_addr){
  const uintptr_t bank_addr = (uintptr_t)NVM_BANK_BASE(page_addr);
  const uint32_t bank = (bank_addr - FLASH_BASE_NS)/FLASH_BANK_SIZE;
  const uint32_t page = ((uintptr_t)page_addr - bank_addr)/FLASH_PAGE_SIZE;
  
  //select the bank
  WRITE_BIT(FLASH_NS->NSCR, FLASH_NSCR_BKER,bank^bank_swapped());

  //select the page
  MODIFY_REG(FLASH_NS->NSCR, (FLASH_NSCR_PNB | FLASH_NSCR_PER), ((page << FLASH_NSCR_PNB_Pos) | FLASH_NSCR_PER));
  
  const bool icache_enabled = icache_is_enabled();
  if (icache_enabled){//need to disable icache, it treats write in cache-able areas as errors.
	  icache_disable();
  }
  //erase the page
  SET_BIT(FLASH_NS->NSCR, FLASH_NSCR_STRT);

  //wait end of operation
  while(READ_BIT(FLASH_NS->NSSR, FLASH_NSSR_BSY));
  WRITE_REG(FLASH_NS->NSCR, 0);//clear NSCR
  if (icache_enabled){
	  icache_enable();
  }
}

static void __attribute__ ((section (".flash_write_funcs"))) nvm_ll_write_unaligned_src(uint64_t*dst, const uint8_t* buf, uint32_t n_words32){
  uint32_t*dst32 = (uint32_t*)dst;
  const bool icache_enabled = icache_is_enabled();
  if (icache_enabled){//need to disable icache, it treats write in cache-able areas as errors.
	  icache_disable();
  }
  WRITE_REG(FLASH_NS->NSCR, FLASH_NSCR_PG);
  for(unsigned int i=0;i<n_words32;i+=4){
    uint32_t buf32[4];
    memcpy(buf32,buf,sizeof(buf32));
    buf+=sizeof(buf32);
    dst32[i] = buf32[0];
    dst32[i+1] = buf32[1];
    dst32[i+2] = buf32[2];
    dst32[i+3] = buf32[3];

    //wait end of operation
    while(READ_BIT(FLASH_NS->NSSR, FLASH_NSSR_BSY));
  }
  WRITE_REG(FLASH_NS->NSCR, 0);//clear PG
  if (icache_enabled){
	  icache_enable();
  }
}

static void __attribute__ ((section (".flash_write_funcs"))) nvm_ll_write(uint64_t*dst, const uint64_t*const buf, uint32_t n_words32){
  if((uintptr_t)buf % sizeof(uint32_t)) {
    nvm_ll_write_unaligned_src(dst,(const uint8_t*)buf,n_words32);
  } else {
    uint32_t*dst32 = (uint32_t*)dst;
    const uint32_t*const buf32 = (const uint32_t*const)buf;
    const bool icache_enabled = icache_is_enabled();
    if (icache_enabled){//need to disable icache, it treats write in cache-able areas as errors.
      icache_disable();
    }
    WRITE_REG(FLASH_NS->NSCR, FLASH_NSCR_PG);
    for(unsigned int i=0;i<n_words32;i+=4){
      dst32[i] = buf32[i];
      dst32[i+1] = buf32[i+1];
      dst32[i+2] = buf32[i+2];
      dst32[i+3] = buf32[i+3];

      //wait end of operation
      while(READ_BIT(FLASH_NS->NSSR, FLASH_NSSR_BSY));
    }
    WRITE_REG(FLASH_NS->NSCR, 0);//clear PG
    if (icache_enabled){
      icache_enable();
    }
  }
}

uint8_t __attribute__((weak)) nvm_erase(void*base_address, unsigned int n_pages){
  if(0 == n_pages) return 0;
  const uintptr_t size = n_pages * FLASH_PAGE_SIZE;
  if((uintptr_t)base_address < FLASH_BASE_NS) return 1;
  if(((uintptr_t)base_address + size) > (FLASH_BASE_NS + FLASH_SIZE)) return 2;
  if(0 != ((uintptr_t)base_address % FLASH_PAGE_SIZE)) return 3;
  /* Disable interrupts to avoid any interruption */
  const bool interrupts_enabled = (__get_PRIMASK() == 0);
  __disable_irq();
  //unlock flash write
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY1);
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY2);

  uint64_t*dst64 = (uint64_t*)base_address;
  for(unsigned int i=0;i<n_pages;i++){
    nvm_ll_erase_page(dst64);
    dst64 += SIZE64(FLASH_PAGE_SIZE);
  }

  //lock flash write
  SET_BIT(FLASH->NSCR, FLASH_NSCR_LOCK);
  // Restore backed-up-state
  if (interrupts_enabled) {
      __enable_irq();
  }
  return 0;
}

uint8_t __attribute__((weak)) nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(0 == size) return 0;
  if((uintptr_t)dst_nvm_addr < FLASH_BASE_NS) return 1;
  if(((uintptr_t)dst_nvm_addr + size) > (FLASH_BASE_NS + FLASH_SIZE)) return 2;
  if(0 != ((uintptr_t)dst_nvm_addr % FLASH_WRITE_SIZE)) return 3;
  if(0 != (size % FLASH_WRITE_SIZE)) return 4;
  /* Disable interrupts to avoid any interruption */
  const bool interrupts_enabled = (__get_PRIMASK() == 0);
  __disable_irq();
  //unlock flash write
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY1);
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY2);
  uint64_t*dst_nvm_addr64 = (uint64_t*)dst_nvm_addr;
  uint64_t*src64 = (uint64_t*)src;
  
  nvm_ll_write(dst_nvm_addr64, src64, size / sizeof(uint32_t));

  //lock flash write
  SET_BIT(FLASH->NSCR, FLASH_NSCR_LOCK);

  uint8_t fail = 0;
  fail = memcmp(dst_nvm_addr,src,size) ? 5 : 0;
  // Restore backed-up-state
  if (interrupts_enabled) {
      __enable_irq();
  }

  return fail;
}

