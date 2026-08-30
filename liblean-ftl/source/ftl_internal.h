#pragma once

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "lean-ftl.h"

#define EWLF_MAGIC 0x2CF292E376D73985

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

/**
 * \brief Report a fatal NVM-level error and never return.
 *
 * Calls the NVM's error handler if one is registered, then loops forever.
 * This is the last resort for the (unexpected) case where the registered
 * error handler itself returns, even though it is documented to never do so.
 *
 * \param ctx Properties of the NVM on which the error occurred
 * \param err_code The error code to report
 */
static void raise_nvm_error(lftl_nvm_props_t*ctx, uint32_t err_code){
  if((uintptr_t)(ctx->error_handler)!=(uintptr_t)LFTL_INVALID_POINTER){
    ctx->error_handler(err_code);
  }
  while(1);
}

/**
 * \brief Report a fatal error for an LFTL area and never return.
 *
 * Convenience wrapper around ::raise_nvm_error using the area's own NVM.
 *
 * \param ctx Context of the LFTL area on which the error occurred
 * \param err_code The error code to report
 */
static void raise_error(lftl_ctx_t*ctx, uint32_t err_code){
  raise_nvm_error(ctx->nvm_props, err_code);
}

/**
 * \brief Erase one or more physical pages, raising an error on failure.
 *
 * Thin wrapper around the user-provided ::nvm_erase_t callback. Does
 * nothing if `n_pages` is 0.
 *
 * \param ctx Properties of the target NVM
 * \param base_address Start address of the range to erase
 * \param n_pages Number of physical pages to erase
 */
static void nvm_erase(lftl_nvm_props_t*ctx, void*base_address, unsigned int n_pages){
  if(0==n_pages) return;
  uint8_t status = ctx->erase(base_address, n_pages);
  if(status) raise_nvm_error(ctx,LFTL_ERROR_LOW_LEVEL_ERASE | status);
}

/**
 * \brief Write physical NVM, raising an error on failure.
 *
 * Thin wrapper around the user-provided ::nvm_write_t callback. Does
 * nothing if `size` is 0.
 *
 * \param ctx Properties of the target NVM
 * \param dst_nvm_addr Destination address in NVM
 * \param src Source address
 * \param size Size in bytes to write
 */
static void nvm_write(lftl_nvm_props_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(0==size) return;
  uint8_t status = ctx->write(dst_nvm_addr, src, size);
  if(status) raise_nvm_error(ctx,LFTL_ERROR_LOW_LEVEL_WRITE | status);
}

/**
 * \brief Read physical NVM, raising an error on failure.
 *
 * Thin wrapper around the user-provided ::nvm_read_t callback. Does
 * nothing if `size` is 0.
 *
 * \param ctx Properties of the target NVM
 * \param dst Destination address, in volatile memory
 * \param src_nvm_addr Source address in NVM
 * \param size Size in bytes to read
 */
static void nvm_read(lftl_nvm_props_t*ctx, void* dst, const void*const src_nvm_addr, uintptr_t size){
  if(0==size) return;
  uint8_t status = ctx->read(dst, src_nvm_addr, size);
  if(status) raise_nvm_error(ctx,LFTL_ERROR_LOW_LEVEL_READ | status);
}

/**
 * \brief Compute (part of) a CRC32C checksum.
 *
 * This is only the core of the CRC computation: to get the full CRC,
 * initialize `crc` to -1 and complement the result of this function.
 * Can be called repeatedly on successive chunks of a larger buffer by
 * feeding the previous return value back in as `crc`.
 *
 * \param crc Initial/accumulated CRC value
 * \param buf Start address of the buffer
 * \param len Size in bytes of the buffer
 * \returns The updated (not yet complemented) CRC value
 */
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

/**
 * \brief Return the greater of two `uintptr_t` values.
 *
 * \param a First value
 * \param b Second value
 * \returns `a` if it is greater than `b`, `b` otherwise
 */
static uintptr_t max_uintptr(uintptr_t a,uintptr_t b){
  return a > b ? a : b;
}

/**
 * \brief Check whether an address falls within a byte range.
 *
 * \param addr Address to test
 * \param base Start address of the range
 * \param size Size in bytes of the range
 * \returns true if `base <= addr < base+size`, false otherwise
 */
