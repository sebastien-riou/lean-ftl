#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <errno.h> 

#include <sys/types.h>
#include <sys/stat.h>

#define SIMULATED_TEARING 0xFF

void write_file(const char*name, const void*const buf, size_t size);
void read_file(const char*name, void*const buf, size_t*size);

const uint32_t nvm_write_size = LFTL_WU_SIZE;
const uint32_t nvm_erase_size = LFTL_PAGE_SIZE;

uint64_t tearing_sim_cnt=0;
uint64_t tearing_sim_target_cnt = -1;

void tearing_sim_init(){
  tearing_sim_cnt=0;
  tearing_sim_target_cnt = -1;
}

uint32_t tearing_sim_get_max_target(){
  return tearing_sim_cnt / nvm_write_size;
}

void tearing_sim_set_target(uint64_t target_write){
  tearing_sim_cnt=0;
  tearing_sim_target_cnt = target_write * nvm_write_size;
}

uint32_t tearing_size(uint32_t size){
  if(tearing_sim_cnt + size > tearing_sim_target_cnt){
    uint32_t out = tearing_sim_cnt + size - tearing_sim_target_cnt;
    //reset counters
    tearing_sim_cnt=0;
    tearing_sim_target_cnt = -1;
    return out;
  }
  tearing_sim_cnt += size;
  return 0;
}

bool tearing_sim(void*base_address, uint32_t size){
  const uint32_t tsize = tearing_size(size);
  if(tsize){
    //corrupt the last tsize bytes to simulate a tearing
    //we prefer corrupting the data rather than exactly simulating 
    //a tearing that would keep old data because in some case
    //a tearing could go undetected (old data may match new data)
    const uint32_t offset = size - tsize;
    for(uint32_t i = offset;i<tsize;i++){
      uint8_t*dst = (uint8_t*)base_address+i;
      *dst ^= 0x55; 
    }
  } 
  return tsize ? 1:0;
}

extern const void* nvm_base;
extern uintptr_t nvm_size;
extern uint32_t nvm_alignement;
extern const char*save_nvm_file_name;

uint32_t get_alignement_requirement(uint32_t original_req){
  if(original_req > nvm_alignement) return nvm_alignement;
  return original_req;
}

uint8_t nvm_erase(void*base_address, unsigned int n_pages){
  const uintptr_t size = n_pages * nvm_erase_size;
  if(base_address < nvm_base) return 1;
  if(((uintptr_t)base_address + size) > ((uintptr_t)nvm_base + nvm_size)) return 2;
  //Linux makes it hard to get nvm aligned to large units like 4k or 8k, so we check against the minimum between the original constraint and the alignement of nvm
  if(0 != ((uintptr_t)base_address % get_alignement_requirement(nvm_erase_size))) return 3;
  
  memset(base_address, 0xFF, size);
  bool tearing = tearing_sim(base_address,size);
  if(save_nvm_file_name){
    write_file(save_nvm_file_name,nvm_base,nvm_size);
  }
  return tearing ? SIMULATED_TEARING:0;
}

uint8_t nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(dst_nvm_addr < nvm_base) return 1;
  if(((uintptr_t)dst_nvm_addr + size) > ((uintptr_t)nvm_base + nvm_size)) return 2;
  if(0 != ((uintptr_t)dst_nvm_addr % nvm_write_size)) return 3;
  if(0 != (size % nvm_write_size)) return 4;
  memcpy(dst_nvm_addr,src,size);
  bool tearing = tearing_sim(dst_nvm_addr,size);
  if(save_nvm_file_name){
    write_file(save_nvm_file_name,nvm_base,nvm_size);
  }
  return tearing ? SIMULATED_TEARING:0;
}

uint8_t nvm_read(void* dst, const void*const src_nvm_addr, uintptr_t size){
  memcpy(dst,src_nvm_addr,size);
  return 0;
}