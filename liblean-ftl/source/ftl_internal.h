#pragma once

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "lean-ftl.h"

#define LFTL_EWLF_MARKER LFTL_INVALID_POINTER2

#ifdef LFTL_DEBUG
  #ifdef HAS_PRINTF
    #include <stdio.h>
    #define PRINTF(...) printf( __VA_ARGS__ );
  #else
    #define PRINTF(...)
  #endif 
  #define DEBUG_PRINTLN(...) do{PRINTF( __VA_ARGS__ );PRINTF("\n\r");}while(0)
#else
  #define PRINTF(...)
  #define DEBUG_PRINTLN(...)
#endif

#define NO_TRANSACTION 0
#define TRANSACTION 1

#define UNALIGNED 0
#define ALIGNED 1

static void nvm_erase(lftl_ctx_t*ctx, void*base_address, unsigned int n_pages){
  if(0==n_pages) return;
  uint8_t status = ctx->erase(base_address, n_pages);
  if(status) ctx->error_handler(LFTL_ERROR_LOW_LEVEL_ERASE | status);
}

static void nvm_write(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(0==size) return;
  uint8_t status = ctx->write(dst_nvm_addr, src, size);
  if(status) ctx->error_handler(LFTL_ERROR_LOW_LEVEL_WRITE | status);
}

static void nvm_read(lftl_ctx_t*ctx, void* dst, const void*const src_nvm_addr, uintptr_t size){
  if(0==size) return;
  uint8_t status = ctx->read(dst, src_nvm_addr, size);
  if(status) ctx->error_handler(LFTL_ERROR_LOW_LEVEL_READ | status);
}

static uint32_t crc32c(uint32_t crc, const void*const buf, unsigned int len) {
  const uint8_t*buf8 = (const uint8_t*)buf;
  //Its the core of the CRC only
  //to get full CRC: init crc=-1 and complement the result of that function
  uint32_t poly = 0x05EC76F1;
  while (len != 0) {
    crc = crc ^ *buf8++;
    for (unsigned int i = 0; i<8; i++) {
        uint32_t mask = -(crc & 1);
        crc = (crc >> 1) ^ (poly & mask);
    }
    len--;
  }
  return crc;
}

static uintptr_t max_uintptr(uintptr_t a,uintptr_t b){
  return a > b ? a : b;
}

static bool is_in_range(const void*const addr, const void*const base, uintptr_t size){
  if(addr < base) return false;
  if((uintptr_t)addr > ((uintptr_t)base+size)) return false;
  return true;
}

static bool is_in_nvm(lftl_ctx_t*ctx, const void*const addr){
  return is_in_range(addr, ctx->nvm_props->base, ctx->nvm_props->size);
}

static void mem_read(lftl_ctx_t*ctx, void*dst, const void*const src, uintptr_t size){
  if(is_in_nvm(ctx,src)) nvm_read(ctx,dst,src,size);
  else memcpy(dst,src,size);
}


static uint32_t checksum(lftl_ctx_t*ctx, const void*const src, uintptr_t size){
  const uint8_t*src8 = (const uint8_t*)src;
  uint64_t buf[16];
  uint32_t out=0xFFFFFFFF;
  while(size){
    const uint32_t readsize = size > sizeof(buf) ? sizeof(buf) : size;
    mem_read(ctx,buf,src8,readsize);
    out = crc32c(out,buf,readsize);
    size -= readsize;
    src8 += readsize;
  }
  return out;
}

static uintptr_t page_size(lftl_ctx_t*ctx){
  return ctx->nvm_props->erase_size;
}

typedef struct lftl_meta_struct { 
  union{
    uint32_t items[LFTL_META_N_ITEMS];
    struct {
      uint32_t version;
      uint32_t checksum;
      uint32_t checksum2;
    };
  };
} lftl_meta_t;
typedef uint32_t meta_items_worst_case_t[LFTL_META_N_ITEMS*4];//enough to support NVM with write size of 128 bits

static uintptr_t meta_phy_size(lftl_ctx_t*ctx){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  return LFTL_META_N_ITEMS * item_size;
}

static uintptr_t n_pages_in_slot(lftl_ctx_t*ctx){
  const uintptr_t min_size = ctx->data_size + meta_phy_size(ctx);
  const uintptr_t n_pages = (min_size + page_size(ctx)-1) / page_size(ctx);
  return n_pages;
}