static bool is_in_range(const void*const addr, const void*const base, uintptr_t size){
  if(addr < base) return false;
  if((uintptr_t)addr >= ((uintptr_t)base+size)) return false;
  return true;
}

/**
 * \brief Check whether a logical address is within an area's data.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address to test (always between `ctx->area`
 *   and `ctx->area+ctx->data_size`, if valid)
 * \returns true if `nvm_addr` is within `ctx`'s data range
 */
static bool is_in_data(lftl_ctx_t*ctx, const void*const nvm_addr){//nvm_addr is a logical address, so always between ctx->area and ctx->area+data_size
  return is_in_range(nvm_addr, ctx->area, ctx->data_size);
}

/**
 * \brief Search the registered areas other than `ctx` for one containing an address.
 *
 * Walks the circular linked list of registered areas (via `ctx->next`),
 * starting just after `ctx`, until it wraps back around to `ctx`.
 *
 * \param ctx Context to start the search after (excluded from the search itself)
 * \param nvm_addr Logical address to search for
 * \returns The matching context, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_ctx_t*get_other_ctx(lftl_ctx_t*ctx, const void*const nvm_addr){
  const lftl_ctx_t*stop=ctx;
  while(ctx->next != stop){
    if(LFTL_INVALID_POINTER==ctx->next) break;
    ctx = ctx->next;
    if(is_in_data(ctx,nvm_addr)) return ctx;
  }
  return LFTL_INVALID_POINTER;
}

/**
 * \brief Search `ctx` and the other registered areas for one containing an address.
 *
 * Checks `ctx` itself first, then falls back to ::get_other_ctx.
 *
 * \param ctx Context to check first (may be ::LFTL_INVALID_POINTER)
 * \param nvm_addr Logical address to search for
 * \returns The matching context, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_ctx_t*get_any_ctx(lftl_ctx_t*ctx, const void*const nvm_addr){
  if((LFTL_INVALID_POINTER!=ctx) && is_in_data(ctx,nvm_addr)) return ctx;
  return get_other_ctx(ctx,nvm_addr);
}

static lftl_ctx_t*first_area;
static lftl_ctx_t*first_area_ewlf;
static lftl_nvm_props_t*first_nvm;

/**
 * \brief Check whether more than one std area is currently registered.
 *
 * \returns true if 2 or more areas were registered via ::lftl_register_area
 */
static bool has_several_areas(){
  if(LFTL_INVALID_POINTER==first_area) return 0;
  return first_area->next != first_area;
}
/**
 * \brief Check whether more than one EWLF area is currently registered.
 *
 * \returns true if 2 or more areas were registered via ::lftl_register_area_ewlf
 */
static bool has_several_areas_ewlf(){
  if(LFTL_INVALID_POINTER==first_area_ewlf) return 0;
  return first_area_ewlf->next != first_area_ewlf;
}
/**
 * \brief Check whether more than one physical NVM is currently registered.
 *
 * \returns true if 2 or more ::lftl_nvm_props_t were registered via ::lftl_register_nvm
 */
static bool has_several_nvms(){
  if(LFTL_INVALID_POINTER==first_nvm) return 0;
  return first_nvm->next != first_nvm;
}

/**
 * \brief Link an area into a circular list and register its NVM.
 *
 * Shared implementation for ::lftl_register_area and ::lftl_register_area_ewlf:
 * inserts `ctx` into the circular linked list pointed to by `*first`
 * (creating a single-element self-loop if it was empty), then registers
 * `ctx->nvm_props` via ::lftl_register_nvm.
 *
 * \param ctx Context of the area to register
 * \param first Pointer to the head of the target list (::first_area or ::first_area_ewlf)
 */
static void lftl_register_area_core(lftl_ctx_t*ctx, lftl_ctx_t**first){
  lftl_ctx_t*prev = *first;
  if(LFTL_INVALID_POINTER==prev){
    *first = ctx;
    ctx->next = ctx;
  } else {
    prev->next = ctx;
    ctx->next = *first;
  }
  lftl_register_nvm(ctx->nvm_props);
}

