#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define LFTL_DEFINE_HELPERS
#define WU_SIZE 1 //dummy value, not used
#define FLASH_SW_PAGE_SIZE 1 //dummy value, not used
#include "lean-ftl.h"

static void nvm_erase(lftl_ctx_t*ctx, void*base_address, unsigned int n_pages){
  uint8_t status = ctx->erase(base_address, n_pages);
  if(status) ctx->error_handler(LFTL_ERROR_LOW_LEVEL_ERASE | status);
}

static void nvm_write(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size){
  uint8_t status = ctx->write(dst_nvm_addr, src, size);
  if(status) ctx->error_handler(LFTL_ERROR_LOW_LEVEL_WRITE | status);
}

static void nvm_read(lftl_ctx_t*ctx, void* dst, const void*const src_nvm_addr, uintptr_t size){
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
  return LFTL_META_N_ITEMS * ctx->nvm_props->write_size;
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
  const uintptr_t meta_size = LFTL_META_N_ITEMS * write_size;
  uint32_t*phy_meta = (uint32_t*)(slot_base(ctx, slot_index) + meta_offset(ctx));
  meta_items_worst_case_t buf;
  nvm_read(ctx,buf,phy_meta,meta_size);
  for(unsigned int i = 0; i < LFTL_META_N_ITEMS; i++){
    dst->items[i] = buf[i*write_size/sizeof(uint32_t)];
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
  const uintptr_t meta_size = LFTL_META_N_ITEMS * write_size;
  uint8_t*const base = slot_base(ctx, slot_index);
  lftl_meta_t*meta_phy_addr = (lftl_meta_t*)(base + meta_offset(ctx));
  meta_items_worst_case_t buf = {0};
  for(unsigned int i = 0; i < LFTL_META_N_ITEMS; i++){
    buf[i*write_size/sizeof(uint32_t)] = meta->items[i];
  }
  //write everything but checksum2
  nvm_write(ctx,meta_phy_addr,buf,meta_size - write_size);
  //write checksum2
  const uintptr_t checksum2_offset = meta_size - write_size;
  uint32_t*const checksum2_phy_addr = (uint32_t*)(base + meta_offset(ctx) + checksum2_offset);
  uint8_t*const checksum2_src = (uint8_t*)buf + checksum2_offset;
  nvm_write(ctx,checksum2_phy_addr,checksum2_src,write_size);
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

static bool is_in_data(lftl_ctx_t*ctx, const void*const nvm_addr){
  return is_in_range(nvm_addr, ctx->area, ctx->data_size);
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
  nvm_erase(ctx,base,n_pages_in_slot(ctx));
}

static uintptr_t n_pages(lftl_ctx_t*ctx){
  return ctx->area_size / page_size(ctx);
}

static void write_core(lftl_ctx_t*ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool transaction){
  const uint32_t write_size = ctx->nvm_props->write_size;
  if(0 != ((uintptr_t)dst_nvm_addr % write_size)) ctx->error_handler(LFTL_ERROR_BASE_MISALIGNED);
  if(0 != (size % write_size)) ctx->error_handler(LFTL_ERROR_SIZE_MISALIGNED);
  const void*const current_phy_addr = translate_addr(ctx, dst_nvm_addr, size);
  const uint8_t*const current_base = slot_base(ctx,get_current_slot_index(ctx));
  const uintptr_t offset = (uintptr_t)current_phy_addr - (uintptr_t)ctx->data;
  const unsigned int index = next_slot(ctx);
  uint8_t*const base = slot_base(ctx, index);
  if(base == current_base) ctx->error_handler(LFTL_INTERNAL_ERROR);
  void*const phy_addr = base + offset;
  if(!transaction){
    if(LFTL_INVALID_POINTER != ctx->transaction_tracker) ctx->error_handler(LFTL_ERROR_TRANSACTION_ONGOING);
    //erase next slot
    erase_slot(ctx,index);
    //write new data in next slot
    if(offset){
      nvm_write(ctx, base, current_base, offset);
    }
  }
  nvm_write(ctx,phy_addr,src,size);
  if(!transaction){
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
  }
}

static void erase(lftl_ctx_t*ctx, void*const dst_nvm_addr, uintptr_t size){
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
}

#define xstr(s) str(s)
#define str(s) #s
static const char*version = xstr(GIT_VERSION);
static const uint64_t version_timestamp = VERSION_TIMESTAMP;
static const char*build_type = xstr(BUILD_TYPE);

const char* lftl_version(){
  return version;
}

uint64_t lftl_version_timestamp(){
  return version_timestamp;
}

const char* lftl_build_type(){
  return build_type;
}

void lftl_format(lftl_ctx_t*ctx){
  if(ctx->nvm_props->write_size>16) ctx->error_handler(LFTL_ERROR_WU_SIZE_TOO_LARGE);
  nvm_erase(ctx,ctx->area,n_pages(ctx));
  ctx->data = ctx->area;
  write_meta(ctx, 0, 1);
}

void lftl_erase_all(lftl_ctx_t*ctx){
  erase(ctx, ctx->area, ctx->data_size);//dst_nvm_addr is area because erase function does the address translation
}

void lftl_basic_write(lftl_ctx_t*ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  if(0==size) return;
  write_core(ctx,dst_nvm_addr,src,size,0);
}

void lftl_read(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size){
  if(0==size) return;
  const void*const phy_addr = translate_addr(ctx, src_nvm_addr, size);
  nvm_read(ctx,dst, phy_addr, size);
}

void lftl_transaction_start(lftl_ctx_t*ctx, void *const transaction_tracker){
  if(LFTL_INVALID_POINTER != ctx->transaction_tracker) ctx->error_handler(LFTL_ERROR_TRANSACTION_ONGOING);
  ctx->transaction_tracker = transaction_tracker;
  const uint32_t size = LFTL_TRANSACTION_TRACKER_SIZE(ctx);
  memset(ctx->transaction_tracker,0,size);
  const unsigned int index = next_slot(ctx);
  //erase next slot
  erase_slot(ctx,index);
}

void lftl_transaction_write(lftl_ctx_t*ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  if(LFTL_INVALID_POINTER == ctx->transaction_tracker) ctx->error_handler(LFTL_ERROR_NO_TRANSACTION);
  //check/update transaction tracker
  const uint32_t write_size = ctx->nvm_props->write_size;
  const uint32_t n_write_units = size / write_size;
  const uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)ctx->area;
  const uint32_t offset_wu = offset / write_size;
  uint8_t*tracker = (uint8_t*)ctx->transaction_tracker;
  for(uintptr_t i = 0; i < n_write_units; i++){
    const uint32_t wu_index = offset_wu+i;
    const uint32_t byte_index = wu_index / BITS_PER_BYTE;
    const uint32_t bit_index = wu_index % BITS_PER_BYTE;
    const uint8_t mask = 1 << bit_index;
    if(tracker[byte_index] & mask) ctx->error_handler(LFTL_ERROR_TRANSACTION_OVERWRITE);
    tracker[byte_index] |= mask;
  }
  write_core(ctx,dst_nvm_addr,src,size,1);
}

void lftl_transaction_commit(lftl_ctx_t*ctx){
  if(LFTL_INVALID_POINTER == ctx->transaction_tracker) ctx->error_handler(LFTL_ERROR_NO_TRANSACTION);
  //lookup transaction tracker and copy unwritten write units
  const uint32_t write_size = ctx->nvm_props->write_size;
  const uint32_t n_write_units = ctx->data_size / write_size;
  uintptr_t nvm_addr = (uintptr_t)ctx->area;
  uint8_t*tracker = (uint8_t*)ctx->transaction_tracker;
  uint32_t wu_cnt=0;
  for(uintptr_t i = 0; i < LFTL_DIV_CEIL(n_write_units,BITS_PER_BYTE); i++){
    const uint8_t track_byte = tracker[i];
    uint8_t mask = 1;
    for(unsigned int bi = 0; bi < BITS_PER_BYTE; bi++){
      if(0 == (track_byte & mask)){
        uint64_t buf[SIZE64(write_size)];
        lftl_read(ctx,buf,(void*)nvm_addr,write_size);
        write_core(ctx,(void*)nvm_addr,buf,write_size,1);//TODO: optimize, at least by doing address translation once and calling nvm_write directly.
      }
      mask = mask << 1;
      nvm_addr += write_size;
      wu_cnt++;
      if(wu_cnt == n_write_units) break;
    }
  }
  //increment version and write new meta data in next slot
  const unsigned int index = next_slot(ctx);
  uint8_t*const base = slot_base(ctx, index);
  const uint32_t version = 1 + get_slot_version(ctx, get_current_slot_index(ctx));
  write_meta(ctx, index, version);
  //update context
  ctx->data = base;
  ctx->transaction_tracker = LFTL_INVALID_POINTER;
}

void lftl_transaction_abort(lftl_ctx_t*ctx){
  ctx->transaction_tracker = LFTL_INVALID_POINTER;
}

void lftl_transaction_read(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size){
  if(LFTL_INVALID_POINTER == ctx->transaction_tracker) ctx->error_handler(LFTL_ERROR_NO_TRANSACTION);
  if(0==size) return;
  const uint32_t write_size = ctx->nvm_props->write_size;
  const uint32_t n_write_units = size / write_size;
  const uintptr_t offset = (uintptr_t)src_nvm_addr - (uintptr_t)ctx->area;
  const uint32_t offset_wu = offset / write_size;
  uint8_t*tracker = (uint8_t*)ctx->transaction_tracker;
  const unsigned int index = next_slot(ctx);
  uint8_t*const base = slot_base(ctx, index);
  uint8_t*dst8 = (uint8_t*)dst;
  for(uintptr_t i = 0; i < n_write_units; i++){
    const uint32_t wu_index = offset_wu+i;
    const uint32_t byte_index = wu_index / BITS_PER_BYTE;
    const uint32_t bit_index = wu_index % BITS_PER_BYTE;
    const uint8_t mask = 1 << bit_index;
    if(tracker[byte_index] & mask) {
      //read new data
      void*phy_addr = base + wu_index*write_size;
      nvm_read(ctx,dst8, phy_addr, write_size);
    }else{
      //read current data
      void*phy_addr = (uint8_t*)(ctx->data) + wu_index*write_size;
      nvm_read(ctx,dst8, phy_addr, write_size);
    }
    dst8 += write_size;
  }
}

void lftl_write(lftl_ctx_t*ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  if(0==size) return;
  if(ctx->transaction_tracker == LFTL_INVALID_POINTER){
    lftl_basic_write(ctx, dst_nvm_addr, src, size);
  } else {
    lftl_transaction_write(ctx, dst_nvm_addr, src, size);
  }
}

void lftl_read_newer(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size){
  if(0==size) return;
  if(ctx->transaction_tracker == LFTL_INVALID_POINTER){
    lftl_read(ctx, dst, src_nvm_addr, size);
  } else {
    lftl_transaction_read(ctx, dst, src_nvm_addr, size);
  }
}
