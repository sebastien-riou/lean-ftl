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

/// How a write unit reads, see ::read_unit_state.
typedef enum {
  UNIT_ERASED, /**< every byte reads 0xFF */
  UNIT_ZERO,   /**< every byte reads 0x00 */
  UNIT_MIXED,  /**< anything else: a value, or a marker a power cut left partially programmed */
} unit_state_t;

/**
 * \brief Classify the content of a write unit already read into memory.
 *
 * \param unit The unit's bytes
 * \param size Size in bytes of the unit
 */
static unit_state_t unit_state_of(const uint8_t*unit, uintptr_t size){
  bool erased = true;
  bool zero = true;
  for(uintptr_t i=0;i<size;i++){
    if(0xFF != unit[i]) erased = false;
    if(0x00 != unit[i]) zero = false;
  }
  return erased ? UNIT_ERASED : (zero ? UNIT_ZERO : UNIT_MIXED);
}

/**
 * \brief Read a write unit once and classify it.
 *
 * One read: a word a power cut left weak can read differently from one read
 * to the next, so every decision about it is made on the same reading.
 *
 * \param ctx Context of the LFTL area
 * \param addr Physical address of the unit
 * \param size Size in bytes of the unit, at most ::LFTL_WU_MAX_SIZE
 */
static unit_state_t read_unit_state(lftl_ctx_t*ctx, const uint8_t*addr, uintptr_t size){
  uint64_t buf[SIZE64(LFTL_WU_MAX_SIZE)];
  nvm_read(ctx,ctx->nvm_props,buf,addr,size);
  return unit_state_of((const uint8_t*)buf,size);
}

/**
 * \brief Program a write unit to zeros.
 *
 * On an erased unit this is an ordinary program. On a unit that is not
 * erased it is the one program a flash declared with `zero_overwrite`
 * accepts (N3 of the flash page): every cell gets a full pulse, so the unit
 * reads zeros for good. The scans use it to pin a decision about a word a
 * power cut left weak, never on a unit that already reads all zeros (the
 * STM32L5 and STM32U5 refuse that program).
 *
 * \param ctx Context of the LFTL area
 * \param addr Physical address of the unit
 * \param size Size in bytes of the unit, at most ::LFTL_WU_MAX_SIZE
 */
