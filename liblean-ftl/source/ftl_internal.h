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
 * \brief Report a fatal error for an operation on an LFTL area and never return.
 *
 * Aborts the area's transaction, if any, before calling ::raise_nvm_error:
 * after an error the transaction may be inconsistent (tracker bits set for
 * write units never written), and the error handler may unwind the frame
 * holding the tracker.
 *
 * \param ctx Context of the LFTL area on whose behalf the operation was done,
 *   or ::LFTL_INVALID_POINTER if none
 * \param nvm_props Properties of the NVM on which the error occurred
 * \param err_code The error code to report
 */
static void raise_area_error(lftl_ctx_t*ctx, lftl_nvm_props_t*nvm_props, uint32_t err_code){
  if(LFTL_INVALID_POINTER != ctx) ctx->transaction_tracker = LFTL_INVALID_POINTER;
  raise_nvm_error(nvm_props, err_code);
}

/**
 * \brief Report a fatal error for an LFTL area and never return.
 *
 * Convenience wrapper around ::raise_area_error using the area's own NVM.
 *
 * \param ctx Context of the LFTL area on which the error occurred
 * \param err_code The error code to report
 */
static void raise_error(lftl_ctx_t*ctx, uint32_t err_code){
  raise_area_error(ctx, ctx->nvm_props, err_code);
}

/**
 * \brief Erase one or more physical pages, raising an error on failure.
 *
 * Thin wrapper around the user-provided ::nvm_erase_t callback. Does
 * nothing if `n_pages` is 0.
 *
 * \param ctx Context of the LFTL area on whose behalf this is done, see ::raise_area_error
 * \param nvm_props Properties of the target NVM
 * \param base_address Start address of the range to erase
 * \param n_pages Number of physical pages to erase
 */
static void nvm_erase(lftl_ctx_t*ctx, lftl_nvm_props_t*nvm_props, void*base_address, unsigned int n_pages){
  if(0==n_pages) return;
  uint8_t status = nvm_props->erase(base_address, n_pages);
  if(status) raise_area_error(ctx,nvm_props,LFTL_ERROR_LOW_LEVEL_ERASE | status);
}

/**
 * \brief Write physical NVM, raising an error on failure.
 *
 * Thin wrapper around the user-provided ::nvm_write_t callback. Does
 * nothing if `size` is 0.
 *
 * \param ctx Context of the LFTL area on whose behalf this is done, see ::raise_area_error
 * \param nvm_props Properties of the target NVM
 * \param dst_nvm_addr Destination address in NVM
 * \param src Source address
 * \param size Size in bytes to write
 */
static void nvm_write(lftl_ctx_t*ctx, lftl_nvm_props_t*nvm_props, void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(0==size) return;
  uint8_t status = nvm_props->write(dst_nvm_addr, src, size);
  if(status) raise_area_error(ctx,nvm_props,LFTL_ERROR_LOW_LEVEL_WRITE | status);
}

/**
 * \brief Read physical NVM, raising an error on failure.
 *
 * Thin wrapper around the user-provided ::nvm_read_t callback. Does
 * nothing if `size` is 0.
 *
 * \param ctx Context of the LFTL area on whose behalf this is done, see ::raise_area_error
 * \param nvm_props Properties of the target NVM
 * \param dst Destination address, in volatile memory
 * \param src_nvm_addr Source address in NVM
 * \param size Size in bytes to read
 */
