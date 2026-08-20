#include "ftl_internal.h"

const char* lftl_version(){
  return version;
}

uint64_t lftl_version_timestamp(){
  return version_timestamp;
}

const char* lftl_build_type(){
  return build_type;
}

void lftl_init_lib(){
  first_area = LFTL_INVALID_POINTER;
  last_area = LFTL_INVALID_POINTER;
  first_area_ewlf = LFTL_INVALID_POINTER;
  last_area_ewlf = LFTL_INVALID_POINTER;
}

void lftl_register_area(lftl_ctx_t*ctx){
  lftl_register_area_core(ctx, &first_area, &last_area);
}

void lftl_register_area_ewlf(lftl_ctx_t*ctx){
  ctx->transaction_tracker = LFTL_EWLF_MARKER;
  lftl_register_area_core(ctx, &first_area_ewlf, &last_area_ewlf);
}

void lftl_format(lftl_ctx_t*ctx){
  DEBUG_PRINTLN("lftl_format entry");
  if(ctx->nvm_props->write_size>LFTL_WU_MAX_SIZE) ctx->error_handler(LFTL_ERROR_WU_SIZE_TOO_LARGE);
  nvm_erase(ctx,ctx->area,n_pages(ctx));
  ctx->data = ctx->area;
  write_meta(ctx, 0, 1);
  DEBUG_PRINTLN("lftl_format exit");
}

lftl_ctx_t*lftl_get_ctx(const void*const addr){
  lftl_ctx_t*ctx = first_area;
  if(is_in_data(ctx,addr)) return ctx;
  return get_other_ctx(ctx,addr);
}

bool lftl_is_ewlf(const lftl_ctx_t*const ctx){
  return ctx->transaction_tracker == LFTL_EWLF_MARKER;
}

void lftl_erase_all(lftl_ctx_t*ctx){
  erase(ctx, ctx->area, ctx->data_size);//dst_nvm_addr is area because erase function does the address translation
}

void lftl_basic_write(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  if(0==size) return;
  write_core(dst_ctx,dst_nvm_addr,src,size,NO_TRANSACTION,UNALIGNED);
}

void lftl_read(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size){
  DEBUG_PRINTLN("lftl_read entry");
  if(0==size) return;
  const void*const phy_addr = translate_addr(ctx, src_nvm_addr, size);
  nvm_read(ctx,dst, phy_addr, size);
  DEBUG_PRINTLN("lftl_read exit");
}

void lftl_transaction_start(lftl_ctx_t*dst_ctx, void *const transaction_tracker){
  if(LFTL_INVALID_POINTER != dst_ctx->transaction_tracker) dst_ctx->error_handler(LFTL_ERROR_TRANSACTION_ONGOING);
  dst_ctx->transaction_tracker = transaction_tracker;
  const uint32_t size = LFTL_TRANSACTION_TRACKER_SIZE(dst_ctx);
  memset(dst_ctx->transaction_tracker,0,size);
  const unsigned int index = next_slot(dst_ctx);
  //erase next slot
  erase_slot(dst_ctx,index);
}

void lftl_transaction_write(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  if(LFTL_INVALID_POINTER == dst_ctx->transaction_tracker) dst_ctx->error_handler(LFTL_ERROR_NO_TRANSACTION);
  //check/update transaction tracker
  const uint32_t write_size = dst_ctx->nvm_props->write_size;
  const uint32_t n_write_units = size / write_size;
  const uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)dst_ctx->area;
  const uint32_t offset_wu = offset / write_size;
  uint8_t*tracker = (uint8_t*)dst_ctx->transaction_tracker;
  for(uintptr_t i = 0; i < n_write_units; i++){
    const uint32_t wu_index = offset_wu+i;
    const uint32_t byte_index = wu_index / BITS_PER_BYTE;
    const uint32_t bit_index = wu_index % BITS_PER_BYTE;
    const uint8_t mask = 1 << bit_index;
    if(tracker[byte_index] & mask) dst_ctx->error_handler(LFTL_ERROR_TRANSACTION_OVERWRITE);
    tracker[byte_index] |= mask;
  }
  write_core(dst_ctx,dst_nvm_addr,src,size,TRANSACTION, ALIGNED);
}

void lftl_transaction_write_any(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  const uint32_t write_size = dst_ctx->nvm_props->write_size;
  const uintptr_t addr_misalignement = ((uintptr_t)dst_nvm_addr % write_size);
  const bool addr_is_aligned = 0 == addr_misalignement;
  const bool size_is_aligned = 0 == (size % write_size);
  if(addr_is_aligned & size_is_aligned) {
    lftl_transaction_write(dst_ctx, dst_nvm_addr, src, size);
  } else {
    if(LFTL_INVALID_POINTER == dst_ctx->transaction_tracker) dst_ctx->error_handler(LFTL_ERROR_NO_TRANSACTION);
    //check/update transaction tracker
    const uintptr_t dst_nvm_addr_aligned = (uintptr_t)dst_nvm_addr - addr_misalignement;
    const uint32_t n_write_units = LFTL_DIV_CEIL(size+addr_misalignement,write_size);
    const uintptr_t offset = dst_nvm_addr_aligned - (uintptr_t)dst_ctx->area;
    const uint32_t offset_wu = offset / write_size;
    uint8_t*tracker = (uint8_t*)dst_ctx->transaction_tracker;
    for(uintptr_t i = 0; i < n_write_units; i++){
      const uint32_t wu_index = offset_wu+i;
      const uint32_t byte_index = wu_index / BITS_PER_BYTE;
      const uint32_t bit_index = wu_index % BITS_PER_BYTE;
      const uint8_t mask = 1 << bit_index;
      if(tracker[byte_index] & mask) dst_ctx->error_handler(LFTL_ERROR_TRANSACTION_OVERWRITE);
      tracker[byte_index] |= mask;
    }
    write_core(dst_ctx,dst_nvm_addr,src,size,TRANSACTION, UNALIGNED);
  }
  
}

