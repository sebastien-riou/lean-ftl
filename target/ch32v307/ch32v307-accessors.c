#include <stdbool.h>
#include <string.h>
#define FLASH_SIZE (288*1024)
#include "ch32v30x_flash.h"


#ifdef LFTL_CH32V307
#define LFTL_PAGE_SIZE (4*1024)
#define LFTL_WU_SIZE 2
#endif

#ifdef HAS_PRINTF
  #include <stdio.h>
  #define PRINTF(...) printf( __VA_ARGS__ )
#else
  #define PRINTF(...)
#endif
#define PRINTLN(...) do{PRINTF( __VA_ARGS__ );PRINTF("\n\r");}while(0)
#ifdef LFTL_DEBUG
  #define DEBUG_PRINTF(...) PRINTF( __VA_ARGS__ )
  #define DEBUG_PRINTLN(...) PRINTLN( __VA_ARG__ )
  #define DEBUG_DUMP_CORE(addr,size,display_addr) dump_core(addr,size,display_addr)
  #define DEBUG_DUMP(addr,size) dump(addr,size)
#else
  #define DEBUG_PRINTF(...)
  #define DEBUG_PRINTLN(...)
  #define DEBUG_DUMP_CORE(addr,size,display_addr)
  #define DEBUG_DUMP(addr,size)
#endif

uint8_t __attribute__ ((section (".flash_write_funcs"))) nvm_erase(void*base_address, unsigned int n_pages){
  const uintptr_t size = n_pages * LFTL_PAGE_SIZE;
  DEBUG_PRINTLN("nvm_erase @0x%08x, %u bytes",(uintptr_t)base_address,size);
  if(0 == n_pages) return 0;
  uintptr_t ba = (uintptr_t)base_address;
  if(ba < FLASH_BASE) {
  ba |= FLASH_BASE;//handle flash address aliasing
  };
  if((ba + size) > (FLASH_BASE + FLASH_SIZE)) return 2;
  if(0 != (ba % LFTL_PAGE_SIZE)) return 3;
  FLASH_Status FLASHStatus;
  FLASH_Unlock();
  FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
  uint8_t status = 0;
  for(uint32_t i = 0; i < n_pages; i++){
    FLASHStatus = FLASH_ErasePage(ba + (LFTL_PAGE_SIZE * i)); //Erase 4KB
    if(FLASHStatus != FLASH_COMPLETE){
      DEBUG_PRINTLN("FLASH Erase Fail");
      status = 4;
      break;
    }
  }
  FLASH_Lock();
  return status;
}
uint8_t __attribute__ ((section (".flash_write_funcs"))) nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  #if LFTL_WU_SIZE != 2
    #error "nvm_write accessor supports only LFTL_WU_SIZE = 2"
  #endif
  DEBUG_PRINTF("nvm_write ");
  DEBUG_DUMP_CORE((uintptr_t)src,size,(uintptr_t)dst_nvm_addr);
  if(0 == size) return 0;
  uintptr_t dst = (uintptr_t)dst_nvm_addr;
  if(dst < FLASH_BASE) {
    dst |= FLASH_BASE;//handle flash address aliasing
  };
  if((dst + size) > (FLASH_BASE + FLASH_SIZE)) return 2;
  if(0 != (dst % LFTL_WU_SIZE)) return 3;
  if(0 != (size % LFTL_WU_SIZE)) return 4;

  FLASH_Unlock();
  FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
  
  uint8_t status = 0;
  uint32_t dsti = (uint32_t)dst;
  uint32_t srci = (uint32_t)src;
  const bool src_aligned = 0 == (srci % LFTL_WU_SIZE);
  for(uint32_t i=0;i<size / LFTL_WU_SIZE;i++){
    uint16_t dat;
    if(src_aligned) dat = *((uint16_t*)srci);
    else {
      dat = *((uint8_t*)srci+1);
      dat = (dat << 8);
      dat |=  *((uint8_t*)srci);
    }
    FLASH_Status FLASHStatus = FLASH_ProgramHalfWord(dsti, dat);
    if(FLASHStatus != FLASH_COMPLETE){
      DEBUG_PRINTLN("FLASH Write Fail");
      status = 5;
      break;
    }
    dsti += LFTL_WU_SIZE;
    srci += LFTL_WU_SIZE;
  }

  FLASH_Lock();
  if(status) return status;
  uint8_t fail = 0;
  fail = memcmp(dst_nvm_addr,src,size) ? 6 : 0;
  if(fail){
    DEBUG_DUMP((uintptr_t)src,size);
    DEBUG_DUMP((uintptr_t)dst_nvm_addr,size);
  }
  return fail;
}

uint8_t __attribute__ ((section (".flash_write_funcs"))) nvm_read(void* dst, const void*const src_nvm_addr, uintptr_t size){
  DEBUG_PRINTF("nvm_read ");
  memcpy(dst,src_nvm_addr,size);
  DEBUG_DUMP_CORE((uintptr_t)dst,size,(uintptr_t)src_nvm_addr);
  return 0;
}
