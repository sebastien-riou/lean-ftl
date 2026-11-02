#pragma once

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "lean-ftl.h"

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
 * \brief Compute (part of) a CRC-64/NVME, the checksum of EWLF records.
 *
 * Width 64, polynomial 0xAD93D23594C93659 (0x9A6C9329AC4BC9B5 reflected),
 * reflected input and output, initial value and final XOR all ones; the
 * check value of "123456789" is 0xAE8B14860A799888. This is the core only:
 * to get the full CRC, initialize `crc` to all ones and complement the
 * result. Bitwise, with a branch-free mask: no table, and a running time
 * that depends on the length alone.
 *
 * \param crc Initial/accumulated CRC value
 * \param buf Start address of the buffer
 * \param len Size in bytes of the buffer
 * \returns The updated (not yet complemented) CRC value
 */
static uint64_t crc64_update(uint64_t crc, const void*const buf, unsigned int len) {
  const uint8_t*buf8 = (const uint8_t*)buf;
  const uint64_t poly = 0x9A6C9329AC4BC9B5ull;
  while (len != 0) {
    crc = crc ^ *buf8++;
    for (unsigned int i = 0; i<8; i++) {
        const uint64_t mask = -(crc & 1);
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

/// Size in bytes of the content of an EWLF record's meta field: a 32-bit
/// version followed by a 64-bit checksum, both little endian.
#define EWLF_META_SIZE 12

/// Whether the stuck-state pin of ::find_current_record programs a flag that
/// already reads all zeros. A weak word can read all zeros, so leaving it
/// alone keeps a residual (the anti-tearing page, "What a power cut can still
/// do"); programming it needs a flash that accepts all zeros over all zeros,
/// FLASH.md open point 2, to be measured on the STM32L5 and STM32U5. Off
/// unless the build says otherwise.
#ifndef LFTL_EWLF_PIN_OVER_ZEROS
#define LFTL_EWLF_PIN_OVER_ZEROS 0
#endif

/**
 * \brief Physical size in bytes of an EWLF record's start marker, and of
 * each of its two flags: one write unit.
 *
 * The start marker is the first word of a record to be programmed, to all
 * zeros: a position is started once its start marker has a bit at 0, and
 * nothing after an erased start marker was ever programmed (see
 * ::ewlf_place_and_write). Flag-a is programmed after the meta field and
 * flag-b after flag-a, both to zeros: a bit at 0 in flag-a proves the meta
 * field complete, one in flag-b proves flag-a complete (see
 * ::ewlf_record_state).
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_flag_phy_size(lftl_ctx_t*ctx){
  return max_uintptr(ctx->nvm_props->write_size, sizeof(uint8_t));
}

/**
 * \brief Physical size in bytes of an EWLF record's meta field:
 * ::EWLF_META_SIZE rounded up to whole write units, programmed in one call.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_meta_phy_size(lftl_ctx_t*ctx){
  const uint32_t write_size = ctx->nvm_props->write_size;
  return LFTL_DIV_CEIL(EWLF_META_SIZE, write_size) * write_size;
}

/**
 * \brief Physical size in bytes of an EWLF record's whole header: start
 * marker, meta field, flag-a, flag-b.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_header_phy_size(lftl_ctx_t*ctx){
  return 3*ewlf_flag_phy_size(ctx) + ewlf_meta_phy_size(ctx);
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
 * \brief Physical size in bytes of one whole EWLF record: a multiple of the
 * write unit, since every field is one.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_record_phy_size(lftl_ctx_t*ctx){
  return ewlf_header_phy_size(ctx) + ewlf_data_phy_size(ctx);
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
 * \brief Index of the erase unit holding a record.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static unsigned int ewlf_unit_index(lftl_ctx_t*ctx, const uint8_t*record_addr){
  return (uintptr_t)(record_addr - (uint8_t*)ctx->area) / page_size(ctx);
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
 * \brief Physical address of an EWLF record's start marker: its first word.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static uint8_t* ewlf_start_marker_addr(lftl_ctx_t*ctx, const uint8_t*record_addr){
  (void)ctx;
  return (uint8_t*)record_addr;
}

/**
 * \brief Physical address of an EWLF record's meta field.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static uint8_t* ewlf_meta_addr(lftl_ctx_t*ctx, const uint8_t*record_addr){
  return (uint8_t*)record_addr + ewlf_flag_phy_size(ctx);
}

/**
 * \brief Physical address of an EWLF record's flag-a.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static uint8_t* ewlf_flag_a_addr(lftl_ctx_t*ctx, const uint8_t*record_addr){
  return (uint8_t*)record_addr + ewlf_flag_phy_size(ctx) + ewlf_meta_phy_size(ctx);
}

/**
 * \brief Physical address of an EWLF record's flag-b.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 */
static uint8_t* ewlf_flag_b_addr(lftl_ctx_t*ctx, const uint8_t*record_addr){
  return (uint8_t*)record_addr + 2*ewlf_flag_phy_size(ctx) + ewlf_meta_phy_size(ctx);
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
  return read_unit_state(ctx,ewlf_start_marker_addr(ctx,record_addr),ewlf_flag_phy_size(ctx));
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
  pin_unit(ctx,ewlf_start_marker_addr(ctx,record_addr),ewlf_flag_phy_size(ctx));
}

/**
 * \brief Program one of an EWLF record's flags to zeros: one write unit.
 *
 * \param ctx Context of the EWLF area
 * \param flag_addr Physical address of the flag, see ::ewlf_flag_a_addr and ::ewlf_flag_b_addr
 */
static void ewlf_write_flag(lftl_ctx_t*ctx, uint8_t*flag_addr){
  pin_unit(ctx,flag_addr,ewlf_flag_phy_size(ctx));
}

/// The content of an EWLF record's meta field, in volatile memory.
typedef struct {
  uint32_t version;  /**< never 0 (a tombstone) nor 0xFFFFFFFF (erased): format writes 1 */
  uint64_t checksum; /**< CRC-64/NVME of the version then the data, see ::ewlf_checksum_start */
} ewlf_meta_t;

/**
 * \brief Start the checksum of an EWLF record: a CRC-64/NVME over the four
 * bytes of its version, little endian, then its data
 * (::ewlf_checksum_update), then ::ewlf_checksum_end.
 *
 * \param version The record's version
 * \returns The checksum so far
 */
static uint64_t ewlf_checksum_start(uint32_t version){
  uint8_t bytes[sizeof(version)];
  for(unsigned int i=0;i<sizeof(version);i++) bytes[i] = (uint8_t)(version >> (8*i));
  return crc64_update(~(uint64_t)0,bytes,sizeof(bytes));
}

/**
 * \brief Update the checksum of an EWLF record with a (possibly non-RAM) range.
 *
 * \param ctx Context of the LFTL area on whose behalf this checksum is computed
 * \param crc Checksum so far
 * \param src Start address of the range
 * \param size Size in bytes of the range
 * \returns The updated checksum
 */
static uint64_t ewlf_checksum_update(lftl_ctx_t*ctx, uint64_t crc, const void*const src, uintptr_t size){
  const uint8_t*src8 = (const uint8_t*)src;
  uint64_t buf[16];
  while(size){
    const uint32_t readsize = size > sizeof(buf) ? sizeof(buf) : size;
    mem_read(ctx,buf,src8,readsize);
    crc = crc64_update(crc,buf,readsize);
    size -= readsize;
    src8 += readsize;
  }
  return crc;
}

/**
 * \brief Update the checksum of an EWLF record with erased (0xFF) bytes.
 *
 * \param crc Checksum so far
 * \param size Number of erased bytes
 * \returns The updated checksum
 */
static uint64_t ewlf_checksum_update_erased(uint64_t crc, uintptr_t size){
  uint8_t erased[16];
  memset(erased,0xFF,sizeof(erased));
  while(size){
    const uintptr_t chunk = size > sizeof(erased) ? sizeof(erased) : size;
    crc = crc64_update(crc,erased,chunk);
    size -= chunk;
  }
  return crc;
}

/**
 * \brief Finish the checksum of an EWLF record.
 *
 * \param crc Checksum so far
 * \returns The value stored in the record's meta field
 */
static uint64_t ewlf_checksum_end(uint64_t crc){
  return ~crc;
}

/**
 * \brief Encode a meta field: version then checksum, little endian, then
 * zeros up to the write unit.
 *
 * \param meta The content
 * \param bytes Destination, ::EWLF_META_SIZE bytes at least
 */
static void ewlf_meta_to_bytes(const ewlf_meta_t*meta, uint8_t*bytes){
  for(unsigned int i=0;i<sizeof(meta->version);i++) bytes[i] = (uint8_t)(meta->version >> (8*i));
  for(unsigned int i=0;i<sizeof(meta->checksum);i++) bytes[sizeof(meta->version)+i] = (uint8_t)(meta->checksum >> (8*i));
}

/**
 * \brief Decode a meta field, see ::ewlf_meta_to_bytes.
 *
 * \param meta Destination
 * \param bytes The field as read, ::EWLF_META_SIZE bytes at least
 */
static void ewlf_meta_from_bytes(ewlf_meta_t*meta, const uint8_t*bytes){
  meta->version = 0;
  for(unsigned int i=0;i<sizeof(meta->version);i++) meta->version |= ((uint32_t)bytes[i]) << (8*i);
  meta->checksum = 0;
  for(unsigned int i=0;i<sizeof(meta->checksum);i++) meta->checksum |= ((uint64_t)bytes[sizeof(meta->version)+i]) << (8*i);
}

/// One reading of an EWLF record's meta field and flags: every decision
/// about a record is made on one reading, since a word a power cut left weak
/// can read differently from one read to the next.
typedef struct {
  ewlf_meta_t meta;         /**< the meta field as read */
  unit_state_t flag_a;      /**< how flag-a's write unit reads */
  unit_state_t flag_b;      /**< how flag-b's write unit reads */
  uint32_t meta_zero_units; /**< bit i set when write unit i of the meta field read all zeros */
  bool erased;              /**< the meta field and both flags read all erased: nothing after the start marker was programmed, or reads so */
} ewlf_reading_t;

/**
 * \brief Read an EWLF record's meta field and both flags, in one read.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param r Output: the reading
 */
static void ewlf_read_record(lftl_ctx_t*ctx, const uint8_t*record_addr, ewlf_reading_t*r){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const uintptr_t flag_size = ewlf_flag_phy_size(ctx);
  const uintptr_t meta_size = ewlf_meta_phy_size(ctx);
  uint64_t buf[SIZE64(3*LFTL_WU_MAX_SIZE)];
  nvm_read(ctx,ctx->nvm_props,buf,ewlf_meta_addr(ctx,record_addr),meta_size + 2*flag_size);
  const uint8_t*const bytes = (const uint8_t*)buf;
  ewlf_meta_from_bytes(&r->meta,bytes);
  r->meta_zero_units = 0;
  r->erased = true;
  for(unsigned int i=0;i*write_size<meta_size;i++){
    const unit_state_t unit = unit_state_of(bytes + i*write_size,write_size);
    if(UNIT_ZERO == unit) r->meta_zero_units |= ((uint32_t)1) << i;
    if(UNIT_ERASED != unit) r->erased = false;
  }
  r->flag_a = unit_state_of(bytes + meta_size,flag_size);
  r->flag_b = unit_state_of(bytes + meta_size + flag_size,flag_size);
  if((UNIT_ERASED != r->flag_a) || (UNIT_ERASED != r->flag_b)) r->erased = false;
}

/**
 * \brief Program an EWLF record's meta field, in one call.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param meta The content
 */
static void ewlf_write_meta(lftl_ctx_t*ctx, uint8_t*record_addr, const ewlf_meta_t*meta){
  uint64_t buf[SIZE64(LFTL_WU_MAX_SIZE)];
  memset(buf,0,sizeof(buf));
  ewlf_meta_to_bytes(meta,(uint8_t*)buf);
  nvm_write(ctx,ctx->nvm_props,ewlf_meta_addr(ctx,record_addr),buf,ewlf_meta_phy_size(ctx));
}

/**
 * \brief Tombstone an EWLF record: program its meta field to zeros, so that
 * its version reads 0 and it is no record for good (see
 * ::ewlf_record_state).
 *
 * Every write unit of the field that did not read all zeros in the reading
 * the decision was made on is programmed, the checksum's units first, with
 * the one program a flash declared with `zero_overwrite` accepts on a unit
 * in any state (N3 of the flash page). A cut leaves the field weak: the
 * record then reads as no record, or aborted and is tombstoned again.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param meta_zero_units The reading's ::ewlf_reading_t::meta_zero_units
 */
static void ewlf_tombstone(lftl_ctx_t*ctx, uint8_t*record_addr, uint32_t meta_zero_units){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int n_units = ewlf_meta_phy_size(ctx) / write_size;
  uint8_t*const base = ewlf_meta_addr(ctx,record_addr);
  for(unsigned int i = n_units; i-- > 0;){
    if(0 == (meta_zero_units & (((uint32_t)1) << i))) pin_unit(ctx,base + i*write_size,write_size);
  }
}

/// What a record position holds, see ::ewlf_record_state.
typedef enum {
  EWLF_RECORD_NONE,     /**< no record: erased, torn before or during its meta field, tombstoned, or foreign content */
  EWLF_RECORD_ABORTED,  /**< checksum valid, both flags all erased: a write cut before flag-a, or whose flag-a is weak and reads erased */
  EWLF_RECORD_HALF,     /**< checksum valid, flag-a has a programmed bit, flag-b all erased: the data is solid, flag-a may be weak */
  EWLF_RECORD_COMPLETE, /**< checksum valid, flag-b has a programmed bit: flag-a is solid, the record is taken when newest */
} ewlf_record_state_t;

/**
 * \brief Classify a record position, from one reading of its meta field and
 * flags.
 *
 * A version reading 0xFFFFFFFF (erased) or 0 (a tombstone) is no record,
 * and that test comes before the checksum. The checksum covers the version
 * and the data. The flags are programmed one after the other, each once the
 * previous step completed, so a bit at 0 in flag-a proves the meta field
 * and the data solid, and one in flag-b proves flag-a solid (see
 * ::find_current_record).
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param r Output: the reading, meaningful unless ::EWLF_RECORD_NONE is returned
 */
static ewlf_record_state_t ewlf_record_state(lftl_ctx_t*ctx, const uint8_t*record_addr, ewlf_reading_t*r){
  ewlf_read_record(ctx,record_addr,r);
  const uint32_t version = r->meta.version;
  if((0xFFFFFFFF == version) || (0 == version)) return EWLF_RECORD_NONE;
  uint64_t crc = ewlf_checksum_start(version);
  crc = ewlf_checksum_update(ctx,crc,record_addr+ewlf_header_phy_size(ctx),ctx->data_size);
  if(ewlf_checksum_end(crc) != r->meta.checksum) return EWLF_RECORD_NONE;
  if(UNIT_ERASED != r->flag_b) return EWLF_RECORD_COMPLETE;
  if(UNIT_ERASED != r->flag_a) return EWLF_RECORD_HALF;
  return EWLF_RECORD_ABORTED;
}

/// What a scan of an EWLF area chose, before anything is programmed. A
/// version of 0 stands for "none": versions start at 1.
typedef struct {
  const uint8_t*complete;    /**< the newest complete record, NULL if none */
  uint32_t complete_version; /**< its version */
  const uint8_t*half;        /**< the newest half record, NULL if none */
  uint32_t half_version;     /**< its version */
  unit_state_t half_flag_a;  /**< how its flag-a read */
  const uint8_t*torn;        /**< the newest aborted record, NULL if none */
  uint32_t torn_version;     /**< its version */
  uint32_t torn_zero_units;  /**< which units of its meta field read all zeros */
  uint32_t max_version;      /**< the highest version of every valid record seen */
} ewlf_choice_t;

static void ewlf_choice_init(ewlf_choice_t*c){
  memset(c,0,sizeof(*c));
  c->half_flag_a = UNIT_ERASED;
}

/**
 * \brief Offer a record position to a choice.
 *
 * Every valid record raises the highest version seen. An aborted or a half
 * record is remembered when it is the newest of its kind; a complete one
 * replaces the newest complete when its version is higher, and two complete
 * records sharing a version are ::LFTL_ERROR_VERSION_COLLISION (hostile
 * content, or a bug: a complete record's meta field is solid, and the write
 * path numbers above every valid record). A half or aborted record sharing
 * a version with a complete one is reachable after a cut tombstone and
 * harmless: comparisons are strict, the complete one wins.
 *
 * \returns true if the position holds a complete record
 */
static bool ewlf_choice_offer(lftl_ctx_t*ctx, ewlf_choice_t*c, const uint8_t*addr, ewlf_record_state_t state, const ewlf_reading_t*r){
  if(EWLF_RECORD_NONE == state) return false;
  const uint32_t version = r->meta.version;
  if(version > c->max_version) c->max_version = version;
  if(EWLF_RECORD_ABORTED == state){
    if(version > c->torn_version){
      c->torn = addr;
      c->torn_version = version;
      c->torn_zero_units = r->meta_zero_units;
    }
    return false;
  }
  if(EWLF_RECORD_HALF == state){
    if(version > c->half_version){
      c->half = addr;
      c->half_version = version;
      c->half_flag_a = r->flag_a;
    }
    return false;
  }
  if((NULL != c->complete) && (version == c->complete_version) && (addr != c->complete)) raise_error(ctx,LFTL_ERROR_VERSION_COLLISION);
  if(version > c->complete_version){
    c->complete = addr;
    c->complete_version = version;
  }
  return true;
}

/**
 * \brief Offer a unit's positions to a choice, from the lowest up to the
 * first complete record: records are written from the top down, so that
 * record is the newest complete one of the unit, and the half and aborted
 * records above it in age are seen on the way.
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
    ewlf_reading_t r;
    const ewlf_record_state_t state = ewlf_record_state(ctx,addr,&r);
    if(ewlf_choice_offer(ctx,c,addr,state,&r)) return;
  }
}

/**
 * \brief The baseline scan: every unit, from its bottom up to its first
 * complete record. Bounded by the unit and record counts, and right whatever
 * the area holds.
 */
static void ewlf_choose_linear(lftl_ctx_t*ctx, ewlf_choice_t*c){
  const unsigned int n_units = n_pages(ctx);
  const unsigned int records_per_unit = page_size(ctx) / ewlf_record_phy_size(ctx);
  for(unsigned int unit_index = 0; unit_index < n_units; unit_index++){
    ewlf_offer_unit_from(ctx,unit_index,records_per_unit-1,c);
  }
}

/**
 * \brief A unit's key: the version of its top record when its checksum
 * verifies, 0 otherwise.
 *
 * A unit is erased right before its first record is programmed and that
 * record is its top one, so every record of a newer unit has a higher
 * version than every record of an older one: keys order the units by age,
 * and a unit without a key (erased, left partly erased by a torn erase,
 * holding a top record torn in its meta field or tombstoned) is older than
 * any keyed one. A valid checksum is enough for the key, whatever the
 * flags: a torn rollover record at the top of a unit keys it, and the
 * copies the scan may have placed below it are found in it. The start
 * marker alone would not do: a torn erase only sets bits to 1, and can
 * leave a top record with a version word that reads larger; the checksum
 * rejects it.
 *
 * \param ctx Context of the EWLF area
 * \param unit_index Index of the erase unit
 */
static uint32_t ewlf_unit_key(lftl_ctx_t*ctx, unsigned int unit_index){
  ewlf_reading_t r;
  const ewlf_record_state_t state = ewlf_record_state(ctx,ewlf_unit_top_record(ctx,unit_index),&r);
  return (EWLF_RECORD_NONE == state) ? 0 : r.meta.version;
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
 * Inside it the newest records are the lowest started positions: the start
 * markers are read from the bottom up to the first started one, one write
 * unit per read, and the records from there up to the first complete one.
 * The start markers are not halved on: a start marker a power cut left weak
 * can read erased at a later boot, a hole in the run of started positions,
 * which a halving would take for its end.
 *
 * The result is checked before it is trusted: the current unit must be a
 * local maximum in write order, both its neighbours holding a lower key or
 * none, and the complete record found must have a higher version than both.
 * Only hostile content, a bug, or a unit whose newest records are all half
 * or aborted (a rollover write cut in a flag, before the scan repaired it)
 * can fail that check, and the caller then falls back to
 * ::ewlf_choose_linear. The next unit in write order is then walked whole:
 * it is where a torn rollover leaves its record, and where the copies the
 * scan made of one sit below it.
 *
 * \returns true if the result passed the check
 */
static bool ewlf_choose_fast(lftl_ctx_t*ctx, ewlf_choice_t*c){
  const unsigned int n_units = n_pages(ctx);
  const uintptr_t record_size = ewlf_record_phy_size(ctx);
  const unsigned int records_per_unit = page_size(ctx) / record_size;
  //the current unit: the jump up in the cyclic sequence of keys
  unsigned int lo = 0;
  unsigned int hi = n_units - 1;
  uint32_t last_key = ewlf_unit_key(ctx,hi);
  while(lo < hi){
    const unsigned int mid = (lo + hi) / 2;
    const uint32_t key = ewlf_unit_key(ctx,mid);
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
  const uint32_t prev_key = ewlf_unit_key(ctx,prev_unit);
  const uint32_t next_key = (next_unit == prev_unit) ? prev_key : ewlf_unit_key(ctx,next_unit);
  if((prev_key >= last_key) || (next_key >= last_key)) return false;
  //the newest records of the current unit: positions fill from the top, so
  //they are the lowest started ones. The start markers are read from the
  //bottom up to the first started one, one write unit per read and no
  //checksum. A search by halving would take a start marker a power cut left
  //weak, reading erased at this boot, for the end of the run of started
  //positions and miss the records below it; a search from the bottom cannot
  //be fooled by it. The top position is started: it holds the key
  const uint8_t*const top = ewlf_unit_top_record(ctx,current_unit);
  unsigned int from = records_per_unit - 1;
  while((from > 0) && (UNIT_ERASED == ewlf_start_marker_state(ctx,top - from*record_size))) from--;
  ewlf_offer_unit_from(ctx,current_unit,from,c);
  if(NULL == c->complete) return false;
  if((c->complete_version <= prev_key) || (c->complete_version <= next_key)) return false;
  //the next unit in write order, whole
  ewlf_offer_unit_from(ctx,next_unit,records_per_unit-1,c);
  return true;
}

/**
 * \brief Whether a copy made by the scan may erase a unit: not the one
 * holding the last complete record, nor one holding a half record newer
 * than it (the exposed ancestor of a chain of copies may be such a record).
 *
 * \param ctx Context of the EWLF area
 * \param unit_index Index of the erase unit
 * \param protected_record The last complete record, or NULL
 * \param protected_version Its version, 0 if none
 */
static bool ewlf_unit_erasable(lftl_ctx_t*ctx, unsigned int unit_index, const uint8_t*protected_record, uint32_t protected_version){
  const uint8_t*const base = ewlf_unit_base(ctx,unit_index);
  if((NULL != protected_record) && (protected_record >= base) && (protected_record < base + page_size(ctx))) return false;
  const uintptr_t record_size = ewlf_record_phy_size(ctx);
  const unsigned int records_per_unit = page_size(ctx) / record_size;
  const uint8_t*const top = ewlf_unit_top_record(ctx,unit_index);
  for(unsigned int i=0;i<records_per_unit;i++){
    ewlf_reading_t r;
    if((EWLF_RECORD_HALF == ewlf_record_state(ctx,top - i*record_size,&r)) && (r.meta.version > protected_version)) return false;
  }
  return true;
}

/// What ::ewlf_place_and_write needs beyond the write itself.
typedef struct {
  const uint8_t*current_record;   /**< the record the new one goes below, and whose data fills what the write leaves untouched */
  uint32_t base_version;          /**< the new record is numbered above this, and above every valid record stepped over */
  const uint8_t*protected_record; /**< NULL, or a record whose unit a rollover must not erase (copies only) */
  uint32_t protected_version;     /**< nor a unit holding a half record newer than this (copies only) */
  bool copy;                      /**< the source is the current record's own data, and `ctx->data` is left alone */
} ewlf_write_params_t;

static uint8_t* ewlf_place_and_write(lftl_ctx_t*dst_ctx, const ewlf_write_params_t*p, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool write_data);

/**
 * \brief Copy a half record forward: write its data again as a new record,
 * through the write path, so that it ends with a solid flag-b.
 *
 * \param ctx Context of the EWLF area
 * \param c The choice, whose half record is the source
 * \returns The copy, or NULL when no position exists for it without erasing a protected unit
 */
static uint8_t* ewlf_copy_record(lftl_ctx_t*ctx, const ewlf_choice_t*c){
  ewlf_write_params_t p;
  p.current_record = c->half;
  p.base_version = c->max_version;
  p.protected_record = c->complete;
  p.protected_version = c->complete_version;
  p.copy = true;
  return ewlf_place_and_write(ctx,&p,ctx->area,LFTL_INVALID_POINTER,ctx->data_size,true);
}

/**
 * \brief Pin a half record in place, when no position is left for a copy:
 * program its flag-a, then its flag-b. Once the program of flag-b returned
 * the record is complete for good; a cut pin is retried at the next boot.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the start of the record
 * \param flag_a How flag-a read: a flag that read all zeros is left alone unless ::LFTL_EWLF_PIN_OVER_ZEROS
 */
static void ewlf_pin_flags(lftl_ctx_t*ctx, uint8_t*record_addr, unit_state_t flag_a){
  if(LFTL_EWLF_PIN_OVER_ZEROS || (UNIT_ZERO != flag_a)) ewlf_write_flag(ctx,ewlf_flag_a_addr(ctx,record_addr));
  ewlf_write_flag(ctx,ewlf_flag_b_addr(ctx,record_addr));
}

/**
 * \brief Find an EWLF area's current record and set `ctx->data` to its data
 * (the EWLF scan section of the documentation's anti-tearing page).
 *
 * The choice (::ewlf_choose_fast, or ::ewlf_choose_linear when its check
 * fails) names the newest complete, half and aborted records. Then, at most
 * one pass per record position plus two:
 * - an aborted record newer than both others is the abort of a write that
 *   never reached flag-a, or whose flag-a is weak and read erased: with
 *   `zero_overwrite` it is tombstoned, so that it never comes back, and the
 *   choice runs again; without, it is ignored (it was never exposed: an
 *   exposed record has a bit in flag-a that never reads erased again);
 * - a half record newer than the complete one holds solid data behind a
 *   flag-a that may be weak: it is copied forward (::ewlf_copy_record) and
 *   the choice runs again, finding the copy complete. When no position is
 *   left without erasing the unit of the complete record or of a half
 *   record newer than it, the stuck state, it is pinned in place with
 *   `zero_overwrite` (::ewlf_pin_flags) or, without, exposed as it is: the
 *   one residual, see the documentation;
 * - otherwise the complete record is exposed, or
 *   ::LFTL_ERROR_NO_VALID_VERSION raised when there is none.
 * A record is thus exposed either after a program that returned in this
 * very boot, or on a reading of flag-b with a bit, which proves flag-a
 * solid: it is half or complete at every later boot, it is never tombstoned
 * (only aborted readings are), its unit is never erased by a copy, and the
 * application's writes erase only the unit below the current record's. A
 * half record is never tombstoned: it may be the record exposed at an
 * earlier boot, when its flag-b read a bit. `ctx->data` is assigned once, at
 * the end, so that a cut during a copy leaves it unset.
 *
 * With LFTL_SCAN_CROSS_CHECK defined (the Linux and ASan test builds), both
 * scans run and any disagreement on what matters is ::LFTL_INTERNAL_ERROR.
 *
 * \param ctx Context of the EWLF area
 */
static void find_current_record(lftl_ctx_t*ctx){
  const uintptr_t header_size = ewlf_header_phy_size(ctx);
  const unsigned int max_passes = n_pages(ctx) * (page_size(ctx) / ewlf_record_phy_size(ctx)) + 2;
  for(unsigned int pass = 0; ; pass++){
    //a program that reports success without changing the flash must not spin
    if(pass > max_passes) raise_error(ctx,LFTL_INTERNAL_ERROR);
    ewlf_choice_t c;
    ewlf_choice_init(&c);
    if(!ewlf_choose_fast(ctx,&c)){
      ewlf_choice_init(&c);
      ewlf_choose_linear(ctx,&c);
    }
    const bool half_newest = c.half_version > c.complete_version;
    const bool torn_newest = (c.torn_version > c.complete_version) && (c.torn_version > c.half_version);
    #ifdef LFTL_SCAN_CROSS_CHECK
    {
      ewlf_choice_t l;
      ewlf_choice_init(&l);
      ewlf_choose_linear(ctx,&l);
      const bool l_half_newest = l.half_version > l.complete_version;
      const bool l_torn_newest = (l.torn_version > l.complete_version) && (l.torn_version > l.half_version);
      if((c.complete != l.complete) || (half_newest != l_half_newest) || (half_newest && (c.half != l.half))
        || (torn_newest != l_torn_newest) || (torn_newest && (c.torn != l.torn))) raise_error(ctx,LFTL_INTERNAL_ERROR);
    }
    #endif
    if(torn_newest && ctx->nvm_props->zero_overwrite){
      ewlf_tombstone(ctx,(uint8_t*)c.torn,c.torn_zero_units);
      continue;
    }
    if(half_newest){
      if(NULL != ewlf_copy_record(ctx,&c)) continue;
      //the stuck state
      if(ctx->nvm_props->zero_overwrite){
        ewlf_pin_flags(ctx,(uint8_t*)c.half,c.half_flag_a);
        continue;
      }
      ctx->data = (void*)(c.half + header_size);
      return;
    }
    if(NULL == c.complete) raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
    ctx->data = (void*)(c.complete + header_size);
    return;
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
 * \brief Place and write one EWLF record, for the application's writes
 * (::write_ewlf_core) and for the scan's copies (::ewlf_copy_record).
 *
 * Places the new record below the current one in its own erase unit, at the
 * first position whose start marker reads all erased, and whose meta field
 * and flags too: a start marker is the first word of a record to be
 * programmed, so nothing after an erased one was ever programmed, unless the
 * marker is weak and the position was stepped over and tombstoned since
 * (below). A started position on the way holds a record a power
 * cut tore, one the scan tombstoned, or a copy it made: it is skipped, the
 * new record is numbered above its version when its checksum is valid, so
 * that no two candidates ever share a version whatever its flags read
 * later, and on a flash with `zero_overwrite` its start marker is pinned to
 * zeros, so that a weak one cannot later read erased and open a hole in the
 * run of started positions, and its meta field is tombstoned when it does
 * not read valid, so that a weak one cannot later read valid with a
 * version about to be reused. With no room left, rolls over to the top of
 * the unit immediately below, wrapping from the lowest unit back to the
 * highest, and erases that unit first, whatever it reads: a torn erase is
 * redone, and cells a torn record left behind are never programmed over
 * (ISSUES.md LFTL-013). The unit the write order leaves is not erased: it
 * is, lazily, when the order comes back to it, so a unit is erased once per
 * cycle and never at boot. A copy may not erase the unit of the last
 * complete record, nor one holding a half record newer than it
 * (::ewlf_unit_erasable): it then returns NULL and writes nothing.
 *
 * Then writes the start marker, the data (unless `write_data` is false, in
 * which case the fresh, already-erased target data is left untouched so it
 * reads back as erased), the meta field in one program, flag-a, and flag-b
 * last. The checksum is computed from the data meant to be written rather
 * than from what reads back, so a record that did not program as intended
 * fails its check.
 *
 * \param dst_ctx Context of the target EWLF area
 * \param p The current record, the version base, the protection and whether this is a copy
 * \param dst_nvm_addr Destination logical address
 * \param src Source address (ignored for a copy: the current record's data)
 * \param size Size in bytes to write (ignored when `write_data` is false:
 *   the whole record's data is always (re)written as a unit)
 * \param write_data Whether to actually write new data, or leave the
 *   fresh target data in its erased state
 * \returns The new record, or NULL when a copy had no position
 */
static uint8_t* ewlf_place_and_write(lftl_ctx_t*dst_ctx, const ewlf_write_params_t*p, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool write_data){
  DEBUG_PRINTLN("ewlf_place_and_write(%p,%p,%p,%u,%u) entry",dst_ctx,dst_nvm_addr,src,size,write_data);
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
  //the current record: the requested range is checked by the caller, and
  //once extended to whole write units it may end in the padding after
  //data_size, which every record has room for (see data_phy_size)
  const uintptr_t offset = dst_nvm_addr_aligned - (uintptr_t)dst_ctx->area;
  const uintptr_t header_size = ewlf_header_phy_size(dst_ctx);
  const uint8_t*const current_record_addr = p->current_record;
  const uint8_t*const current_data = current_record_addr + header_size;
  const unsigned int current_unit_index = ewlf_unit_index(dst_ctx,current_record_addr);
  uint32_t max_version = p->base_version;

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
    ewlf_reading_t r;
    const ewlf_record_state_t state = ewlf_record_state(dst_ctx,candidate,&r);
    //free: nothing after the start marker reads programmed either. A start
    //marker a power cut left weak can read erased at a boot after the
    //position was stepped over and its meta field tombstoned: the record
    //would then be programmed over that tombstone
    if((UNIT_ERASED == start_marker) && r.erased){
      found = true;
      break;
    }
    if(EWLF_RECORD_NONE != state){
      if(r.meta.version > max_version) max_version = r.meta.version;
    } else if(dst_ctx->nvm_props->zero_overwrite){
      //a meta field that does not read valid may be weak, whether it reads
      //erased or garbage: it must not read valid at a later boot, with a
      //version about to be reused. The units that already read zeros are
      //left alone; over erased ones this is an ordinary program
      ewlf_tombstone(dst_ctx,candidate,r.meta_zero_units);
    }
    if((UNIT_ZERO != start_marker) && dst_ctx->nvm_props->zero_overwrite) ewlf_write_start_marker(dst_ctx,candidate);
  }
  uint8_t*new_record_addr;
  if(found){
    new_record_addr = (uint8_t*)candidate_addr;
  } else {
    // no room left: move to the unit immediately below, wrapping from the
    // lowest unit back to the highest one, and erase it first, whatever it
    // reads: it holds no record newer than the current one, unless the copy
    // chain of the scan wrapped around, which the protection refuses
    const unsigned int new_unit_index = (current_unit_index==0) ? (n_pages(dst_ctx)-1) : (current_unit_index-1);
    if(p->copy && !ewlf_unit_erasable(dst_ctx,new_unit_index,p->protected_record,p->protected_version)) return NULL;
    nvm_erase(dst_ctx,dst_ctx->nvm_props,ewlf_unit_base(dst_ctx,new_unit_index),1);
    new_record_addr = ewlf_unit_top_record(dst_ctx,new_unit_index);
  }
  uint8_t*const new_data_addr = new_record_addr + header_size;

  //the start marker first: from here on the position is started
  ewlf_write_start_marker(dst_ctx,new_record_addr);

  const uint8_t* src_phy_addr = (const uint8_t*)src;
  lftl_ctx_t* src_ctx = dst_ctx;
  if(write_data){
    if(p->copy){
      //the current record's own data, read where it is: never through
      //check_src_phy_addr, which takes an address in the area's logical range
      //for a logical one
      src_phy_addr = current_data;
    } else {
      check_src_phy_addr(&src_ctx,&src_phy_addr,size);
    }
    write_src_phy_addr_to_dst_phy_addr(
      dst_ctx,
      offset,
      size,
      new_data_addr,
      current_data,
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
  //the meta field: the version, and the checksum of the data meant to be
  //written (the current data with the written range replaced, or, if
  //write_data is false, erased data); then flag-a, then flag-b
  ewlf_meta_t meta;
  meta.version = max_version + 1;
  uint64_t crc = ewlf_checksum_start(meta.version);
  if(write_data){
    const uintptr_t first = (uintptr_t)dst_nvm_addr - (uintptr_t)dst_ctx->area;
    crc = ewlf_checksum_update(dst_ctx,crc,current_data,first);
    crc = ewlf_checksum_update(src_ctx,crc,src_phy_addr,size);
    crc = ewlf_checksum_update(dst_ctx,crc,current_data+first+size,dst_ctx->data_size-first-size);
  } else {
    crc = ewlf_checksum_update_erased(crc,dst_ctx->data_size);
  }
  meta.checksum = ewlf_checksum_end(crc);
  ewlf_write_meta(dst_ctx,new_record_addr,&meta);
  ewlf_write_flag(dst_ctx,ewlf_flag_a_addr(dst_ctx,new_record_addr));
  ewlf_write_flag(dst_ctx,ewlf_flag_b_addr(dst_ctx,new_record_addr));

  if(!p->copy) dst_ctx->data = new_data_addr;
  DEBUG_PRINTLN("ewlf_place_and_write exit");
  return new_record_addr;
}

/**
 * \brief Shared core for ::write_ewlf and ::erase_ewlf: the application's
 * writes, below the current record.
 *
 * The current record is the one the scan exposed, or the one the last write
 * left: its meta field is solid (it is complete, or half with a bit in
 * flag-a, which proves the meta field complete), so the new record is
 * numbered from its version.
 *
 * \param dst_ctx Context of the target EWLF area
 * \param dst_nvm_addr Destination logical address
 * \param src Source address
 * \param size Size in bytes to write (ignored when `write_data` is false)
 * \param write_data Whether to actually write new data, or leave the
 *   fresh target data in its erased state
 */
static void write_ewlf_core(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool write_data){
  DEBUG_PRINTLN("write_ewlf_core(%p,%p,%p,%u,%u) entry",dst_ctx,dst_nvm_addr,src,size,write_data);
  //check the range as requested: once extended to whole write units, size could wrap around
  if(write_data) check_in_data(dst_ctx, dst_nvm_addr, size);
  //locate the current record
  if(LFTL_INVALID_POINTER == dst_ctx->data) find_current_record(dst_ctx);
  ewlf_write_params_t p;
  p.current_record = ((const uint8_t*)dst_ctx->data) - ewlf_header_phy_size(dst_ctx);
  ewlf_reading_t r;
  ewlf_read_record(dst_ctx,p.current_record,&r);
  p.base_version = r.meta.version;
  p.protected_record = NULL;
  p.protected_version = 0;
  p.copy = false;
  ewlf_place_and_write(dst_ctx,&p,dst_nvm_addr,src,size,write_data);
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
 * address": start marker, meta field, flag-a, flag-b, its data left erased.
 *
 * \param ctx Context of the target EWLF area
 */
static void format_ewlf(lftl_ctx_t*ctx){
  DEBUG_PRINTLN("format_ewlf entry");
  nvm_erase(ctx,ctx->nvm_props,ctx->area, n_pages(ctx));
  uint8_t*const record_addr = ewlf_unit_top_record(ctx, n_pages(ctx)-1);
  uint8_t*const data_addr = record_addr + ewlf_header_phy_size(ctx);
  ewlf_write_start_marker(ctx,record_addr);
  ewlf_meta_t meta;
  meta.version = 1;
  meta.checksum = ewlf_checksum_end(ewlf_checksum_update_erased(ewlf_checksum_start(meta.version),ctx->data_size));
  ewlf_write_meta(ctx,record_addr,&meta);
  ewlf_write_flag(ctx,ewlf_flag_a_addr(ctx,record_addr));
  ewlf_write_flag(ctx,ewlf_flag_b_addr(ctx,record_addr));
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