/**
 * \brief Search a list of registered areas for one containing an address.
 *
 * \param addr Logical address to search for
 * \param first Head of the circular area list to search (::first_area or ::first_area_ewlf)
 * \returns The matching context, or ::LFTL_INVALID_POINTER if `first` is
 *   ::LFTL_INVALID_POINTER or no area matches
 */
static lftl_ctx_t*addr_to_area_core(const void*const addr, lftl_ctx_t*first){
  if(LFTL_INVALID_POINTER == first) return LFTL_INVALID_POINTER;
  lftl_ctx_t*ctx = first;
  ctx = get_any_ctx(ctx, addr); // we search first within LFTL areas to return the right ctx if several areas use the same NVM.
  return ctx;
}

/**
 * \brief Search the registered std areas for one containing an address.
 *
 * \param addr Logical address to search for
 * \returns The matching context, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_ctx_t*addr_to_area_std(const void*const addr){
  return addr_to_area_core(addr, first_area);
}

/**
 * \brief Search the registered EWLF areas for one containing an address.
 *
 * \param addr Logical address to search for
 * \returns The matching context, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_ctx_t*addr_to_area_ewlf(const void*const addr){
  return addr_to_area_core(addr, first_area_ewlf);
}

/**
 * \brief Search the registered physical NVMs for one containing an address.
 *
 * Walks the circular linked list of ::lftl_nvm_props_t built by
 * ::lftl_register_nvm.
 *
 * \param addr Physical address to search for
 * \returns The matching NVM properties, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_nvm_props_t*addr_to_nvm(const void*const addr){
  if(LFTL_INVALID_POINTER!=first_nvm){
    lftl_nvm_props_t*it = first_nvm;
    const lftl_nvm_props_t*stop=it;
    do{
      if(LFTL_INVALID_POINTER==it->next) break;
      if(is_in_range(addr, it->base, it->size)) return it;
      it = it->next;
    }while(it != stop);
  }
  return LFTL_INVALID_POINTER;
}

/**
 * \brief Resolve an address to its owning area (if any) and physical NVM.
 *
 * Searches std areas first, then EWLF areas, so that if the same NVM is
 * shared by several areas the correct owning context is returned via
 * `*ctx`. If `addr` is not within any registered area, falls back to
 * ::addr_to_nvm to check whether it is at least within some registered
 * physical NVM.
 *
 * \param addr Address to resolve
 * \param ctx Output: set to the owning context, or ::LFTL_INVALID_POINTER
 *   if `addr` is not within any registered area
 * \returns The NVM properties covering `addr`, or ::LFTL_INVALID_POINTER
 *   if `addr` is not within any registered area or physical NVM
 */
static lftl_nvm_props_t*addr_to_area_and_nvm(const void*const addr, lftl_ctx_t**ctx){
  // we search first within LFTL areas to return the right ctx if several areas use the same NVM.
  *ctx = addr_to_area_std(addr);
  if(LFTL_INVALID_POINTER == *ctx) *ctx = addr_to_area_ewlf(addr);
  if(LFTL_INVALID_POINTER!=*ctx) return (*ctx)->nvm_props;
  // addr is not in any LFTL areas of any kind, check other NVM addresses
  return addr_to_nvm(addr);
}

/**
 * \brief Read bytes from wherever `src` actually is.
 *
 * If `src` is a physical address within a registered NVM, reads it via
 * that NVM's ::nvm_read_t callback (needed since NVM may not be directly
 * memory-addressable). Otherwise, treats `src` as regular memory and
 * uses a plain `memcpy`.
 *
 * \param ctx Context of the LFTL area on whose behalf this read is done
 *   (used only for error reporting if `src` turns out to be invalid)
 * \param dst Destination address, in volatile memory
 * \param src Source address: an LFTL area, a physical NVM address, or regular memory
 * \param size Size in bytes to read
 */
static void mem_read(lftl_ctx_t*ctx, void*dst, const void*const src, uintptr_t size){
  lftl_nvm_props_t*nvm_props = addr_to_nvm(src);
  if(LFTL_INVALID_POINTER!=nvm_props){
    nvm_read(nvm_props,dst,src,size);
    return;
  }
  memcpy(dst,src,size);
}