static uintptr_t slot_size(lftl_ctx_t*ctx){
  return n_pages_in_slot(ctx) * page_size(ctx);
}

static unsigned int n_slots(lftl_ctx_t*ctx){
  const uintptr_t size = slot_size(ctx);
  return ctx->area_size / size;
}

static uint8_t* slot_base(lftl_ctx_t*ctx, unsigned int slot_index){
  return ((uint8_t*)ctx->area)+slot_index*slot_size(ctx);
}

static uintptr_t meta_offset(lftl_ctx_t*ctx){
  return slot_size(ctx) - meta_phy_size(ctx);
}

static void get_slot_meta(lftl_ctx_t*ctx, lftl_meta_t* dst, unsigned int slot_index){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int item_size = max_uintptr(write_size,sizeof(uint32_t));
  const uintptr_t meta_size = LFTL_META_N_ITEMS * item_size;
  uint32_t*phy_meta = (uint32_t*)(slot_base(ctx, slot_index) + meta_offset(ctx));
  meta_items_worst_case_t buf;
  nvm_read(ctx,buf,phy_meta,meta_size);
  for(unsigned int i = 0; i < LFTL_META_N_ITEMS; i++){
    dst->items[i] = buf[i*item_size/sizeof(uint32_t)];
  }
}

static uint32_t get_slot_version(lftl_ctx_t*ctx, unsigned int slot_index){
  lftl_meta_t meta;
  get_slot_meta(ctx, &meta, slot_index);
  return meta.version;
}

static uint32_t get_slot_checksum(lftl_ctx_t*ctx, unsigned int slot_index){
  lftl_meta_t meta;
  get_slot_meta(ctx, &meta, slot_index);
  return meta.checksum;
}

static uint32_t compute_slot_checksum(lftl_ctx_t*ctx, unsigned int slot_index){
  lftl_meta_t meta;
  get_slot_meta(ctx,&meta,slot_index);
  const uint32_t sum = checksum(ctx,slot_base(ctx, slot_index),ctx->data_size) + meta.version;
  return sum;
}

static bool slot_integrity_check_ok(lftl_ctx_t*ctx, unsigned int slot_index){
  return get_slot_checksum(ctx,slot_index) == compute_slot_checksum(ctx,slot_index);
}

static void write_meta_core(lftl_ctx_t*ctx, unsigned int slot_index, lftl_meta_t*meta){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int item_size = max_uintptr(write_size,sizeof(uint32_t));
  const uintptr_t meta_size = LFTL_META_N_ITEMS * item_size;
  uint8_t*const base = slot_base(ctx, slot_index);
  lftl_meta_t*meta_phy_addr = (lftl_meta_t*)(base + meta_offset(ctx));
  meta_items_worst_case_t buf = {0};
  for(unsigned int i = 0; i < LFTL_META_N_ITEMS; i++){
    buf[i*item_size/sizeof(uint32_t)] = meta->items[i];
  }
  //write everything but checksum2
  nvm_write(ctx,meta_phy_addr,buf,meta_size - item_size);
  //write checksum2
  const uintptr_t checksum2_offset = meta_size - item_size;
  uint32_t*const checksum2_phy_addr = (uint32_t*)(base + meta_offset(ctx) + checksum2_offset);
  uint8_t*const checksum2_src = (uint8_t*)buf + checksum2_offset;
  nvm_write(ctx,checksum2_phy_addr,checksum2_src,item_size);
}

static void write_meta(lftl_ctx_t*ctx, unsigned int slot_index, uint32_t version){
  uint8_t*const base = slot_base(ctx, slot_index);
  lftl_meta_t meta;
  meta.version = version;
  meta.checksum = checksum(ctx,base,ctx->data_size) + meta.version;
  meta.checksum2 = meta.checksum;
  write_meta_core(ctx,slot_index,&meta);
}