static void nvm_read(lftl_ctx_t*ctx, lftl_nvm_props_t*nvm_props, void* dst, const void*const src_nvm_addr, uintptr_t size){
  if(0==size) return;
  uint8_t status = nvm_props->read(dst, src_nvm_addr, size);
  if(status) raise_area_error(ctx,nvm_props,LFTL_ERROR_LOW_LEVEL_READ | status);
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
//Note: gcov/gcovr reports this whole function as 0 hits with one branch in
//the billions (a "suspicious hits" warning at coverage time), despite both
//outcomes of both checks below being genuinely exercised by tests -- this
//is a heavily-inlined, extremely hot static helper, in the same class as
//crc32c() and page_size() below; a known GCC/gcov counter-attribution
//quirk at -Og for such functions, not a real coverage gap.
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
 * \brief Check that a logical range is within an area's data, raise an error otherwise.
 *
 * The end of the range is checked without computing `nvm_addr+size`, so a
 * `size` close to `UINTPTR_MAX` cannot wrap around and pass.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address of the start of the range
 * \param size Size in bytes of the range
 */
static void check_in_data(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  if(!is_in_data(ctx, nvm_addr)) raise_error(ctx,LFTL_ERROR_FIRST_NOT_IN_DATA);
  const uintptr_t offset = (uintptr_t)nvm_addr - (uintptr_t)ctx->area;
  if(size > ctx->data_size - offset) raise_error(ctx,LFTL_ERROR_LAST_NOT_IN_DATA);
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
    //Defensive only: lftl_register_area(_ewlf) always keeps the list
    //properly circular, so an in-list node's `next` is never
    //LFTL_INVALID_POINTER; this `break` cannot be reached by any test.
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
  if(LFTL_INVALID_POINTER==*first){
    *first = ctx;
    ctx->next = ctx;
  } else {
    lftl_ctx_t*old_second = (*first)->next;
    (*first)->next = ctx;
    ctx->next = old_second;
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
      //Defensive only: lftl_register_nvm always keeps the list properly
      //circular, so an in-list node's `next` is never LFTL_INVALID_POINTER;
      //this `break` cannot be reached by any test.
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
 *   (its transaction is aborted if the read fails, see ::raise_area_error)
 * \param dst Destination address, in volatile memory
 * \param src Source address: an LFTL area, a physical NVM address, or regular memory
 * \param size Size in bytes to read
 */
static void mem_read(lftl_ctx_t*ctx, void*dst, const void*const src, uintptr_t size){
  lftl_nvm_props_t*nvm_props = addr_to_nvm(src);
  if(LFTL_INVALID_POINTER!=nvm_props){
    nvm_read(ctx,nvm_props,dst,src,size);
    return;
  }
  memcpy(dst,src,size);
}


/**
 * \brief Update a CRC32C checksum with a (possibly non-RAM) range.
 *
 * Reads the range in chunks via ::mem_read so that `src` can be an LFTL
 * area, a physical NVM address, or regular memory.
 *
 * \param ctx Context of the LFTL area on whose behalf this checksum is computed
 * \param out Checksum so far, 0xFFFFFFFF to start one
 * \param src Start address of the range to checksum
 * \param size Size in bytes of the range
 * \returns The updated checksum
 */
static uint32_t checksum_update(lftl_ctx_t*ctx, uint32_t out, const void*const src, uintptr_t size){
  const uint8_t*src8 = (const uint8_t*)src;
  uint64_t buf[16];
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
 * \brief Compute a full CRC32C checksum over a (possibly non-RAM) range.
 *
 * \param ctx Context of the LFTL area on whose behalf this checksum is computed
 * \param src Start address of the range to checksum
 * \param size Size in bytes of the range
 * \returns The CRC32C checksum of the range
 */
static uint32_t checksum(lftl_ctx_t*ctx, const void*const src, uintptr_t size){
  return checksum_update(ctx,0xFFFFFFFF,src,size);
}

/**
 * \brief Update a CRC32C checksum with erased (0xFF) bytes.
 *
 * \param out Checksum so far, 0xFFFFFFFF to start one
 * \param size Number of erased bytes
 * \returns The updated checksum
 */
static uint32_t checksum_update_erased(uint32_t out, uintptr_t size){
  const uint8_t erased = 0xFF;
  while(size--){
    out = crc32c(out,&erased,1);
  }
  return out;
}

/**
 * \brief Check whether a physical range reads as erased.
 *
 * \param ctx Context of the LFTL area
 * \param addr Physical address of the range
 * \param size Size in bytes of the range
 * \returns true if every byte of the range reads 0xFF
 */
static bool is_erased(lftl_ctx_t*ctx, const uint8_t*addr, uintptr_t size){
  uint64_t buf[16];
  while(size){
    const uintptr_t readsize = size > sizeof(buf) ? sizeof(buf) : size;
    nvm_read(ctx,ctx->nvm_props,buf,addr,readsize);
    const uint8_t*const buf8 = (const uint8_t*)buf;
    for(uintptr_t i = 0; i < readsize; i++){
      if(0xFF != buf8[i]) return false;
    }
    size -= readsize;
    addr += readsize;
  }
  return true;
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

/**
 * \brief Total number of physical pages in an area.
 *
 * \param ctx Context of the LFTL area
 * \returns `ctx->area_size / page_size(ctx)`
 */
static uintptr_t n_pages(lftl_ctx_t*ctx){
  return ctx->area_size / page_size(ctx);
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

/**
 * \brief Physical size in bytes of an area's data: `data_size` rounded up to
 * whole write units.
 *
 * Writes cover whole write units, so a slot or record always has room for
 * that much data, even when `data_size` ends in the middle of a write unit.
 *
 * \param ctx Context of the LFTL area
 */
static uintptr_t data_phy_size(lftl_ctx_t*ctx){
  const uint32_t write_size = ctx->nvm_props->write_size;
  return LFTL_DIV_CEIL(ctx->data_size, write_size) * write_size;
}

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
 * \brief Read one meta-data item's whole write unit.
 *
 * The items are read one at a time, so that a word left weakly programmed
 * by a tearing is read only when needed: on flash with ECC each read of it
 * can raise an NMI (THREAT-MODEL.md T1.13).
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \param item Index of the item: 0 version, 1 checksum, 2 checksum2
 * \param unit Destination, `max(write_size, sizeof(uint32_t))` bytes, in volatile memory
 */
static void get_slot_item(lftl_ctx_t*ctx, unsigned int slot_index, unsigned int item, uint32_t*unit){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  const uint8_t*const phy = slot_base(ctx, slot_index) + meta_offset(ctx) + item*item_size;
  nvm_read(ctx,ctx->nvm_props,unit,phy,item_size);
}

/**
 * \brief Read the version field of a slot's meta-data.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \returns The slot's stored version number
 */
static uint32_t get_slot_version(lftl_ctx_t*ctx, unsigned int slot_index){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  uint32_t unit[LFTL_DIV_CEIL(item_size,sizeof(uint32_t))];
  get_slot_item(ctx,slot_index,0,unit);
  return unit[0];
}

/**
 * \brief Read the stored checksum field of a slot's meta-data.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \returns The slot's stored checksum (as last written, not recomputed)
 */
static uint32_t get_slot_checksum(lftl_ctx_t*ctx, unsigned int slot_index){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  uint32_t unit[LFTL_DIV_CEIL(item_size,sizeof(uint32_t))];
  get_slot_item(ctx,slot_index,1,unit);
  return unit[0];
}

/**
 * \brief Read the checksum2 field of a slot's meta-data, and whether its
 * whole write unit reads erased.
 *
 * The bytes next to the value are programmed to zero with it, so the whole
 * unit tells a checksum2 that was never programmed from one whose program
 * was torn, see ::find_current_slot.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \param all_erased Output: true if every byte of the write unit reads 0xFF
 * \returns The slot's stored checksum2
 */
static uint32_t get_slot_checksum2(lftl_ctx_t*ctx, unsigned int slot_index, bool*all_erased){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  uint32_t unit[LFTL_DIV_CEIL(item_size,sizeof(uint32_t))];
  get_slot_item(ctx,slot_index,2,unit);
  const uint8_t*const unit8 = (const uint8_t*)unit;
  *all_erased = true;
  for(unsigned int i=0;i<item_size;i++){
    if(0xFF != unit8[i]){
      *all_erased = false;
      break;
    }
  }
  return unit[0];
}

/**
 * \brief Check whether a slot's stored checksum matches its data and a version.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \param version The slot's version, as already read by the caller
 * \returns true if the stored checksum is the checksum of the data plus `version`
 */
static bool slot_checksum_valid(lftl_ctx_t*ctx, unsigned int slot_index, uint32_t version){
  return get_slot_checksum(ctx,slot_index) == checksum(ctx,slot_base(ctx, slot_index),ctx->data_size) + version;
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
  uint32_t buf[LFTL_DIV_CEIL(meta_size,sizeof(uint32_t))];
  memset(buf,0,sizeof(buf));
  for(unsigned int i = 0; i < LFTL_META_N_ITEMS; i++){
    buf[i*item_size/sizeof(uint32_t)] = meta->items[i];
  }
  //write everything but checksum2
  nvm_write(ctx,ctx->nvm_props,meta_phy_addr,buf,meta_size - item_size);
  //write checksum2
  const uintptr_t checksum2_offset = meta_size - item_size;
  uint32_t*const checksum2_phy_addr = (uint32_t*)(base + meta_offset(ctx) + checksum2_offset);
  uint8_t*const checksum2_src = (uint8_t*)buf + checksum2_offset;
  nvm_write(ctx,ctx->nvm_props,checksum2_phy_addr,checksum2_src,item_size);
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
  nvm_erase(ctx,ctx->nvm_props,base,slot_pages);
}

/**
 * \brief Whether a slot other than `except` is valid.
 *
 * Used by ::find_current_slot before it erases a torn slot, so that the
 * area is never left without a valid slot by its own doing. One checksum
 * per other slot, in that rare case only.
 *
 * \param ctx Context of the LFTL area
 * \param except Index of the slot to leave out
 * \returns true if another slot's checksum matches its data and version
 */
static bool other_valid_slot(lftl_ctx_t*ctx, unsigned int except){
  const unsigned int ns = n_slots(ctx);
  for(unsigned int i=0;i<ns;i++){
    if(i == except) continue;
    const uint32_t version = get_slot_version(ctx,i);
    if(version == 0xFFFFFFFF) continue;
    if(slot_checksum_valid(ctx,i,version)) return true;
  }
  return false;
}

/**
 * \brief Locate the slot holding the current data and update `ctx->data`.
 *
 * Scans every slot for the highest version number whose checksum matches
 * its data and version, raising ::LFTL_ERROR_VERSION_COLLISION if two such
 * slots share that version, or ::LFTL_ERROR_NO_VALID_VERSION if none
 * passes. A slot whose checksum does not match is skipped whatever its
 * version word: a tearing during its erase leaves the previous content with
 * random bits set, which can read as any version, the current one included.
 *
 * A write programs the data, then version and checksum, then checksum2, so
 * when the winning slot's checksum2 does not match, a tearing left it either
 * erased (the cut came during the checksum word at the latest: that word may
 * be weakly programmed, and read differently at a later boot) or partially
 * programmed (its own program had started, so the checksum is complete).
 * Only the whole write unit tells the two apart (::get_slot_checksum2).
 * In the first case the slot is erased and the scan runs again: the previous
 * version is current, its meta-data complete, and no boot ever returned the
 * torn slot, whose checksum2 never matched. In the second case the slot is
 * kept as it is. The torn slot is never copied, so the previous version is
 * never erased on the strength of a word that may read differently later
 * (ISSUES.md LFTL-009). A torn slot that is the only valid one is kept too:
 * nothing in normal operation produces that state, and keeping it beats
 * having nothing.
 *
 * \param ctx Context of the LFTL area
 */
static void find_current_slot(lftl_ctx_t*ctx){
  const unsigned int ns = n_slots(ctx);
  const uint32_t invalid_index = 0xFFFFFFFF;
  //a second pass at most: the slot erased by the first one is then invalid
  for(unsigned int pass = 0; pass < 2; pass++){
    uint32_t max_version_index = invalid_index;
    uint32_t max_version = 0;
    for(unsigned int i=0;i<ns;i++){
      const uint32_t version = get_slot_version(ctx,i);
      if(version == 0xFFFFFFFF) continue;
      const bool found_one = invalid_index != max_version_index;
      if(found_one && (version < max_version)) continue;
      if(!slot_checksum_valid(ctx,i,version)) continue;
      if(found_one && (version == max_version)) {
        raise_error(ctx,LFTL_ERROR_VERSION_COLLISION);
      }
      max_version_index = i;
      max_version = version;
    }
    if(invalid_index == max_version_index) {
      raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
    }
    bool checksum2_erased;
    const uint32_t checksum2 = get_slot_checksum2(ctx, max_version_index, &checksum2_erased);
    const bool complete = checksum2 == get_slot_checksum(ctx, max_version_index);
    if(complete || !checksum2_erased || !other_valid_slot(ctx, max_version_index)){
      ctx->data = slot_base(ctx, max_version_index);
      return;
    }
    //torn during the checksum word at the latest: erase it, the previous
    //version is current. ctx->data stays unset until then, so an error here
    //leaves nothing to trust
    erase_slot(ctx, max_version_index);
  }
  raise_error(ctx,LFTL_INTERNAL_ERROR);
}

/**
 * \brief Physical size in bytes of an EWLF record's magic field.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_magic_phy_size(lftl_ctx_t*ctx){
  return max_uintptr(ctx->nvm_props->write_size, sizeof(uint64_t));
}

/**
 * \brief Physical size in bytes of an EWLF record's `prev_erased` flag.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_prev_erased_phy_size(lftl_ctx_t*ctx){
  return max_uintptr(ctx->nvm_props->write_size, sizeof(uint8_t));
}

/**
 * \brief Physical size in bytes of an EWLF record's whole header
 * (magic + prev_erased + version/checksum/checksum2).
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_header_phy_size(lftl_ctx_t*ctx){
  return ewlf_magic_phy_size(ctx) + ewlf_prev_erased_phy_size(ctx) + meta_phy_size(ctx);
}

/**
 * \brief Physical size in bytes of an EWLF record's data, including pad.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_data_phy_size(lftl_ctx_t*ctx){
  return data_phy_size(ctx);
}

/**
 * \brief Physical size in bytes of one whole EWLF record, including pad.
 *
 * Rounded up to a multiple of `sizeof(uint64_t)`: since records are
 * placed at successive `record_phy_size` offsets from an erase unit's
 * (8-byte-aligned) top address, this padding keeps every record, and so
 * its 64-bit magic, 8-byte aligned.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_record_phy_size(lftl_ctx_t*ctx){
  const uintptr_t raw_size = ewlf_header_phy_size(ctx) + ewlf_data_phy_size(ctx);
  return LFTL_DIV_CEIL(raw_size, sizeof(uint64_t)) * sizeof(uint64_t);
}

/**
 * \brief Physical address of the start of an EWLF erase unit.
 *
 * One EWLF erase unit is exactly one physical page.
 *
 * \param ctx Context of the EWLF area
 * \param unit_index Index of the erase unit, in `[0, n_pages(ctx))`
 */
static uint8_t* ewlf_unit_base(lftl_ctx_t*ctx, unsigned int unit_index){
  return ((uint8_t*)ctx->area) + unit_index*page_size(ctx);
}

/**
 * \brief Physical address of the first (topmost) record slot of an EWLF erase unit.
 *
 * \param ctx Context of the EWLF area
 * \param unit_index Index of the erase unit, in `[0, n_pages(ctx))`
 */
static uint8_t* ewlf_unit_top_record(lftl_ctx_t*ctx, unsigned int unit_index){
  return ewlf_unit_base(ctx,unit_index) + page_size(ctx) - ewlf_record_phy_size(ctx);
}

/**
 * \brief Read the version/checksum/checksum2 meta-data of an EWLF record.
 *
 * \param ctx Context of the EWLF area
 * \param dst Destination, in volatile memory
 * \param record_addr Physical address of the start of the record (its magic field)
 */
static void ewlf_get_record_meta(lftl_ctx_t*ctx, lftl_meta_t*dst, const uint8_t*record_addr){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int item_size = max_uintptr(write_size,sizeof(uint32_t));
  const uintptr_t meta_size = LFTL_META_N_ITEMS * item_size;
  const uint8_t*const meta_addr = record_addr + ewlf_magic_phy_size(ctx) + ewlf_prev_erased_phy_size(ctx);
  uint32_t buf[LFTL_DIV_CEIL(meta_size,sizeof(uint32_t))];
  nvm_read(ctx,ctx->nvm_props,buf,meta_addr,meta_size);
  for(unsigned int i = 0; i < LFTL_META_N_ITEMS; i++){
    dst->items[i] = buf[i*item_size/sizeof(uint32_t)];
  }
}

/**
 * \brief Read the version field of an EWLF record's meta-data.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static uint32_t ewlf_get_record_version(lftl_ctx_t*ctx, const uint8_t*record_addr){
  lftl_meta_t meta;
  ewlf_get_record_meta(ctx,&meta,record_addr);
  return meta.version;
}

/**
 * \brief Write the version/checksum/checksum2 meta-data of an EWLF record.
 *
 * Writes everything but checksum2 first, then checksum2 last, mirroring
 * ::write_meta_core's tearing-safety property.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param meta The meta-data to write
 */
static void ewlf_write_record_meta_core(lftl_ctx_t*ctx, uint8_t*record_addr, lftl_meta_t*meta){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int item_size = max_uintptr(write_size,sizeof(uint32_t));
  const uintptr_t meta_size = LFTL_META_N_ITEMS * item_size;
  uint8_t*const meta_addr = record_addr + ewlf_magic_phy_size(ctx) + ewlf_prev_erased_phy_size(ctx);
  uint32_t buf[LFTL_DIV_CEIL(meta_size,sizeof(uint32_t))];
  memset(buf,0,sizeof(buf));
  for(unsigned int i = 0; i < LFTL_META_N_ITEMS; i++){
    buf[i*item_size/sizeof(uint32_t)] = meta->items[i];
  }
  //write everything but checksum2
  nvm_write(ctx,ctx->nvm_props,meta_addr,buf,meta_size - item_size);
  //write checksum2
  const uintptr_t checksum2_offset = meta_size - item_size;
  uint8_t*const checksum2_addr = meta_addr + checksum2_offset;
  uint8_t*const checksum2_src = (uint8_t*)buf + checksum2_offset;
  nvm_write(ctx,ctx->nvm_props,checksum2_addr,checksum2_src,item_size);
}

/**
 * \brief Write an EWLF record's magic field.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static void ewlf_write_record_magic(lftl_ctx_t*ctx, uint8_t*record_addr){
  const uintptr_t size = ewlf_magic_phy_size(ctx);
  uint8_t buf[size];
  memset(buf,0,size);
  *(uint64_t*)buf = EWLF_MAGIC;
  nvm_write(ctx,ctx->nvm_props,record_addr,buf,size);
}

/**
 * \brief Check whether an EWLF record's `prev_erased` flag shows the confirmed pattern.
 *
 * The flag starts in its natural erased (all-1s) state and is only ever
 * written once, to a confirmed pattern with at least one 0 bit, right
 * after successfully erasing the previous erase unit at a rollover --
 * this is what lets it be written without ever needing to flip a bit from
 * 0 back to 1.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \returns true if the flag shows the confirmed (all-zero) pattern
 */
static bool ewlf_record_prev_erased_confirmed(lftl_ctx_t*ctx, const uint8_t*record_addr){
  const uintptr_t size = ewlf_prev_erased_phy_size(ctx);
  uint8_t buf[size];
  const uint8_t*const flag_addr = record_addr + ewlf_magic_phy_size(ctx);
  nvm_read(ctx,ctx->nvm_props,buf, flag_addr, size);
  return buf[0] == 0;
}

/**
 * \brief Write the confirmed pattern into an EWLF record's `prev_erased` flag.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static void ewlf_write_record_prev_erased(lftl_ctx_t*ctx, uint8_t*record_addr){
  const uintptr_t size = ewlf_prev_erased_phy_size(ctx);
  uint8_t buf[size];
  memset(buf,0,size);
  uint8_t*const flag_addr = record_addr + ewlf_magic_phy_size(ctx);
  nvm_write(ctx,ctx->nvm_props,flag_addr, buf, size);
}

/**
 * \brief Check whether an EWLF record's magic and checksum are valid.
 *
 * checksum2 is not part of the check: as for a slot (see ::find_current_slot),
 * a record whose data and checksum are valid is taken whatever its checksum2,
 * and the caller copies it forward when checksum2 reads erased. A tearing
 * while writing the record's meta-data leaves checksum2 erased or partially
 * programmed, and the checksum possibly weakly programmed; rejecting such a
 * record would make the outcome depend on how a weakly programmed word reads
 * at each boot (ISSUES.md LFTL-008).
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param meta Output: the record's meta-data, valid only if true is returned
 * \returns true if the magic matches and the checksum matches the data and
 *   the version
 */
static bool ewlf_record_is_valid(lftl_ctx_t*ctx, const uint8_t*record_addr, lftl_meta_t*meta){
  uint64_t magic;
  nvm_read(ctx,ctx->nvm_props,&magic,record_addr,sizeof(magic));
  if(magic != EWLF_MAGIC) return false;
  ewlf_get_record_meta(ctx,meta,record_addr);
  const uint32_t recomputed = checksum(ctx,record_addr+ewlf_header_phy_size(ctx),ctx->data_size) + meta->version;
  return recomputed == meta->checksum;
}

/**
 * \brief Newest valid record of an EWLF erase unit, if any.
 *
 * Records are written from the top of a unit down, below a record whose
 * magic was complete (a copy forward goes below a record whose checksum may
 * be weak, never one without a magic), and a unit is only ever erased whole,
 * so a unit whose top record has no valid magic holds no record at all: an
 * erased unit costs one read.
 * Otherwise the positions are checked from the lowest address up, and the
 * first valid record (see ::ewlf_record_is_valid) is the newest.
 *
 * \param ctx Context of the EWLF area
 * \param unit_index Index of the erase unit, in `[0, n_pages(ctx))`
 * \param record_addr Output: physical address of the record found
 * \param meta Output: its meta-data, valid only if true is returned
 * \returns true if the unit holds a valid record
 */
static bool ewlf_newest_in_unit(lftl_ctx_t*ctx, unsigned int unit_index, const uint8_t**record_addr, lftl_meta_t*meta){
  const uintptr_t record_size = ewlf_record_phy_size(ctx);
  const unsigned int records_per_unit = page_size(ctx) / record_size;
  const uint8_t*const top = ewlf_unit_top_record(ctx,unit_index);
  uint64_t magic;
  nvm_read(ctx,ctx->nvm_props,&magic,top,sizeof(magic));
  if(magic != EWLF_MAGIC) return false;
  for(unsigned int i = records_per_unit; i > 0; i--){
    const uint8_t*const addr = top - (i-1)*record_size;
    if(ewlf_record_is_valid(ctx,addr,meta)){
      *record_addr = addr;
      return true;
    }
  }
  return false;
}

static void copy_forward_ewlf(lftl_ctx_t*ctx);

/**
 * \brief Whether an EWLF record's checksum2 write unit reads all erased.
 *
 * Tells a record torn during its checksum word (checksum2 untouched, the
 * checksum possibly weakly programmed) from one torn during checksum2
 * itself (some of its bits programmed, the checksum complete), as
 * ::get_slot_checksum2 does for a slot.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \returns true if every byte of the checksum2 write unit reads 0xFF
 */
static bool ewlf_record_checksum2_erased(lftl_ctx_t*ctx, const uint8_t*record_addr){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  const uint8_t*const addr = record_addr + ewlf_magic_phy_size(ctx) + ewlf_prev_erased_phy_size(ctx) + 2*item_size;
  return is_erased(ctx,addr,item_size);
}

/**
 * \brief Locate the current EWLF record and update `ctx->data`.
 *
 * Records only ever start at the top of an erase unit minus a multiple of
 * the record size, so only those positions are checked: data can never be
 * mistaken for a header, and every candidate fits in its erase unit. The
 * units are checked by increasing address, from the area's base address,
 * and the newest record of the first unit holding one (see
 * ::ewlf_newest_in_unit) is the newest of the area, except after a wrap
 * from the lowest unit back to the highest one, interrupted before the
 * lowest unit was erased: the newest record is then in the highest unit,
 * so the newest record of the next unit in write order is checked too, and
 * taken if its version is higher. That unit's newest record, not only its
 * top one: a copy forward (below) can leave two records there. Unlike
 * ::find_current_slot, there is no equivalent of
 * ::LFTL_ERROR_VERSION_COLLISION here: by construction there is nothing to
 * collide with.
 *
 * If the record's checksum2 does not match its checksum, a tearing happened
 * while its meta-data was written. When checksum2 reads all erased, the cut
 * came during the checksum word at the latest, which may be weakly
 * programmed: the record is copied forward first, with complete meta-data,
 * so that nothing depends on how that word reads later (see
 * ::copy_forward_ewlf). Nothing is erased before that copy is complete. When
 * checksum2 is partially programmed, its own program had started, so the
 * checksum is complete and the record is kept as it is.
 *
 * The top record of the unit holding the current record started that unit
 * with a rollover: if its `prev_erased` flag is not confirmed, the previous
 * erase unit may not have been erased, or only "weakly": it is erased
 * again and the flag confirmed. The flag is read on its own, so this works
 * when the top record is the torn one and the current record its copy.
 *
 * \param ctx Context of the EWLF area
 */
static void find_current_record(lftl_ctx_t*ctx){
  const unsigned int n_units = n_pages(ctx);
  for(unsigned int unit_index = 0; unit_index < n_units; unit_index++){
    const uint8_t*record_addr;
    lftl_meta_t meta;
    if(!ewlf_newest_in_unit(ctx,unit_index,&record_addr,&meta)) continue;
    //a newer record can only be in the next unit in write order: the other
    //units that come later in write order are at lower addresses, so their
    //records would have been found first
    unsigned int record_unit = unit_index;
    const unsigned int next_unit = (unit_index + n_units - 1) % n_units;
    const uint8_t*next_addr;
    lftl_meta_t next_meta;
    if(ewlf_newest_in_unit(ctx,next_unit,&next_addr,&next_meta) && (next_meta.version > meta.version)){
      record_addr = next_addr;
      record_unit = next_unit;
      meta = next_meta;
    }
    ctx->data = (void*)(record_addr + ewlf_header_phy_size(ctx));
    if((meta.checksum2 != meta.checksum) && ewlf_record_checksum2_erased(ctx,record_addr)){
      copy_forward_ewlf(ctx);
      record_unit = ((uintptr_t)((uint8_t*)ctx->data - ewlf_header_phy_size(ctx) - (uint8_t*)ctx->area)) / page_size(ctx);
    }
    uint8_t*const top = ewlf_unit_top_record(ctx,record_unit);
    if(!ewlf_record_prev_erased_confirmed(ctx,top)){
      const unsigned int prev_unit = (record_unit + 1) % n_units;
      nvm_erase(ctx,ctx->nvm_props,ewlf_unit_base(ctx,prev_unit), 1);
      ewlf_write_record_prev_erased(ctx,top);
    }
    return;
  }
  raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
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
  check_in_data(ctx, nvm_addr, size);
  if(LFTL_INVALID_POINTER == ctx->data) find_current_slot(ctx);
  const uintptr_t offset = (uintptr_t)nvm_addr - (uintptr_t)ctx->area;
  return (void*)((uintptr_t)ctx->data + offset);
}
/**
 * \brief Translate a logical address into a physical one, for an EWLF area.
 *
 * Finds the current record first if `ctx->data` isn't known yet. The
 * logical address space is the same shape as for std areas -- `ctx->area`
 * to `ctx->area+ctx->data_size` represents one record's data -- only how
 * `ctx->data` is located differs (::find_current_record vs
 * ::find_current_slot).
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address to translate
 * \param size Size in bytes of the access, used to validate the range
 * \returns The corresponding physical address within the current record's data
 */
static void*translate_addr_ewlf(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  check_in_data(ctx, nvm_addr, size);
  if(LFTL_INVALID_POINTER == ctx->data) find_current_record(ctx);
  const uintptr_t offset = (uintptr_t)nvm_addr - (uintptr_t)ctx->area;
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
      nvm_write(dst_ctx,dst_ctx->nvm_props,dst_base, current_base, offset);
    }
  }
  if(addr_misalignement){
    // fix up first WU
    uintptr_t size_consumed = write_size - addr_misalignement;
    //Note: with the "on/linux" test harness's write_size of 2, the only
    //possible addr_misalignement is 1, making size_consumed always exactly
    //1 -- this branch (size_consumed > size) would need size==0 to fire,
    //which is a separate, already-covered early-return case elsewhere. A
    //target built with a larger write unit size (e.g. 16) would be needed
    //to exercise this with a real unaligned sub-write-unit write.
    if(size_consumed > size){
      const uintptr_t tail_size = size_consumed - size;
      size_consumed = size;
      nvm_read(dst_ctx,dst_ctx->nvm_props,((uint8_t*)&wu) + addr_misalignement + size, current_base+offset+ addr_misalignement+size, tail_size);
    }
    nvm_read(dst_ctx,dst_ctx->nvm_props,&wu, current_base+offset, addr_misalignement);
    mem_read(src_ctx,((uint8_t*)&wu) + addr_misalignement, src_phy_addr , size_consumed);
    nvm_write(dst_ctx,dst_ctx->nvm_props,dst_phy_addr,&wu,write_size);
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
  nvm_write(dst_ctx,dst_ctx->nvm_props,dst_phy_addr,src_phy_addr,size_aligned);
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
    nvm_read(dst_ctx,dst_ctx->nvm_props,wu_part2_base, wu_part2_src, wu_part2_size);
    nvm_write(dst_ctx,dst_ctx->nvm_props,dst_base+last_wu_offset, &wu, write_size);
  }
  if(!transaction){
    //up to the data rounded up to whole write units, where end_offset may already be
    const uintptr_t remaining = data_phy_size(dst_ctx) - end_offset;
    if(remaining){
      nvm_write(dst_ctx,dst_ctx->nvm_props,dst_base+end_offset, current_base + end_offset, remaining);
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
  //check the range as requested: once extended to whole write units below, size could wrap around
  check_in_data(dst_ctx, dst_nvm_addr, size);
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
 * \brief Shared core for ::write_ewlf, ::erase_ewlf and ::copy_forward_ewlf.
 *
 * Places the new record below the current one in its own erase unit, at
 * the first position whose cells all read erased (a tearing may have left
 * a partial record right below the current one, and some flash refuses to
 * program cells that are not erased), or else rolls over to the top of
 * the unit immediately below, wrapping from the lowest unit back to the
 * highest, after erasing that unit if needed: it holds no record newer
 * than the current one. Then writes the record's data (unless
 * `write_data` is false, in which case the fresh, already-erased target
 * data is left untouched so it reads back as erased), then magic +
 * version + checksum, then checksum2. The checksum is computed from the
 * data meant to be written rather than from what reads back, so a record
 * that did not program as intended fails its check. On a rollover,
 * additionally erases the just-vacated unit and confirms the new record's
 * `prev_erased` flag.
 *
 * \param dst_ctx Context of the target EWLF area
 * \param dst_nvm_addr Destination logical address (ignored if `write_data` is false)
 * \param src Source address (ignored if `write_data` is false)
 * \param size Size in bytes to write (ignored if `write_data` is false;
 *   the whole record's data is always (re)written as a unit)
 * \param write_data Whether to actually write new data, or leave the
 *   fresh target data in its erased state
 * \param copy_current Whether the source is the current record's own data
 *   (`src` is then ignored): its physical address, read as any NVM source,
 *   which ::check_src_phy_addr could take for a logical one
 */
static void write_ewlf_core(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool write_data, bool copy_current){
  DEBUG_PRINTLN("write_ewlf_core(%p,%p,%p,%u,%u,%u) entry",dst_ctx,dst_nvm_addr,src,size,write_data,copy_current);
  //check the range as requested: once extended to whole write units below, size could wrap around
  if(write_data) check_in_data(dst_ctx, dst_nvm_addr, size);
  const uint32_t write_size = dst_ctx->nvm_props->write_size;
  const uintptr_t addr_misalignement = write_data ? ((uintptr_t)dst_nvm_addr % write_size) : 0;
  const uintptr_t dst_nvm_addr_aligned = (uintptr_t)dst_nvm_addr - addr_misalignement;
  const uint32_t size_misalignement = write_data ? (size % write_size) : 0;
  const bool aligned = (0 == addr_misalignement) && (0 == size_misalignement);
  uintptr_t size_aligned = write_data ? (size + addr_misalignement) : dst_ctx->data_size;
  if(write_data && !aligned){
    // unaligned: extend the start address and end address to start of WU and end of WU respectively
    if(0 != (size_aligned % write_size)){
      size_aligned += write_size - size_aligned % write_size;
    }
  }
  //locate the current record: the requested range is checked above, and once
  //extended to whole write units it may end in the padding after data_size,
  //which every record has room for (see data_phy_size)
  const void*const current_data_addr = translate_addr_ewlf(dst_ctx, (void*)dst_nvm_addr_aligned, 0);
  const uintptr_t offset = (uintptr_t)current_data_addr - (uintptr_t)dst_ctx->data;
  const uintptr_t header_size = ewlf_header_phy_size(dst_ctx);
  const uint8_t*const current_record_addr = (uint8_t*)dst_ctx->data - header_size;
  const unsigned int current_unit_index = (uintptr_t)(current_record_addr - (uint8_t*)dst_ctx->area) / page_size(dst_ctx);

  const uintptr_t record_size = ewlf_record_phy_size(dst_ctx);
  //room below the current record in its unit: the first position whose cells all read erased
  const uintptr_t unit_base = (uintptr_t)ewlf_unit_base(dst_ctx,current_unit_index);
  uintptr_t candidate_addr = (uintptr_t)current_record_addr;
  bool found = false;
  while(!found && (candidate_addr - unit_base >= record_size)){
    candidate_addr -= record_size;
    found = is_erased(dst_ctx,(const uint8_t*)candidate_addr,record_size);
  }
  bool rollover;
  unsigned int new_unit_index;
  uint8_t*new_record_addr;
  if(found){
    rollover = false;
    new_unit_index = current_unit_index;
    new_record_addr = (uint8_t*)candidate_addr;
  } else {
    // no room left: move to the unit immediately below, wrapping from
    // the lowest unit back to the highest one; erase it first if a
    // tearing left stale or partial records in it
    rollover = true;
    new_unit_index = (current_unit_index==0) ? (n_pages(dst_ctx)-1) : (current_unit_index-1);
    uint8_t*const new_unit_base = ewlf_unit_base(dst_ctx,new_unit_index);
    if(!is_erased(dst_ctx,new_unit_base,page_size(dst_ctx))){
      nvm_erase(dst_ctx,dst_ctx->nvm_props,new_unit_base,1);
    }
    new_record_addr = ewlf_unit_top_record(dst_ctx,new_unit_index);
  }
  uint8_t*const new_data_addr = new_record_addr + header_size;

  const uint8_t* src_phy_addr = src;
  lftl_ctx_t* src_ctx = dst_ctx;
  if(write_data){
    if(copy_current){
      src_phy_addr = (const uint8_t*)dst_ctx->data;
    } else {
      check_src_phy_addr(&src_ctx,&src_phy_addr,size);
    }
    write_src_phy_addr_to_dst_phy_addr(
      dst_ctx,
      offset,
      size,
      new_data_addr,
      (const uint8_t*)dst_ctx->data,
      src_ctx,
      src_phy_addr,
      write_size,
      addr_misalignement,
      dst_nvm_addr_aligned,
      size_misalignement,
      size_aligned,
      0 // no transaction
    );
  }
  //write magic, then version+checksum (checksum of the data meant to be
  //written: the current data with the written range replaced, or, if
  //write_data is false, erased data)
  ewlf_write_record_magic(dst_ctx,new_record_addr);
  const uint32_t version = 1 + ewlf_get_record_version(dst_ctx, current_record_addr);
  uint32_t sum = 0xFFFFFFFF;
  if(write_data){
    const uintptr_t first = (uintptr_t)dst_nvm_addr - (uintptr_t)dst_ctx->area;
    const uint8_t*const current_data = (const uint8_t*)dst_ctx->data;
    sum = checksum_update(dst_ctx,sum,current_data,first);
    sum = checksum_update(src_ctx,sum,src_phy_addr,size);
    sum = checksum_update(dst_ctx,sum,current_data+first+size,dst_ctx->data_size-first-size);
  } else {
    sum = checksum_update_erased(sum,dst_ctx->data_size);
  }
  lftl_meta_t meta;
  meta.version = version;
  meta.checksum = sum + meta.version;
  meta.checksum2 = meta.checksum;
  ewlf_write_record_meta_core(dst_ctx,new_record_addr,&meta);

  if(rollover){
    //the just-vacated unit is now fully superseded: erase it, then
    //confirm this new record's prev_erased flag
    nvm_erase(dst_ctx,dst_ctx->nvm_props,ewlf_unit_base(dst_ctx,current_unit_index), 1);
    ewlf_write_record_prev_erased(dst_ctx,new_record_addr);
  }

  dst_ctx->data = new_data_addr;
  DEBUG_PRINTLN("write_ewlf_core exit");
}

/**
 * \brief Write to an EWLF area.
 *
 * EWLF areas never support transactions, so every write is immediate.
 *
 * \param dst_ctx Context of the target EWLF area
 * \param dst_nvm_addr Destination logical address
 * \param src Source address
 * \param size Size in bytes to write
 */
static void write_ewlf(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  write_ewlf_core(dst_ctx,dst_nvm_addr,src,size,true,false);
}

/**
 * \brief Copy the current EWLF record forward, with complete meta-data.
 *
 * Used by ::find_current_record when the current record's checksum2 reads
 * erased: a tearing happened while that record's meta-data was written, so
 * its checksum may be weakly programmed and read differently later. This
 * takes the normal write path: the same data, read from the record itself,
 * at the next free position, with the next version. With at least 2
 * records per erase unit (see ::check_config), a copy of a unit's top
 * record stays in that unit, so nothing is erased before it is complete.
 * A regular area cannot do the same, since a copy there needs another slot
 * erased first: it erases the torn slot instead (see ::find_current_slot).
 *
 * \param ctx Context of the EWLF area, with `ctx->data` set to the record to copy
 */
static void copy_forward_ewlf(lftl_ctx_t*ctx){
  write_ewlf_core(ctx,ctx->area,LFTL_INVALID_POINTER,ctx->data_size,true,true);
}

/**
 * \brief Logically erase an EWLF area's data.
 *
 * Writes a fresh record whose data is left untouched in its natural
 * erased (0xFF) state, so it reads back as erased, without needing a
 * synthetic all-0xFF source buffer regardless of `data_size`.
 *
 * \param ctx Context of the target EWLF area
 */
static void erase_ewlf(lftl_ctx_t*ctx){
  write_ewlf_core(ctx,ctx->area,LFTL_INVALID_POINTER,0,false,false);
}

/**
 * \brief Format an EWLF area.
 *
 * Erases every erase unit, then writes the first record (version 1) at
 * the top of the highest unit, per "writes start at the highest possible
 * address". Its `prev_erased` flag is written as confirmed immediately:
 * every unit was just freshly erased, so there is nothing to repair.
 *
 * \param ctx Context of the target EWLF area
 */
static void format_ewlf(lftl_ctx_t*ctx){
  DEBUG_PRINTLN("format_ewlf entry");
  nvm_erase(ctx,ctx->nvm_props,ctx->area, n_pages(ctx));
  uint8_t*const record_addr = ewlf_unit_top_record(ctx, n_pages(ctx)-1);
  uint8_t*const data_addr = record_addr + ewlf_header_phy_size(ctx);
  ewlf_write_record_magic(ctx,record_addr);
  lftl_meta_t meta;
  meta.version = 1;
  meta.checksum = checksum(ctx,data_addr,ctx->data_size) + meta.version;
  meta.checksum2 = meta.checksum;
  ewlf_write_record_meta_core(ctx,record_addr,&meta);
  ewlf_write_record_prev_erased(ctx,record_addr);
  ctx->data = data_addr;
  DEBUG_PRINTLN("format_ewlf exit");
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
//Note: erase()'s only caller is lftl_erase_all, which always passes the
//full, page-aligned area range (offset==0 and remaining==0 always hold).
//The misalignment checks below and the offset/remaining copy branches are
//therefore unreachable via the current public API; erase() keeps them
//because it is written as a general sub-range primitive, ready for a future
//caller that erases a narrower range.
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
    nvm_write(ctx,ctx->nvm_props,base, current_base, offset);
  }

  const uintptr_t end_offset = offset+size;
  const uintptr_t remaining = ctx->data_size - end_offset;
  if(remaining){
    nvm_write(ctx,ctx->nvm_props,base+end_offset, current_base + end_offset, remaining);
  }
  //increment version and write new meta data in next slot
  const uint32_t version = 1 + get_slot_version(ctx, get_current_slot_index(ctx));
  write_meta(ctx, index, version);
  //update context
  ctx->data = base;
  DEBUG_PRINTLN("erase exit");
}

/**
 * \brief Check an area's configuration, raise an error if it is bad.
 *
 * Called when an area is registered, before it is linked, so that a bad
 * configuration fails there instead of corrupting memory or flash later.
 * Raises ::LFTL_ERROR_WU_SIZE_TOO_LARGE if the write unit is larger than
 * ::LFTL_WU_MAX_SIZE (which bounds stack buffers), ::LFTL_ERROR_BAD_CONFIG if:
 * - the write unit is not a power of 2 dividing the erase unit,
 * - the area is not whole erase units within its NVM,
 * - a regular area holds less than 2 slots, or its data is not whole write
 *   units (::lftl_transaction_commit copies whole write units only),
 * - an EWLF area has less than 2 erase units, or less than 2 records fit in
 *   one (::copy_forward_ewlf must be able to copy a unit's top record within
 *   that unit).
 *
 * \param ctx Context of the LFTL area, with `area_type` already set
 */
static void check_config(lftl_ctx_t*ctx){
  const lftl_nvm_props_t*const nvm_props = ctx->nvm_props;
  const uint32_t write_size = nvm_props->write_size;
  const uint32_t erase_size = nvm_props->erase_size;
  if(write_size > LFTL_WU_MAX_SIZE) raise_error(ctx,LFTL_ERROR_WU_SIZE_TOO_LARGE);
  if((0 == write_size) || (0 != (write_size & (write_size-1)))) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  if((0 == erase_size) || (0 != (erase_size % write_size))) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  const uintptr_t area = (uintptr_t)ctx->area;
  const uintptr_t base = (uintptr_t)nvm_props->base;
  if((area < base) || (area - base > nvm_props->size) || (ctx->area_size > nvm_props->size - (area - base))) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  if((0 != (area - base) % erase_size) || (0 != ctx->area_size % erase_size)) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  if(lftl_is_ewlf(ctx)){
    if(n_pages(ctx) < 2) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
    if(erase_size / ewlf_record_phy_size(ctx) < 2) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  } else {
    if(n_slots(ctx) < 2) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
    if(0 != (ctx->data_size % write_size)) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  }
}

#define xstr(s) str(s)
#define str(s) #s
static const char*version = xstr(GIT_VERSION);
static const uint64_t version_timestamp = VERSION_TIMESTAMP;
static const char*build_type = xstr(BUILD_TYPE);