/**
 * \brief Compute a full CRC32C checksum over a (possibly non-RAM) range.
 *
 * Reads the range in chunks via ::mem_read so that `src` can be an LFTL
 * area, a physical NVM address, or regular memory.
 *
 * \param ctx Context of the LFTL area on whose behalf this checksum is computed
 * \param src Start address of the range to checksum
 * \param size Size in bytes of the range
 * \returns The CRC32C checksum of the range
 */
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

/**
 * \brief Return the physical erase page size of an area's NVM.
 *
 * \param ctx Context of the LFTL area
 * \returns `ctx->nvm_props->erase_size`
 */
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

/**
 * \brief Physical size in bytes of a slot's meta-data block.
 *
 * Each of the ::LFTL_META_N_ITEMS meta items (version, checksum,
 * checksum2) occupies at least `sizeof(uint32_t)` bytes, rounded up to
 * the NVM's write unit size.
 *
 * \param ctx Context of the LFTL area
 * \returns Size in bytes of the meta-data block
 */
static uintptr_t meta_phy_size(lftl_ctx_t*ctx){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  return LFTL_META_N_ITEMS * item_size;
}

/**
 * \brief Number of physical pages occupied by one slot.
 *
 * \param ctx Context of the LFTL area
 * \returns `ceil((ctx->data_size + meta_phy_size(ctx)) / page_size(ctx))`
 */
static uintptr_t n_pages_in_slot(lftl_ctx_t*ctx){
  const uintptr_t min_size = ctx->data_size + meta_phy_size(ctx);
  const uintptr_t n_pages = (min_size + page_size(ctx)-1) / page_size(ctx);
  return n_pages;
}

/**
 * \brief Size in bytes of one slot, including its meta-data.
 *
 * \param ctx Context of the LFTL area
 * \returns `n_pages_in_slot(ctx) * page_size(ctx)`
 */
static uintptr_t slot_size(lftl_ctx_t*ctx){
  return n_pages_in_slot(ctx) * page_size(ctx);
}

/**
 * \brief Number of slots in an area.
 *
 * \param ctx Context of the LFTL area
 * \returns `ctx->area_size / slot_size(ctx)`
 */
static unsigned int n_slots(lftl_ctx_t*ctx){
  const uintptr_t size = slot_size(ctx);
  return ctx->area_size / size;
}

/**
 * \brief Physical address of the start of a slot.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot, in `[0, n_slots(ctx))`
 * \returns Address of `ctx->area + slot_index * slot_size(ctx)`
 */
static uint8_t* slot_base(lftl_ctx_t*ctx, unsigned int slot_index){
  return ((uint8_t*)ctx->area)+slot_index*slot_size(ctx);
}

/**
 * \brief Byte offset of the meta-data block within a slot.
 *
 * \param ctx Context of the LFTL area
 * \returns `slot_size(ctx) - meta_phy_size(ctx)`
 */
static uintptr_t meta_offset(lftl_ctx_t*ctx){
  return slot_size(ctx) - meta_phy_size(ctx);
}

/**
 * \brief Read the meta-data of a slot into `dst`.
 *
 * \param ctx Context of the LFTL area
 * \param dst Destination, in volatile memory
 * \param slot_index Index of the slot to read the meta-data of
 */
static void get_slot_meta(lftl_ctx_t*ctx, lftl_meta_t* dst, unsigned int slot_index){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int item_size = max_uintptr(write_size,sizeof(uint32_t));
  const uintptr_t meta_size = LFTL_META_N_ITEMS * item_size;
  uint32_t*phy_meta = (uint32_t*)(slot_base(ctx, slot_index) + meta_offset(ctx));
  meta_items_worst_case_t buf;
  nvm_read(ctx->nvm_props,buf,phy_meta,meta_size);
  for(unsigned int i = 0; i < LFTL_META_N_ITEMS; i++){
    dst->items[i] = buf[i*item_size/sizeof(uint32_t)];
  }
}

/**
 * \brief Read the version field of a slot's meta-data.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \returns The slot's stored version number
 */
static uint32_t get_slot_version(lftl_ctx_t*ctx, unsigned int slot_index){
  lftl_meta_t meta;
  get_slot_meta(ctx, &meta, slot_index);
  return meta.version;
}