static void find_current_slot(lftl_ctx_t*ctx){
  const unsigned int ns = n_slots(ctx);
  const uint32_t invalid_index = 0xFFFFFFFF;
  uint32_t max_version_index = invalid_index;
  uint32_t max_version = 0;
  for(unsigned int i=0;i<ns;i++){
    const uint32_t version = get_slot_version(ctx,i);
    if(version == 0xFFFFFFFF) continue;
    if(version == max_version) {
      ctx->error_handler(LFTL_ERROR_VERSION_COLLISION);
    }
    if(version > max_version) {
      if(slot_integrity_check_ok(ctx,i)){
        max_version_index = i;
        max_version = version;
      }
    }
  }
  if(invalid_index == max_version_index) {
    ctx->error_handler(LFTL_ERROR_NO_VALID_VERSION);
  }
  ctx->data = slot_base(ctx, max_version_index);
  //check integrity of checksum2
  lftl_meta_t meta;
  get_slot_meta(ctx, &meta, max_version_index);
  if(meta.checksum2 != meta.checksum){
    //A tearing happened during programming of checksum or checksum2
    //we reprogram the whole meta again 
    //(because checksum may have been weakly programmed and checksum2 not at all)
    meta.checksum2 = meta.checksum;
    write_meta_core(ctx,max_version_index,&meta);
  }
}

static bool is_in_data(lftl_ctx_t*ctx, const void*const nvm_addr){//nvm_addr is a logical address, so always between ctx->area and ctx->area+data_size
  return is_in_range(nvm_addr, ctx->area, ctx->data_size);
}

static lftl_ctx_t*get_other_ctx(lftl_ctx_t*ctx, const void*const nvm_addr){
  const lftl_ctx_t*stop=ctx;
  while(ctx->next != stop){
    if(LFTL_INVALID_POINTER==ctx->next) break;
    ctx = ctx->next;
    if(is_in_data(ctx,nvm_addr)) return ctx;
  }
  return LFTL_INVALID_POINTER;
}

static lftl_ctx_t*get_any_ctx(lftl_ctx_t*ctx, const void*const nvm_addr){
  if(is_in_data(ctx,nvm_addr)) return ctx;
  return get_other_ctx(ctx,nvm_addr);
}

static lftl_ctx_t*first_area = LFTL_INVALID_POINTER;
static lftl_ctx_t*last_area = LFTL_INVALID_POINTER;
static lftl_ctx_t*first_area_ewlf = LFTL_INVALID_POINTER;
static lftl_ctx_t*last_area_ewlf = LFTL_INVALID_POINTER;

static bool has_several_areas(){
  return first_area != last_area;
}
static bool has_several_areas_ewlf(){
  return first_area_ewlf != last_area_ewlf;
}

static lftl_ctx_t*is_in_any_area_core(const void*const addr, lftl_ctx_t*first){
  lftl_ctx_t*ctx = first;
  ctx = get_any_ctx(ctx, addr); // we search first within LFTL areas to return the right ctx if several areas use the same NVM.
  if(LFTL_INVALID_POINTER!=ctx) return ctx;
  // addr is not in LFTL areas, check other NVM addresses
  ctx = first;
  if(is_in_nvm(ctx,addr)) return ctx;
  if(has_several_areas()){
    const lftl_ctx_t*stop=ctx;
    while(ctx->next != stop){
      if(LFTL_INVALID_POINTER==ctx->next) break;
      ctx = ctx->next;
      if(is_in_nvm(ctx,addr)) return ctx;
    }
  }
  return LFTL_INVALID_POINTER;
}

static lftl_ctx_t*is_in_any_area(const void*const addr){
  return is_in_any_area_core(addr, first_area);
}

static lftl_ctx_t*is_in_any_area_ewlf(const void*const addr){
  return is_in_any_area_core(addr, first_area_ewlf);
}

static void*translate_addr(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  if(!is_in_data(ctx, nvm_addr)) ctx->error_handler(LFTL_ERROR_FIRST_NOT_IN_DATA);
  if(LFTL_INVALID_POINTER == ctx->data) find_current_slot(ctx);
  const uintptr_t offset = (uintptr_t)nvm_addr - (uintptr_t)ctx->area;
  if(offset+size > ctx->data_size) ctx->error_handler(LFTL_ERROR_LAST_NOT_IN_DATA);
  return (void*)((uintptr_t)ctx->data + offset);
}

static unsigned int get_current_slot_index(lftl_ctx_t*ctx){
  const uintptr_t offset = (uintptr_t)ctx->data - (uintptr_t)ctx->area;
  return offset / slot_size(ctx);
}

static unsigned int next_slot(lftl_ctx_t*ctx){
  const uintptr_t area_limit = (uintptr_t)ctx->area+ctx->area_size;
  const uintptr_t next_slot_limit = (uintptr_t)ctx->data + 2*slot_size(ctx); // 1 slot for the current data, 1 slot for the next
  if(next_slot_limit > area_limit ){ // if equal, next slot is the last slot, we will wrap around next time
    return 0; //wrap around
  } else {
    return get_current_slot_index(ctx) + 1;
  }
}