void lftl_transaction_commit(lftl_ctx_t*dst_ctx){
  if(LFTL_INVALID_POINTER == dst_ctx->transaction_tracker) dst_ctx->error_handler(LFTL_ERROR_NO_TRANSACTION);
  //lookup transaction tracker and copy unwritten write units
  const uint32_t write_size = dst_ctx->nvm_props->write_size;
  const uint32_t n_write_units = dst_ctx->data_size / write_size;
  uintptr_t nvm_addr = (uintptr_t)dst_ctx->area;
  uint8_t*tracker = (uint8_t*)dst_ctx->transaction_tracker;
  uint32_t wu_cnt=0;
  for(uintptr_t i = 0; i < LFTL_DIV_CEIL(n_write_units,BITS_PER_BYTE); i++){
    const uint8_t track_byte = tracker[i];
    uint8_t mask = 1;
    for(unsigned int bi = 0; bi < BITS_PER_BYTE; bi++){
      if(0 == (track_byte & mask)){
        uint64_t buf[SIZE64(write_size)];
        lftl_read(dst_ctx,buf,(void*)nvm_addr,write_size);
        write_core(dst_ctx,(void*)nvm_addr,buf,write_size,TRANSACTION,ALIGNED);//TODO: optimize, at least by doing address translation once and calling nvm_write directly.
      }
      mask = mask << 1;
      nvm_addr += write_size;
      wu_cnt++;
      if(wu_cnt == n_write_units) break;
    }
  }
  //increment version and write new meta data in next slot
  const unsigned int index = next_slot(dst_ctx);
  uint8_t*const base = slot_base(dst_ctx, index);
  const uint32_t version = 1 + get_slot_version(dst_ctx, get_current_slot_index(dst_ctx));
  write_meta(dst_ctx, index, version);
  //update context
  dst_ctx->data = base;
  dst_ctx->transaction_tracker = LFTL_INVALID_POINTER;
}

void lftl_transaction_abort(lftl_ctx_t*dst_ctx){
  dst_ctx->transaction_tracker = LFTL_INVALID_POINTER;
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

void lftl_write(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  if(0==size) return;
  if(lftl_is_ewlf(dst_ctx)){
    write_ewlf(dst_ctx, dst_nvm_addr, src, size);
  }else{
    if(dst_ctx->transaction_tracker == LFTL_INVALID_POINTER){
      lftl_basic_write(dst_ctx, dst_nvm_addr, src, size);
    } else {
      lftl_transaction_write(dst_ctx, dst_nvm_addr, src, size);
    }
  }
}

void lftl_write_any(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  if(0==size) return;
  if(dst_ctx->transaction_tracker == LFTL_INVALID_POINTER){
    lftl_basic_write(dst_ctx, dst_nvm_addr, src, size);
  } else {
    lftl_transaction_write_any(dst_ctx, dst_nvm_addr, src, size);
  }
}

void lftl_read_newer(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size){
  if(0==size) return;
  DEBUG_PRINTLN("lftl_read_newer entry");
  if(ctx->transaction_tracker == LFTL_INVALID_POINTER){
    lftl_read(ctx, dst, src_nvm_addr, size);
  } else {
    lftl_transaction_read(ctx, dst, src_nvm_addr, size);
  }
  DEBUG_PRINTLN("lftl_read_newer exit");
}

void lftl_memread(void*dst, const void*const src, uintptr_t size){
  DEBUG_PRINTLN("lftl_memread entry");
  lftl_ctx_t*ctx = is_in_any_area(src);
  if(LFTL_INVALID_POINTER==ctx) { // regular memory
    memcpy(dst,src,size);
  } else { // NVM, in or out of any LFTL area
    if(is_in_data(ctx,src)) lftl_read(ctx,dst,src,size);
    else nvm_read(ctx,dst,src,size); // outside of LFTL area but within NVM
  }
  DEBUG_PRINTLN("lftl_memread exit");
}

void lftl_memread_newer(void*dst, const void*const src, uintptr_t size){
  DEBUG_PRINTLN("lftl_memread_newer entry");
  lftl_ctx_t*ctx = is_in_any_area(src);
  if(LFTL_INVALID_POINTER==ctx) { // regular memory
    memcpy(dst,src,size);
  } else { // NVM, in or out of any LFTL area
    if(is_in_data(ctx,src)) lftl_read_newer(ctx,dst,src,size);
    else nvm_read(ctx,dst,src,size); // outside of LFTL area but within NVM
  }
  DEBUG_PRINTLN("lftl_memread_newer exit");
}