static void pin_unit(lftl_ctx_t*ctx, uint8_t*addr, uintptr_t size){
  uint64_t zeros[SIZE64(LFTL_WU_MAX_SIZE)];
  memset(zeros,0,sizeof(zeros));
  nvm_write(ctx,ctx->nvm_props,addr,zeros,size);
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
 * whole write unit reads all erased or all zeros.
 *
 * The bytes next to the value are programmed to zero with it, so the whole
 * unit tells a checksum2 that was never programmed from one whose program
 * was torn, and one that ::find_current_slot pinned, see there. One read:
 * a word a power cut left weak can read differently from one read to the
 * next, so every decision about it is made on the same reading.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \param all_erased Output: true if every byte of the write unit reads 0xFF
 * \param all_zero Output: true if every byte of the write unit reads 0x00
 * \returns The slot's stored checksum2
 */
static uint32_t get_slot_checksum2(lftl_ctx_t*ctx, unsigned int slot_index, bool*all_erased, bool*all_zero){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  uint32_t unit[LFTL_DIV_CEIL(item_size,sizeof(uint32_t))];
  get_slot_item(ctx,slot_index,2,unit);
  const uint8_t*const unit8 = (const uint8_t*)unit;
  *all_erased = true;
  *all_zero = true;
  for(unsigned int i=0;i<item_size;i++){
    if(0xFF != unit8[i]) *all_erased = false;
    if(0x00 != unit8[i]) *all_zero = false;
  }
  return unit[0];
}

/**
 * \brief Program one meta-data item's whole write unit to zeros.
 *
 * The one program a flash declared with `zero_overwrite` accepts on a write
 * unit in any state (N3 of the flash page): every cell gets a full pulse, so
 * the unit reads zeros for good. ::find_current_slot uses it to pin a
 * decision about a word a power cut left weak, and never on a unit that
 * already reads all zeros (the STM32L5 and STM32U5 refuse that program).
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \param item Index of the item: 0 version, 1 checksum, 2 checksum2
 */
static void pin_slot_item(lftl_ctx_t*ctx, unsigned int slot_index, unsigned int item){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  pin_unit(ctx, slot_base(ctx, slot_index) + meta_offset(ctx) + item*item_size, item_size);
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
 * Used by ::find_current_slot before it sets a torn slot aside, so that the
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
 * all erased (the cut came during the checksum word at the latest: that word
 * may be weakly programmed, and read differently at a later boot) or
 * partially programmed (its own program had started, so the checksum is
 * complete). Only the whole write unit tells the two apart
 * (::get_slot_checksum2), and the scan never erases, so that a power cut at
 * every boot costs no erase (ISSUES.md LFTL-010):
 * - partially programmed: the slot is taken. On a flash with
 *   `zero_overwrite` the unit is pinned to zeros first, so it can never read
 *   all erased at a later boot and bring the previous version back
 *   (LFTL-011); a unit reading all zeros is such a pinned one, or a
 *   checksum that is legitimately 0, and is taken as complete.
 * - all erased, another slot valid: the previous version is current and the
 *   scan runs again without the torn slot. With `zero_overwrite` the torn
 *   slot is tombstoned first, its version item pinned to zeros, so that its
 *   checksum can never verify again; without it the slot is left as it is,
 *   and the next write erases it anyway, as every write erases its target.
 *   The torn slot is never copied, so the previous version is never erased
 *   on the strength of a word that may read differently later (LFTL-009).
 * - all erased, no other slot valid: the slot is taken as it is. Nothing in
 *   normal operation produces that state, and taking it beats having nothing.
 *
 * \param ctx Context of the LFTL area
 */
static void find_current_slot(lftl_ctx_t*ctx){
  const unsigned int ns = n_slots(ctx);
  const uint32_t invalid_index = 0xFFFFFFFF;
  uint32_t skip_index = invalid_index;
  //a second pass at most: it leaves out the slot the first one found torn
  for(unsigned int pass = 0; pass < 2; pass++){
    uint32_t max_version_index = invalid_index;
    uint32_t max_version = 0;
    for(unsigned int i=0;i<ns;i++){
      if(i == skip_index) continue;
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
    bool checksum2_zero;
    const uint32_t checksum2 = get_slot_checksum2(ctx, max_version_index, &checksum2_erased, &checksum2_zero);
    const bool complete = checksum2 == get_slot_checksum(ctx, max_version_index);
    if(complete || checksum2_zero){
      //complete, or pinned by an earlier boot
      ctx->data = slot_base(ctx, max_version_index);
      return;
    }
    if(!checksum2_erased){
      //partially programmed: the checksum is complete, the slot is stable.
      //Pin the word so that it cannot read all erased at a later boot
      if(ctx->nvm_props->zero_overwrite) pin_slot_item(ctx, max_version_index, 2);
      ctx->data = slot_base(ctx, max_version_index);
      return;
    }
    if(!other_valid_slot(ctx, max_version_index)){
      ctx->data = slot_base(ctx, max_version_index);
      return;
    }
    //all erased, torn during the checksum word at the latest: the previous
    //version is current. Tombstone the torn slot where the flash allows it.
    //ctx->data stays unset until the second pass, so an error here leaves
    //nothing to trust
    if(ctx->nvm_props->zero_overwrite) pin_slot_item(ctx, max_version_index, 0);
    skip_index = max_version_index;
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
 * \brief Physical size in bytes of an EWLF record's start marker.
 *
 * One write unit, right after the magic. It is the first word of a record
 * to be programmed, to all zeros: a position is started once its start
 * marker has a bit at 0, and nothing after an erased start marker was ever
 * programmed (see ::write_ewlf_core).
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_start_marker_phy_size(lftl_ctx_t*ctx){
  return max_uintptr(ctx->nvm_props->write_size, sizeof(uint8_t));
}

/**
 * \brief Physical size in bytes of an EWLF record's whole header
 * (magic + start marker + version/checksum/end marker).
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_header_phy_size(lftl_ctx_t*ctx){
  return ewlf_magic_phy_size(ctx) + ewlf_start_marker_phy_size(ctx) + meta_phy_size(ctx);
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
 * \brief Physical address of an EWLF record's start marker.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record (its magic field)
 */
static uint8_t* ewlf_start_marker_addr(lftl_ctx_t*ctx, const uint8_t*record_addr){
  return (uint8_t*)record_addr + ewlf_magic_phy_size(ctx);
}

/**
 * \brief Physical address of one of an EWLF record's meta-data items.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param item Index of the item: 0 version, 1 checksum, 2 end marker
 */
static uint8_t* ewlf_item_addr(lftl_ctx_t*ctx, const uint8_t*record_addr, unsigned int item){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  return (uint8_t*)record_addr + ewlf_magic_phy_size(ctx) + ewlf_start_marker_phy_size(ctx) + item*item_size;
}

/**
 * \brief How an EWLF record's start marker reads: erased (the position is
 * free), zeros (started) or partially programmed (started, by a write a
 * power cut interrupted during that very word).
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static unit_state_t ewlf_start_marker_state(lftl_ctx_t*ctx, const uint8_t*record_addr){
  return read_unit_state(ctx,ewlf_start_marker_addr(ctx,record_addr),ewlf_start_marker_phy_size(ctx));
}

/**
 * \brief Program an EWLF record's start marker to zeros.
 *
 * The first write of a record, and also what the write path does to the
 * start marker of a started position it skips, on a flash with
 * `zero_overwrite`, so that a weak start marker cannot later read erased
 * and open a hole in the run of started positions.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static void ewlf_write_start_marker(lftl_ctx_t*ctx, uint8_t*record_addr){
  pin_unit(ctx,ewlf_start_marker_addr(ctx,record_addr),ewlf_start_marker_phy_size(ctx));
}

/**
 * \brief Program one of an EWLF record's meta-data items to zeros.
 *
 * The end marker (item 2) is written this way at the end of every record
 * write; the scan also pins a partially programmed end marker with it, and
 * tombstones a torn record by pinning its version (item 0): its checksum can
 * then never verify again (a version is never 0: format writes 1).
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param item Index of the item: 0 version, 1 checksum, 2 end marker
 */
static void ewlf_pin_item(lftl_ctx_t*ctx, uint8_t*record_addr, unsigned int item){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  pin_unit(ctx,ewlf_item_addr(ctx,record_addr,item),item_size);
}

/**
 * \brief Read the version and checksum of an EWLF record, and how its end
 * marker reads.
 *
 * One read of the three items: the end marker may be weak, and every
 * decision about it is made on the same reading.
 *
 * \param ctx Context of the EWLF area
 * \param dst Destination of the meta-data, in volatile memory (`checksum2` is 0)
 * \param end_marker Output: how the end marker's write unit reads
 * \param record_addr Physical address of the start of the record (its magic field)
 */
static void ewlf_get_record_meta_ex(lftl_ctx_t*ctx, lftl_meta_t*dst, unit_state_t*end_marker, const uint8_t*record_addr){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int item_size = max_uintptr(write_size,sizeof(uint32_t));
  const uintptr_t meta_size = LFTL_META_N_ITEMS * item_size;
  const uint8_t*const meta_addr = ewlf_item_addr(ctx,record_addr,0);
  uint32_t buf[LFTL_DIV_CEIL(meta_size,sizeof(uint32_t))];
  nvm_read(ctx,ctx->nvm_props,buf,meta_addr,meta_size);
  dst->version = buf[0];
  dst->checksum = buf[item_size/sizeof(uint32_t)];
  dst->checksum2 = 0;
  *end_marker = unit_state_of(((const uint8_t*)buf) + 2*item_size,item_size);
}

/**
 * \brief Read the version and checksum of an EWLF record.
 *
 * \param ctx Context of the EWLF area
 * \param dst Destination, in volatile memory
 * \param record_addr Physical address of the start of the record
 */
static void ewlf_get_record_meta(lftl_ctx_t*ctx, lftl_meta_t*dst, const uint8_t*record_addr){
  unit_state_t end_marker;
  ewlf_get_record_meta_ex(ctx,dst,&end_marker,record_addr);
}

/**
 * \brief Write the version, the checksum and the end marker of an EWLF record.
 *
 * Version and checksum first, in one write, then the end marker, an all-zero
 * write unit, last: a complete write leaves it solid, and a write cut before
 * it leaves it all erased (see ::find_current_record).
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param meta The version and checksum to write
 */
static void ewlf_write_record_meta_core(lftl_ctx_t*ctx, uint8_t*record_addr, const lftl_meta_t*meta){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int item_size = max_uintptr(write_size,sizeof(uint32_t));
  uint32_t buf[LFTL_DIV_CEIL(2*item_size,sizeof(uint32_t))];
  memset(buf,0,sizeof(buf));
  buf[0] = meta->version;
  buf[item_size/sizeof(uint32_t)] = meta->checksum;
  nvm_write(ctx,ctx->nvm_props,ewlf_item_addr(ctx,record_addr,0),buf,2*item_size);
  ewlf_pin_item(ctx,record_addr,2);
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

/// What a record position holds, see ::ewlf_record_state.
typedef enum {
  EWLF_RECORD_NONE,      /**< no record: the position is erased, torn before its checksum was complete, tombstoned, or holds anything else */
  EWLF_RECORD_UNMARKED,  /**< magic and checksum valid, end marker all erased: a write cut during its checksum word at the latest; never taken, its checksum may be weak */
  EWLF_RECORD_CANDIDATE, /**< magic and checksum valid, end marker programmed in part or in full: taken when newest */
} ewlf_record_state_t;

/**
 * \brief Classify a record position.
 *
 * Reads the magic, and when it matches the meta-data and the data: a
 * record is valid when its checksum is the one of its data and version.
 * The end marker then tells a complete or stable record (programmed in
 * full, or in part: its own program had started, so the checksum before it
 * is complete) from one torn earlier (all erased: the checksum word may be
 * weak and read differently later), see ::find_current_record.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param meta Output: the record's version and checksum, valid unless ::EWLF_RECORD_NONE is returned
 * \param end_marker Output: how the end marker reads, valid unless ::EWLF_RECORD_NONE is returned
 */
static ewlf_record_state_t ewlf_record_state(lftl_ctx_t*ctx, const uint8_t*record_addr, lftl_meta_t*meta, unit_state_t*end_marker){
  uint64_t magic;
  nvm_read(ctx,ctx->nvm_props,&magic,record_addr,sizeof(magic));
  if(magic != EWLF_MAGIC) return EWLF_RECORD_NONE;
  ewlf_get_record_meta_ex(ctx,meta,end_marker,record_addr);
  const uint32_t recomputed = checksum(ctx,record_addr+ewlf_header_phy_size(ctx),ctx->data_size) + meta->version;
  if(recomputed != meta->checksum) return EWLF_RECORD_NONE;
  return (UNIT_ERASED == *end_marker) ? EWLF_RECORD_UNMARKED : EWLF_RECORD_CANDIDATE;
}

/// What a scan of an EWLF area chose, before anything is programmed.
typedef struct {
  const uint8_t*best;           /**< the newest candidate, NULL if none */
  uint32_t best_version;        /**< its version */
  unit_state_t best_end_marker; /**< how its end marker read */
  const uint8_t*torn;           /**< the newest unmarked record, NULL if none */
  uint32_t torn_version;        /**< its version */
} ewlf_choice_t;

static void ewlf_choice_init(ewlf_choice_t*c){
  c->best = NULL;
  c->best_version = 0;
  c->best_end_marker = UNIT_ZERO;
  c->torn = NULL;
  c->torn_version = 0;
}

/**
 * \brief Offer a record position to a choice.
 *
 * An unmarked record is remembered when it is the newest one seen; a
 * candidate replaces the best one when its version is higher, and two
 * candidates sharing a version are ::LFTL_ERROR_VERSION_COLLISION (hostile
 * content, or a bug).
 *
 * \returns true if the position holds a candidate
 */
static bool ewlf_choice_offer(lftl_ctx_t*ctx, ewlf_choice_t*c, const uint8_t*addr, ewlf_record_state_t state, const lftl_meta_t*meta, unit_state_t end_marker){
  if(EWLF_RECORD_NONE == state) return false;
  if(EWLF_RECORD_UNMARKED == state){
    if((NULL == c->torn) || (meta->version > c->torn_version)){
      c->torn = addr;
      c->torn_version = meta->version;
    }
    return false;
  }
  if((NULL != c->best) && (meta->version == c->best_version)) raise_error(ctx,LFTL_ERROR_VERSION_COLLISION);
  if((NULL == c->best) || (meta->version > c->best_version)){
    c->best = addr;
    c->best_version = meta->version;
    c->best_end_marker = end_marker;
  }
  return true;
}

/**
 * \brief Offer a unit's positions to a choice, from the lowest up to the
 * first candidate: records are written from the top down, so that candidate
 * is the newest of the unit.
 *
 * \param ctx Context of the EWLF area
 * \param unit_index Index of the erase unit
 * \param from Index of the position to start from, 0 being the top one
 * \param c The choice
 */
static void ewlf_offer_unit_from(lftl_ctx_t*ctx, unsigned int unit_index, unsigned int from, ewlf_choice_t*c){
  const uintptr_t record_size = ewlf_record_phy_size(ctx);
  const uint8_t*const top = ewlf_unit_top_record(ctx,unit_index);
  for(unsigned int i = from + 1; i > 0; i--){
    const uint8_t*const addr = top - (i-1)*record_size;
    lftl_meta_t meta;
    unit_state_t end_marker;
    const ewlf_record_state_t state = ewlf_record_state(ctx,addr,&meta,&end_marker);
    if(ewlf_choice_offer(ctx,c,addr,state,&meta,end_marker)) return;
  }
}

/**
 * \brief The baseline scan: every unit, from its bottom up to its first
 * candidate. Bounded by the unit and record counts, and right whatever the
 * area holds.
 */
static void ewlf_choose_linear(lftl_ctx_t*ctx, ewlf_choice_t*c){
  const unsigned int n_units = n_pages(ctx);
  const unsigned int records_per_unit = page_size(ctx) / ewlf_record_phy_size(ctx);
  for(unsigned int unit_index = 0; unit_index < n_units; unit_index++){
    ewlf_offer_unit_from(ctx,unit_index,records_per_unit-1,c);
  }
}

/**
 * \brief A unit's key: the version of its top record when that record is a
 * candidate in full, checksum included, 0 otherwise.
 *
 * A unit is erased right before its first record is programmed and that
 * record is its top one, so every record of a newer unit has a higher
 * version than every record of an older one: keys order the units by age,
 * and a unit without a key (erased, left partly erased by a torn erase,
 * holding a torn or tombstoned top record) is older than any keyed one. The
 * magic and the marker alone would not do: a torn erase only sets bits to 1,
 * and can leave a top record with a valid magic and a version word that
 * reads larger; the checksum rejects it.
 *
 * \param ctx Context of the EWLF area
 * \param unit_index Index of the erase unit
 * \param state Output: the top record's state
 * \param meta Output: its meta-data, valid unless the state is ::EWLF_RECORD_NONE
 */
static uint32_t ewlf_unit_key(lftl_ctx_t*ctx, unsigned int unit_index, ewlf_record_state_t*state, lftl_meta_t*meta){
  unit_state_t end_marker;
  *state = ewlf_record_state(ctx,ewlf_unit_top_record(ctx,unit_index),meta,&end_marker);
  return (EWLF_RECORD_CANDIDATE == *state) ? meta->version : 0;
}

/**
 * \brief The scan with a number of reads logarithmic in the unit and record
 * counts (the EWLF scan section of the documentation's anti-tearing page).
 *
 * Read in address order, the keys decrease from the current unit's, which is
 * the highest, except for one jump up, at the current unit itself: the units
 * above it in address are the ones written before it in this cycle, older
 * and older, the units below it are the previous cycle's, older still, and
 * the oldest ones have no key. Keeping a range of units that contains the
 * jump and comparing a probe in its middle with the range's last key tells
 * which half holds it, so the current unit costs one key per halving plus one.
 * Inside it, on a flash with `zero_overwrite`, the started positions form a
 * run from the top, since the write path pins the start marker of every
 * started position it skips: the last started one is found by halving on
 * start markers, one write unit per probe, and the newest candidate is there
 * or at the first candidate above it. Without `zero_overwrite` a skipped
 * start marker can be weak and later read erased, a hole in the run, so the
 * unit is read from its bottom up instead.
 *
 * The result is checked before it is trusted: the current unit must be a
 * local maximum in write order, both its neighbours holding a lower key or
 * none, and the record found must have a higher version than both. Only
 * hostile content or a bug can fail that check, and the caller then falls
 * back to ::ewlf_choose_linear. The two neighbours are read anyway: the
 * next unit in write order is where a torn rollover leaves its record,
 * which is offered to the choice so that it can be tombstoned.
 *
 * \returns true if the result passed the check
 */
static bool ewlf_choose_fast(lftl_ctx_t*ctx, ewlf_choice_t*c){
  const unsigned int n_units = n_pages(ctx);
  const uintptr_t record_size = ewlf_record_phy_size(ctx);
  const unsigned int records_per_unit = page_size(ctx) / record_size;
  ewlf_record_state_t state;
  lftl_meta_t meta;
  //the current unit: the jump up in the cyclic sequence of keys
  unsigned int lo = 0;
  unsigned int hi = n_units - 1;
  uint32_t last_key = ewlf_unit_key(ctx,hi,&state,&meta);
  while(lo < hi){
    const unsigned int mid = (lo + hi) / 2;
    const uint32_t key = ewlf_unit_key(ctx,mid,&state,&meta);
    if(key > last_key){
      hi = mid;
      last_key = key;
    } else {
      lo = mid + 1;
    }
  }
  const unsigned int current_unit = lo;
  if(0 == last_key) return false; //no keyed unit at all
  //its neighbours in write order: the unit written before it, above, and the
  //one it will roll over to, below
  const unsigned int prev_unit = (current_unit + 1) % n_units;
  const unsigned int next_unit = (current_unit + n_units - 1) % n_units;
  lftl_meta_t prev_meta;
  ewlf_record_state_t prev_state;
  const uint32_t prev_key = ewlf_unit_key(ctx,prev_unit,&prev_state,&prev_meta);
  lftl_meta_t next_meta;
  ewlf_record_state_t next_state;
  const uint32_t next_key = (next_unit == prev_unit) ? prev_key : ewlf_unit_key(ctx,next_unit,&next_state,&next_meta);
  if(next_unit == prev_unit){
    next_state = prev_state;
    next_meta = prev_meta;
  }
  if((prev_key >= last_key) || (next_key >= last_key)) return false;
  //the newest record of the current unit
  const uint8_t*const top = ewlf_unit_top_record(ctx,current_unit);
  unsigned int from = records_per_unit - 1;
  if(ctx->nvm_props->zero_overwrite){
    //the last started position; the top one is started: it holds the key
    unsigned int slo = 0;
    unsigned int shi = records_per_unit - 1;
    while(slo < shi){
      const unsigned int mid = (slo + shi + 1) / 2;
      if(UNIT_ERASED != ewlf_start_marker_state(ctx,top - mid*record_size)){
        slo = mid;
      } else {
        shi = mid - 1;
      }
    }
    from = slo;
  }
  ewlf_offer_unit_from(ctx,current_unit,from,c);
  if(NULL == c->best) return false;
  if((c->best_version <= prev_key) || (c->best_version <= next_key)) return false;
  //a torn rollover record at the top of the next unit
  if(EWLF_RECORD_UNMARKED == next_state){
    unit_state_t end_marker;
    ewlf_choice_offer(ctx,c,ewlf_unit_top_record(ctx,next_unit),next_state,&next_meta,end_marker = UNIT_ERASED);
  }
  return true;
}

/**
 * \brief Locate the current EWLF record and update `ctx->data`.
 *
 * Records only ever start at the top of an erase unit minus a multiple of
 * the record size, so only those positions are checked: data can never be
 * mistaken for a header, and every candidate fits in its erase unit. Units
 * are erased right before their first record is programmed and never when
 * the write order leaves them, so stale units are the normal state and the
 * newest record is the candidate with the highest version over all units
 * (::ewlf_choose_fast, ::ewlf_choose_linear).
 *
 * The scan reads and chooses; it never erases, so that a power cut at every
 * boot costs no erase (ISSUES.md LFTL-010). What it decides about the one
 * word a power cut can have left weak, the end marker of the interrupted
 * write, it decides on one reading, and on a flash with `zero_overwrite` it
 * pins the decision with one word program:
 * - end marker partially programmed: the record is stable and taken; the
 *   marker is pinned to zeros, so that it cannot read all erased at a later
 *   boot and bring the previous version back (LFTL-011);
 * - end marker all erased, a newer version than the record taken: the torn
 *   record is tombstoned, its version item pinned to zeros, so that it
 *   cannot come back later as a "new" version once its marker jitters.
 * Without `zero_overwrite` the same choices are made at every boot on the
 * word as it reads then, and the next write settles them: it numbers its
 * record above every valid checksum it steps over (::write_ewlf_core).
 *
 * With LFTL_SCAN_CROSS_CHECK defined (the Linux and ASan test builds), both
 * scans run and any disagreement is ::LFTL_INTERNAL_ERROR.
 *
 * \param ctx Context of the EWLF area
 */
static void find_current_record(lftl_ctx_t*ctx){
  ewlf_choice_t c;
  ewlf_choice_init(&c);
  if(!ewlf_choose_fast(ctx,&c)){
    ewlf_choice_init(&c);
    ewlf_choose_linear(ctx,&c);
  }
  #ifdef LFTL_SCAN_CROSS_CHECK
  {
    ewlf_choice_t l;
    ewlf_choice_init(&l);
    ewlf_choose_linear(ctx,&l);
    const bool c_torn = (NULL != c.torn) && (c.torn_version > c.best_version);
    const bool l_torn = (NULL != l.torn) && (l.torn_version > l.best_version);
    if((c.best != l.best) || (c_torn != l_torn) || (c_torn && (c.torn != l.torn))) raise_error(ctx,LFTL_INTERNAL_ERROR);
  }
  #endif
  if(NULL == c.best) raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
  if(ctx->nvm_props->zero_overwrite){
    if(UNIT_MIXED == c.best_end_marker) ewlf_pin_item(ctx,(uint8_t*)c.best,2);
    if((NULL != c.torn) && (c.torn_version > c.best_version)) ewlf_pin_item(ctx,(uint8_t*)c.torn,0);
  }
  ctx->data = (void*)(c.best + ewlf_header_phy_size(ctx));
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
 * \brief Shared core for ::write_ewlf and ::erase_ewlf.
 *
 * Places the new record below the current one in its own erase unit, at the
 * first position whose start marker reads all erased: a start marker is the
 * first word of a record to be programmed, so nothing after an erased one
 * was ever programmed. A started position on the way holds a record a power
 * cut tore, or one the scan tombstoned: it is skipped, the new record is
 * numbered above its version when its checksum is valid, so that no two
 * candidates ever share a version whatever its end marker reads later, and
 * on a flash with `zero_overwrite` its start marker is pinned to zeros, so
 * that a weak one cannot later read erased and open a hole in the run of
 * started positions. With no room left, rolls over to the top of the unit
 * immediately below, wrapping from the lowest unit back to the highest, and
 * erases that unit first, whatever it reads: a torn erase is redone, and
 * cells a torn record left behind are never programmed over (ISSUES.md
 * LFTL-013). The unit the write order leaves is not erased: it is, lazily,
 * when the order comes back to it, so a unit is erased once per cycle and
 * never at boot.
 *
 * Then writes the start marker, the data (unless `write_data` is false, in
 * which case the fresh, already-erased target data is left untouched so it
 * reads back as erased), the magic, the version and checksum, and the end
 * marker last. The checksum is computed from the data meant to be written
 * rather than from what reads back, so a record that did not program as
 * intended fails its check.
 *
 * \param dst_ctx Context of the target EWLF area
 * \param dst_nvm_addr Destination logical address (ignored if `write_data` is false)
 * \param src Source address (ignored if `write_data` is false)
 * \param size Size in bytes to write (ignored if `write_data` is false;
 *   the whole record's data is always (re)written as a unit)
 * \param write_data Whether to actually write new data, or leave the
 *   fresh target data in its erased state
 */
static void write_ewlf_core(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool write_data){
  DEBUG_PRINTLN("write_ewlf_core(%p,%p,%p,%u,%u) entry",dst_ctx,dst_nvm_addr,src,size,write_data);
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
  lftl_meta_t current_meta;
  ewlf_get_record_meta(dst_ctx,&current_meta,current_record_addr);
  uint32_t max_version = current_meta.version;

  const uintptr_t record_size = ewlf_record_phy_size(dst_ctx);
  //room below the current record in its unit: the first position whose start
  //marker reads all erased, stepping over the started ones
  const uintptr_t unit_base = (uintptr_t)ewlf_unit_base(dst_ctx,current_unit_index);
  uintptr_t candidate_addr = (uintptr_t)current_record_addr;
  bool found = false;
  while(!found && (candidate_addr - unit_base >= record_size)){
    candidate_addr -= record_size;
    uint8_t*const candidate = (uint8_t*)candidate_addr;
    const unit_state_t start_marker = ewlf_start_marker_state(dst_ctx,candidate);
    if(UNIT_ERASED == start_marker){
      found = true;
      break;
    }
    lftl_meta_t meta;
    unit_state_t end_marker;
    if(EWLF_RECORD_NONE != ewlf_record_state(dst_ctx,candidate,&meta,&end_marker)){
      if(meta.version > max_version) max_version = meta.version;
    }
    if((UNIT_ZERO != start_marker) && dst_ctx->nvm_props->zero_overwrite) ewlf_write_start_marker(dst_ctx,candidate);
  }
  uint8_t*new_record_addr;
  if(found){
    new_record_addr = (uint8_t*)candidate_addr;
  } else {
    // no room left: move to the unit immediately below, wrapping from the
    // lowest unit back to the highest one, and erase it first, whatever it
    // reads: it holds no record newer than the current one
    const unsigned int new_unit_index = (current_unit_index==0) ? (n_pages(dst_ctx)-1) : (current_unit_index-1);
    nvm_erase(dst_ctx,dst_ctx->nvm_props,ewlf_unit_base(dst_ctx,new_unit_index),1);
    new_record_addr = ewlf_unit_top_record(dst_ctx,new_unit_index);
  }
  uint8_t*const new_data_addr = new_record_addr + header_size;

  //the start marker first: from here on the position is started
  ewlf_write_start_marker(dst_ctx,new_record_addr);

  const uint8_t* src_phy_addr = src;
  lftl_ctx_t* src_ctx = dst_ctx;
  if(write_data){
    check_src_phy_addr(&src_ctx,&src_phy_addr,size);
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
  //write magic, then version+checksum, then the end marker (checksum of the
  //data meant to be written: the current data with the written range
  //replaced, or, if write_data is false, erased data)
  ewlf_write_record_magic(dst_ctx,new_record_addr);
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
  meta.version = max_version + 1;
  meta.checksum = sum + meta.version;
  meta.checksum2 = 0;
  ewlf_write_record_meta_core(dst_ctx,new_record_addr,&meta);

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
  write_ewlf_core(dst_ctx,dst_nvm_addr,src,size,true);
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
  write_ewlf_core(ctx,ctx->area,LFTL_INVALID_POINTER,0,false);
}

/**
 * \brief Format an EWLF area.
 *
 * Erases every erase unit, then writes the first record (version 1) at
 * the top of the highest unit, per "writes start at the highest possible
 * address": start marker, magic, version and checksum, end marker.
 *
 * \param ctx Context of the target EWLF area
 */
static void format_ewlf(lftl_ctx_t*ctx){
  DEBUG_PRINTLN("format_ewlf entry");
  nvm_erase(ctx,ctx->nvm_props,ctx->area, n_pages(ctx));
  uint8_t*const record_addr = ewlf_unit_top_record(ctx, n_pages(ctx)-1);
  uint8_t*const data_addr = record_addr + ewlf_header_phy_size(ctx);
  ewlf_write_start_marker(ctx,record_addr);
  ewlf_write_record_magic(ctx,record_addr);
  lftl_meta_t meta;
  meta.version = 1;
  meta.checksum = checksum(ctx,data_addr,ctx->data_size) + meta.version;
  meta.checksum2 = 0;
  ewlf_write_record_meta_core(ctx,record_addr,&meta);
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
 *   one (a constraint the layout macro ::LFTL_AREA_EWLF asserts too; kept
 *   from when a record could be copied forward within its unit).
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