static void erase_slot(lftl_ctx_t*ctx, unsigned int slot_index){
  void*base = slot_base(ctx, slot_index);
  const uint32_t slot_pages = n_pages_in_slot(ctx);
  nvm_erase(ctx,base,slot_pages);
}

static uintptr_t n_pages(lftl_ctx_t*ctx){
  return ctx->area_size / page_size(ctx);
}
/*
#include <stdio.h>
void dbg_memcpy(void*dst, const void*const src, uintptr_t size){
  printf("memcpy: dst=%p, src=%p, size=0x%08lx\n",dst,src,size);
  uint8_t*dst8 = (uint8_t*)dst;
  const uint8_t*const src8 = (uint8_t*)src;
  for(unsigned int i = 0;i<size;i++){
    dst8[i]=src8[i];
  }
}
*/

//if *src_phy_addr is managed by LFTL
//  correct it to target the valid data
//  correct *src_ctx to point to the right context
//else
//  leave inputs intact
static void check_src_phy_addr(lftl_ctx_t**src_ctx, const uint8_t**p_src_phy_addr, uintptr_t size){
  const uint8_t*src_phy_addr = *p_src_phy_addr;
  lftl_ctx_t* ctx = is_in_any_area(src_phy_addr);
  if(LFTL_INVALID_POINTER!=ctx){
    if(is_in_data(ctx,src_phy_addr)){ // src is in an LFTL area
      *p_src_phy_addr = translate_addr(ctx, (void*)src_phy_addr, size);
      *src_ctx = ctx;
    }
  }else{
    ctx = is_in_any_area_ewlf(src_phy_addr);
    if(LFTL_INVALID_POINTER!=ctx){
      if(is_in_data(ctx,src_phy_addr)){ // src is in an LFTL area
        *p_src_phy_addr = translate_addr(ctx, (void*)src_phy_addr, size);
        *src_ctx = ctx;
      }
    }
  }
}

static void write_src_phy_addr_to_dst_phy_addr(
  lftl_ctx_t*dst_ctx,
  uintptr_t offset,
  uintptr_t size,
  uint8_t*const dst_base,
  const uint8_t*const current_base,
  lftl_ctx_t*src_ctx,
  const uint8_t* src_phy_addr,
  uint32_t write_size,
  uintptr_t addr_misalignement,
  uintptr_t dst_nvm_addr_aligned,
  uint32_t size_misalignement,
  uintptr_t size_aligned,
  bool transaction
){
  const uintptr_t end_offset = offset+size_aligned;
  uint64_t wu[SIZE64(write_size)];
  memset(&wu,0x55,sizeof(wu));
  void* dst_phy_addr = dst_base + offset;
  if(!transaction){
    if(offset){
      nvm_write(dst_ctx, dst_base, current_base, offset);
    }
  }
  if(addr_misalignement){
    // fix up first WU
    uintptr_t size_consumed = write_size - addr_misalignement;
    if(size_consumed > size){
      const uintptr_t tail_size = size_consumed - size;
      size_consumed = size;
      nvm_read(dst_ctx, ((uint8_t*)&wu) + addr_misalignement + size, current_base+offset+ addr_misalignement+size, tail_size);
    }
    nvm_read(dst_ctx, &wu, current_base+offset, addr_misalignement);
    mem_read(src_ctx,((uint8_t*)&wu) + addr_misalignement, src_phy_addr , size_consumed);
    nvm_write(dst_ctx,dst_phy_addr,&wu,write_size);
    // adjust write range
    offset += write_size;
    dst_phy_addr = dst_base + offset;
    src_phy_addr += size_consumed;
    size_aligned -= write_size;
    size -= size_consumed;
  }
  if(size != size_aligned){
    //remove the last write unit from the write range as it needs special handling
    size_aligned -= write_size;
  }
  //at this point size_aligned >= size
  nvm_write(dst_ctx,dst_phy_addr,src_phy_addr,size_aligned);
  if(size != size_aligned){
    const uintptr_t last_wu_offset = offset+size_aligned;
    // fix up last WU
    uintptr_t size_misalignement = size % write_size;//((uintptr_t)dst_nvm_addr + size) % write_size;
    const uintptr_t wu_part1_size = size_misalignement;
    const uint8_t* wu_part1_src = ((uint8_t*)src_phy_addr) + size_aligned;
    mem_read(src_ctx,&wu,wu_part1_src, wu_part1_size);
    uint8_t*const wu_part2_base = ((uint8_t*)&wu) + size_misalignement;
    const uintptr_t wu_part2_size = write_size - size_misalignement;
    const uint8_t* wu_part2_src = current_base + last_wu_offset + size_misalignement;
    nvm_read(dst_ctx, wu_part2_base, wu_part2_src, wu_part2_size);
    nvm_write(dst_ctx, dst_base+last_wu_offset, &wu, write_size);
  }
  if(!transaction){
    const uintptr_t remaining = dst_ctx->data_size - end_offset;
    if(remaining){
      nvm_write(dst_ctx, dst_base+end_offset, current_base + end_offset, remaining);
    }
  }
}