/**
 * \brief Read the stored checksum field of a slot's meta-data.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \returns The slot's stored checksum (as last written, not recomputed)
 */
static uint32_t get_slot_checksum(lftl_ctx_t*ctx, unsigned int slot_index){
  lftl_meta_t meta;
  get_slot_meta(ctx, &meta, slot_index);
  return meta.checksum;
}

/**
 * \brief Recompute the checksum a slot's data and version should have.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \returns The checksum of the slot's current data, combined with its stored version
 */
static uint32_t compute_slot_checksum(lftl_ctx_t*ctx, unsigned int slot_index){
  lftl_meta_t meta;
  get_slot_meta(ctx,&meta,slot_index);
  const uint32_t sum = checksum(ctx,slot_base(ctx, slot_index),ctx->data_size) + meta.version;
  return sum;
}

/**
 * \brief Check whether a slot's stored checksum matches its actual content.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \returns true if the slot's data and version are internally consistent
 */
static bool slot_integrity_check_ok(lftl_ctx_t*ctx, unsigned int slot_index){
  return get_slot_checksum(ctx,slot_index) == compute_slot_checksum(ctx,slot_index);
}

/**
 * \brief Write a fully-formed meta-data block to a slot.
 *
 * Writes every meta item except checksum2 first, then checksum2 last, so
 * that a tearing event during this call is always detectable/recoverable
 * afterwards (see ::find_current_slot).
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot to write the meta-data of
 * \param meta The meta-data to write
 */
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
  nvm_write(ctx->nvm_props,meta_phy_addr,buf,meta_size - item_size);
  //write checksum2
  const uintptr_t checksum2_offset = meta_size - item_size;
  uint32_t*const checksum2_phy_addr = (uint32_t*)(base + meta_offset(ctx) + checksum2_offset);
  uint8_t*const checksum2_src = (uint8_t*)buf + checksum2_offset;
  nvm_write(ctx->nvm_props,checksum2_phy_addr,checksum2_src,item_size);
}

/**
 * \brief Compute and write a fresh meta-data block for a slot.
 *
 * Computes the checksum from the slot's current data and the given
 * version, then writes it via ::write_meta_core.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot to write the meta-data of
 * \param version The new version number for this slot
 */
static void write_meta(lftl_ctx_t*ctx, unsigned int slot_index, uint32_t version){
  uint8_t*const base = slot_base(ctx, slot_index);
  lftl_meta_t meta;
  meta.version = version;
  meta.checksum = checksum(ctx,base,ctx->data_size) + meta.version;
  meta.checksum2 = meta.checksum;
  write_meta_core(ctx,slot_index,&meta);
}

/**
 * \brief Locate the slot holding the current data and update `ctx->data`.
 *
 * Scans every slot for the highest version number whose integrity check
 * passes, raising ::LFTL_ERROR_VERSION_COLLISION or
 * ::LFTL_ERROR_NO_VALID_VERSION if the area is corrupted beyond recovery.
 * If the winning slot's checksum2 doesn't match checksum (a tearing
 * happened while writing the meta-data itself), repairs it in place.
 *
 * \param ctx Context of the LFTL area
 */