static void write_core(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool transaction, bool aligned){
  DEBUG_PRINTLN("write_core(%p,%p,%p,%u,%u,%u) entry",dst_ctx,dst_nvm_addr,src,size,transaction,aligned);
  //printf("src=%p, size=0x%08lx\n",src,size);
  const uint32_t write_size = dst_ctx->nvm_props->write_size;
  uintptr_t dst_nvm_addr_aligned;
  uintptr_t addr_misalignement;
  uintptr_t size_aligned;
  if(aligned){
    // check that the args are indeed aligned
    if(0 != ((uintptr_t)dst_nvm_addr % write_size)) dst_ctx->error_handler(LFTL_ERROR_BASE_MISALIGNED);
    if(0 != (size % write_size)) dst_ctx->error_handler(LFTL_ERROR_SIZE_MISALIGNED);
    addr_misalignement = 0;
    dst_nvm_addr_aligned = (uintptr_t)dst_nvm_addr;
    size_aligned = size;
  } else {
    // unaligned: extend the start address and end address to start of WU and end of WU respectively
    addr_misalignement = ((uintptr_t)dst_nvm_addr % write_size);
    dst_nvm_addr_aligned = (uintptr_t)dst_nvm_addr - addr_misalignement;
    size_aligned = size + addr_misalignement;
    if(0 != (size_aligned % write_size)){
      size_aligned += write_size - size_aligned % write_size;
    }
  }
  const uint32_t size_misalignement = size % write_size;

  const void*const current_phy_addr = translate_addr(dst_ctx, (void*)dst_nvm_addr_aligned, size_aligned);
  const uint8_t*const current_base = slot_base(dst_ctx,get_current_slot_index(dst_ctx));
  uintptr_t offset = (uintptr_t)current_phy_addr - (uintptr_t)dst_ctx->data;
  //const uintptr_t end_offset = offset+size_aligned;
  const unsigned int index = next_slot(dst_ctx);
  uint8_t*const dst_base = slot_base(dst_ctx, index);
  if(dst_base == current_base) dst_ctx->error_handler(LFTL_INTERNAL_ERROR);
  const uint8_t* src_phy_addr = src;
  lftl_ctx_t* src_ctx = dst_ctx;
  check_src_phy_addr(&src_ctx,&src_phy_addr,size);
  if(!transaction){
    if(LFTL_INVALID_POINTER != dst_ctx->transaction_tracker) dst_ctx->error_handler(LFTL_ERROR_TRANSACTION_ONGOING);
    //erase next slot
    erase_slot(dst_ctx,index);
  }
  //write new data in next slot
  write_src_phy_addr_to_dst_phy_addr(
    dst_ctx,
    offset,
    size,
    dst_base,
    current_base,
    src_ctx,
    src_phy_addr,
    write_size,
    addr_misalignement,
    dst_nvm_addr_aligned,
    size_misalignement,
    size_aligned,
    transaction      
  );
  if(!transaction){
    //increment version and write new meta data in next slot
    const uint32_t version = 1 + get_slot_version(dst_ctx, get_current_slot_index(dst_ctx));
    write_meta(dst_ctx, index, version);
    //update context
    dst_ctx->data = dst_base;
  }
  DEBUG_PRINTLN("write_core exit");
}