static void find_current_slot(lftl_ctx_t*ctx){
  const unsigned int ns = n_slots(ctx);
  const uint32_t invalid_index = 0xFFFFFFFF;
  uint32_t max_version_index = invalid_index;
  uint32_t max_version = 0;
  for(unsigned int i=0;i<ns;i++){
    const uint32_t version = get_slot_version(ctx,i);
    if(version == 0xFFFFFFFF) continue;
    if(version == max_version) {
      raise_error(ctx,LFTL_ERROR_VERSION_COLLISION);
    }
    if(version > max_version) {
      if(slot_integrity_check_ok(ctx,i)){
        max_version_index = i;
        max_version = version;
      }
    }
  }
  if(invalid_index == max_version_index) {
    raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
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

/**
 * \brief Translate a logical address into a physical one, for a std area.
 *
 * Finds the current slot first if `ctx->data` isn't known yet.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address to translate
 * \param size Size in bytes of the access, used to validate the range
 * \returns The corresponding physical address within the current slot
 */
static void*translate_addr_std(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  if(!is_in_data(ctx, nvm_addr)) raise_error(ctx,LFTL_ERROR_FIRST_NOT_IN_DATA);
  if(LFTL_INVALID_POINTER == ctx->data) find_current_slot(ctx);
  const uintptr_t offset = (uintptr_t)nvm_addr - (uintptr_t)ctx->area;
  if(offset+size > ctx->data_size) raise_error(ctx,LFTL_ERROR_LAST_NOT_IN_DATA);
  return (void*)((uintptr_t)ctx->data + offset);
}
/**
 * \brief Translate a logical address into a physical one, for an EWLF area.
 *
 * Currently identical to ::translate_addr_std; kept separate so the two
 * area kinds can diverge independently in the future.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address to translate
 * \param size Size in bytes of the access, used to validate the range
 * \returns The corresponding physical address within the current slot
 */
static void*translate_addr_ewlf(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  if(!is_in_data(ctx, nvm_addr)) raise_error(ctx,LFTL_ERROR_FIRST_NOT_IN_DATA);
  if(LFTL_INVALID_POINTER == ctx->data) find_current_slot(ctx);
  const uintptr_t offset = (uintptr_t)nvm_addr - (uintptr_t)ctx->area;
  if(offset+size > ctx->data_size) raise_error(ctx,LFTL_ERROR_LAST_NOT_IN_DATA);
  return (void*)((uintptr_t)ctx->data + offset);
}
/**
 * \brief Translate a logical address into a physical one.
 *
 * Dispatches to ::translate_addr_std or ::translate_addr_ewlf depending
 * on ::lftl_is_ewlf.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address to translate
 * \param size Size in bytes of the access, used to validate the range
 * \returns The corresponding physical address within the current slot
 */
static void*translate_addr(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  if(lftl_is_ewlf(ctx)){
    return translate_addr_ewlf(ctx,nvm_addr,size);
  }
  return translate_addr_std(ctx,nvm_addr,size);
}

/**
 * \brief Index of the slot currently holding `ctx->data`.
 *
 * \param ctx Context of the LFTL area
 * \returns The slot index derived from `ctx->data`'s offset within the area
 */
static unsigned int get_current_slot_index(lftl_ctx_t*ctx){
  const uintptr_t offset = (uintptr_t)ctx->data - (uintptr_t)ctx->area;
  return offset / slot_size(ctx);
}

/**
 * \brief Index of the slot to use for the next write.
 *
 * Round-robins to the slot right after the current one, wrapping back to
 * slot 0 once the area's last slot has been used.
 *
 * \param ctx Context of the LFTL area
 * \returns The index of the next slot to write
 */
static unsigned int next_slot(lftl_ctx_t*ctx){
  const uintptr_t area_limit = (uintptr_t)ctx->area+ctx->area_size;
  const uintptr_t next_slot_limit = (uintptr_t)ctx->data + 2*slot_size(ctx); // 1 slot for the current data, 1 slot for the next
  if(next_slot_limit > area_limit ){ // if equal, next slot is the last slot, we will wrap around next time
    return 0; //wrap around
  } else {
    return get_current_slot_index(ctx) + 1;
  }
}

/**
 * \brief Erase all physical pages of a slot.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot to erase
 */
static void erase_slot(lftl_ctx_t*ctx, unsigned int slot_index){
  void*base = slot_base(ctx, slot_index);
  const uint32_t slot_pages = n_pages_in_slot(ctx);
  nvm_erase(ctx->nvm_props,base,slot_pages);
}

/**
 * \brief Total number of physical pages in an area.
 *
 * \param ctx Context of the LFTL area
 * \returns `ctx->area_size / page_size(ctx)`
 */
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

/**
 * \brief Redirect a write's source address if it is itself managed by LFTL.
 *
 * If `*p_src_phy_addr` is a logical address belonging to a registered
 * area, this translates it to the actual physical address of the current
 * data and updates `*src_ctx` to that area's context, so that
 * ::mem_read/nvm_read can be used directly on it later. Otherwise leaves
 * both inputs untouched.
 *
 * \param src_ctx In/out: context to read `*p_src_phy_addr` through
 * \param p_src_phy_addr In/out: address to resolve
 * \param size Size in bytes of the access, used to validate the range
 */
static void check_src_phy_addr(lftl_ctx_t**src_ctx, const uint8_t**p_src_phy_addr, uintptr_t size){
  const uint8_t*src_phy_addr = *p_src_phy_addr;
  lftl_ctx_t* ctx = addr_to_area_std(src_phy_addr);
  if(LFTL_INVALID_POINTER!=ctx){
    if(is_in_data(ctx,src_phy_addr)){ // src is in an LFTL area
      *p_src_phy_addr = translate_addr_std(ctx, (void*)src_phy_addr, size);
      *src_ctx = ctx;
    }
  }else{
    ctx = addr_to_area_ewlf(src_phy_addr);
    if(LFTL_INVALID_POINTER!=ctx){
      if(is_in_data(ctx,src_phy_addr)){ // src is in an LFTL area
        *p_src_phy_addr = translate_addr_ewlf(ctx, (void*)src_phy_addr, size);
        *src_ctx = ctx;
      }
    }
  }
}

/**
 * \brief Low-level helper that assembles and writes one slot's new content.
 *
 * Copies the parts of the destination slot that are not being overwritten
 * from `current_base` (the previous slot), then writes the new data from
 * `src_phy_addr`, taking care of write-unit alignment at both ends of the
 * range so that every physical write is a whole number of write units.
 * Used by both ::write_core and ::write_ewlf.
 *
 * \param dst_ctx Context of the target LFTL area
 * \param offset Byte offset of the write within the area's data
 * \param size Size in bytes actually requested by the caller
 * \param dst_base Physical base address of the slot being written
 * \param current_base Physical base address of the slot being superseded
 * \param src_ctx Context to read `src_phy_addr` through (may differ from `dst_ctx`)
 * \param src_phy_addr Physical address of the source data
 * \param write_size The NVM's write unit size, in bytes
 * \param addr_misalignement How far `dst_nvm_addr_aligned` had to be moved down to align it
 * \param dst_nvm_addr_aligned Write-unit-aligned destination address
 * \param size_misalignement How far `size` is from a write-unit multiple
 * \param size_aligned Write-unit-aligned size of the range to write
 * \param transaction Whether this write is part of an ongoing transaction
 */
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
      nvm_write(dst_ctx->nvm_props, dst_base, current_base, offset);
    }
  }
  if(addr_misalignement){
    // fix up first WU
    uintptr_t size_consumed = write_size - addr_misalignement;
    if(size_consumed > size){
      const uintptr_t tail_size = size_consumed - size;
      size_consumed = size;
      nvm_read(dst_ctx->nvm_props, ((uint8_t*)&wu) + addr_misalignement + size, current_base+offset+ addr_misalignement+size, tail_size);
    }
    nvm_read(dst_ctx->nvm_props, &wu, current_base+offset, addr_misalignement);
    mem_read(src_ctx,((uint8_t*)&wu) + addr_misalignement, src_phy_addr , size_consumed);
    nvm_write(dst_ctx->nvm_props,dst_phy_addr,&wu,write_size);
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
  nvm_write(dst_ctx->nvm_props,dst_phy_addr,src_phy_addr,size_aligned);
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
    nvm_read(dst_ctx->nvm_props, wu_part2_base, wu_part2_src, wu_part2_size);
    nvm_write(dst_ctx->nvm_props, dst_base+last_wu_offset, &wu, write_size);
  }
  if(!transaction){
    const uintptr_t remaining = dst_ctx->data_size - end_offset;
    if(remaining){
      nvm_write(dst_ctx->nvm_props, dst_base+end_offset, current_base + end_offset, remaining);
    }
  }
}

/**
 * \brief Write to a std area, handling both transactional and immediate writes.
 *
 * Aligns the requested range to write units if needed, finds the next
 * slot to use, erases it first unless a transaction is ongoing, then
 * delegates the actual byte assembly to
 * ::write_src_phy_addr_to_dst_phy_addr. Outside of a transaction, also
 * bumps the version and commits the new meta-data immediately.
 *
 * \param dst_ctx Context of the target LFTL area
 * \param dst_nvm_addr Destination logical address
 * \param src Source address
 * \param size Size in bytes to write
 * \param transaction Whether this write is part of an ongoing transaction (::TRANSACTION or ::NO_TRANSACTION)
 * \param aligned Whether the caller has already guaranteed write-unit alignment (::ALIGNED or ::UNALIGNED)
 */
static void write_core(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool transaction, bool aligned){
  DEBUG_PRINTLN("write_core(%p,%p,%p,%u,%u,%u) entry",dst_ctx,dst_nvm_addr,src,size,transaction,aligned);
  //printf("src=%p, size=0x%08lx\n",src,size);
  const uint32_t write_size = dst_ctx->nvm_props->write_size;
  uintptr_t dst_nvm_addr_aligned;
  uintptr_t addr_misalignement;
  uintptr_t size_aligned;
  if(aligned){
    // check that the args are indeed aligned
    if(0 != ((uintptr_t)dst_nvm_addr % write_size)) raise_error(dst_ctx,LFTL_ERROR_BASE_MISALIGNED);
    if(0 != (size % write_size)) raise_error(dst_ctx,LFTL_ERROR_SIZE_MISALIGNED);
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
  if(dst_base == current_base) raise_error(dst_ctx,LFTL_INTERNAL_ERROR);
  const uint8_t* src_phy_addr = src;
  lftl_ctx_t* src_ctx = dst_ctx;
  check_src_phy_addr(&src_ctx,&src_phy_addr,size);
  if(!transaction){
    if(LFTL_INVALID_POINTER != dst_ctx->transaction_tracker) raise_error(dst_ctx,LFTL_ERROR_TRANSACTION_ONGOING);
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

/**
 * \brief Write to an EWLF area.
 *
 * EWLF areas never support transactions, so every write is immediate:
 * this always erases the next slot and commits a new version, unlike
 * ::write_core's non-transactional branch which it otherwise mirrors.
 *
 * \param dst_ctx Context of the target EWLF area
 * \param dst_nvm_addr Destination logical address
 * \param src Source address
 * \param size Size in bytes to write
 */
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
  if(dst_base == current_base) raise_error(dst_ctx,LFTL_INTERNAL_ERROR);
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

/**
 * \brief Logically erase a range of an area's data.
 *
 * Used by ::lftl_erase_all. Like ::write_core's non-transactional
 * branch, erases the next slot and copies over the parts of the range
 * outside `[dst_nvm_addr, dst_nvm_addr+size)`, but writes nothing new
 * inside that range, so it reads back as erased.
 *
 * \param ctx Context of the target LFTL area
 * \param dst_nvm_addr Start of the logical range to erase
 * \param size Size in bytes of the range to erase
 */
static void erase(lftl_ctx_t*ctx, void*const dst_nvm_addr, uintptr_t size){
  DEBUG_PRINTLN("erase entry");
  const uint32_t write_size = ctx->nvm_props->write_size;
  if(0 != ((uintptr_t)dst_nvm_addr % write_size)) raise_error(ctx,LFTL_ERROR_BASE_MISALIGNED);
  if(0 != (size % write_size)) raise_error(ctx,LFTL_ERROR_SIZE_MISALIGNED);
  const void*const current_phy_addr = translate_addr(ctx, dst_nvm_addr, size);
  const uint8_t*const current_base = slot_base(ctx,get_current_slot_index(ctx));
  const uintptr_t offset = (uintptr_t)current_phy_addr - (uintptr_t)ctx->data;
  const unsigned int index = next_slot(ctx);
  uint8_t*const base = slot_base(ctx, index);

  if(LFTL_INVALID_POINTER != ctx->transaction_tracker) raise_error(ctx,LFTL_ERROR_TRANSACTION_ONGOING);
  //erase next slot
  erase_slot(ctx,index);
  //write new data in next slot
  if(offset){
    nvm_write(ctx->nvm_props, base, current_base, offset);
  }

  const uintptr_t end_offset = offset+size;
  const uintptr_t remaining = ctx->data_size - end_offset;
  if(remaining){
    nvm_write(ctx->nvm_props, base+end_offset, current_base + end_offset, remaining);
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