static void write_ewlf(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  DEBUG_PRINTLN("write_ewlf(%p,%p,%p,%u) entry",dst_ctx,dst_nvm_addr,src,size);
  //printf("src=%p, size=0x%08lx\n",src,size);
  const uint32_t write_size = dst_ctx->nvm_props->write_size;
  const uintptr_t addr_misalignement = ((uintptr_t)dst_nvm_addr % write_size);
  const uintptr_t dst_nvm_addr_aligned = (uintptr_t)dst_nvm_addr - addr_misalignement;
  const uint32_t size_misalignement = size % write_size;
  const bool aligned = (0 == addr_misalignement) && (0 == size_misalignement);
  uintptr_t size_aligned = size + addr_misalignement;
  if(!aligned){
    // unaligned: extend the start address and end address to start of WU and end of WU respectively
    if(0 != (size_aligned % write_size)){
      size_aligned += write_size - size_aligned % write_size;
    }
  }
  const void*const current_phy_addr = translate_addr(dst_ctx, (void*)dst_nvm_addr_aligned, size_aligned);
  const uint8_t*const current_base = slot_base(dst_ctx,get_current_slot_index(dst_ctx));
  uintptr_t offset = (uintptr_t)current_phy_addr - (uintptr_t)dst_ctx->data;
  const unsigned int index = next_slot(dst_ctx);
  uint8_t*const dst_base = slot_base(dst_ctx, index);
  if(dst_base == current_base) dst_ctx->error_handler(LFTL_INTERNAL_ERROR);
  const uint8_t* src_phy_addr = src;
  lftl_ctx_t* src_ctx = dst_ctx;
  check_src_phy_addr(&src_ctx,&src_phy_addr,size);
  //erase next slot
  erase_slot(dst_ctx,index);
  //write new data in next slot
  write_src_phy_addr_to_dst_phy_addr(
    dst_ctx,
    offset,
    size,
    dst_base,
    current_base,
    src_ctx,
    src_phy_addr,
    write_size,
    addr_misalignement,
    dst_nvm_addr_aligned,
    size_misalignement,
    size_aligned,
    0 // no transaction      
  );
  //increment version and write new meta data in next slot
  const uint32_t version = 1 + get_slot_version(dst_ctx, get_current_slot_index(dst_ctx));
  write_meta(dst_ctx, index, version);
  //update context
  dst_ctx->data = dst_base;

  DEBUG_PRINTLN("write_ewlf exit");
}

static void erase(lftl_ctx_t*ctx, void*const dst_nvm_addr, uintptr_t size){
  DEBUG_PRINTLN("erase entry");
  const uint32_t write_size = ctx->nvm_props->write_size;
  if(0 != ((uintptr_t)dst_nvm_addr % write_size)) ctx->error_handler(LFTL_ERROR_BASE_MISALIGNED);
  if(0 != (size % write_size)) ctx->error_handler(LFTL_ERROR_SIZE_MISALIGNED);
  const void*const current_phy_addr = translate_addr(ctx, dst_nvm_addr, size);
  const uint8_t*const current_base = slot_base(ctx,get_current_slot_index(ctx));
  const uintptr_t offset = (uintptr_t)current_phy_addr - (uintptr_t)ctx->data;
  const unsigned int index = next_slot(ctx);
  uint8_t*const base = slot_base(ctx, index);

  if(LFTL_INVALID_POINTER != ctx->transaction_tracker) ctx->error_handler(LFTL_ERROR_TRANSACTION_ONGOING);
  //erase next slot
  erase_slot(ctx,index);
  //write new data in next slot
  if(offset){
    nvm_write(ctx, base, current_base, offset);
  }

  const uintptr_t end_offset = offset+size;
  const uintptr_t remaining = ctx->data_size - end_offset;
  if(remaining){
    nvm_write(ctx, base+end_offset, current_base + end_offset, remaining);
  }
  //increment version and write new meta data in next slot
  const uint32_t version = 1 + get_slot_version(ctx, get_current_slot_index(ctx));
  write_meta(ctx, index, version);
  //update context
  ctx->data = base;
  DEBUG_PRINTLN("erase exit");
}

#define xstr(s) str(s)
#define str(s) #s
static const char*version = xstr(GIT_VERSION);
static const uint64_t version_timestamp = VERSION_TIMESTAMP;
static const char*build_type = xstr(BUILD_TYPE);

static void lftl_register_area_core(lftl_ctx_t*ctx, lftl_ctx_t**first, lftl_ctx_t**last){
  if(LFTL_INVALID_POINTER==*first){
    *first = ctx;
  } else {
    (*last)->next = ctx;
  }
  *last = ctx;
  ctx->next = *first;
}
