/*! \file */

#pragma once
#include <stdint.h>

/// Value used for invalid pointers.
#define LFTL_INVALID_POINTER ((void*)-1)

#ifndef LFTL_WU_MAX_SIZE
  #define LFTL_WU_MAX_SIZE 128 
#endif

/// Compute the size required for ``transaction_tracker``. 
/// See ::lftl_transaction_start.
/// \param data_size   Size of the data in the target LFTL area, in bytes
/// \param write_size  Size of the minimum write unit in the NVM, in bytes
///
#define LFTL_TRANSACTION_TRACKER_SIZE_LL(data_size,write_size) (\
  ((((data_size)+(write_size)-1)/(write_size))+7)\
  /8)

/// Compute the size required for ``transaction_tracker``. 
/// See ::lftl_transaction_start.
/// \param ctx  Pointer to lftl_ctx_t
///
#define LFTL_TRANSACTION_TRACKER_SIZE(ctx) \
  LFTL_TRANSACTION_TRACKER_SIZE_LL((ctx)->data_size,(ctx)->nvm_props->write_size)

/// @name Return codes
/// @{

/// Success. 
#define LFTL_SUCCESS 0x00
/// Internal error / corruption: area contains two or more slots with the same version
#define LFTL_ERROR_VERSION_COLLISION 0x01
/// Internal error / corruption: area does not contains any valid slot
#define LFTL_ERROR_NO_VALID_VERSION 0x02
/// Error: base address is not within the valid data address range for this area
#define LFTL_ERROR_FIRST_NOT_IN_DATA 0x03
/// Error: last address is not within the valid data address range for this area
#define LFTL_ERROR_LAST_NOT_IN_DATA 0x04
/// Error: base address is not aligned
#define LFTL_ERROR_BASE_MISALIGNED 0x05
/// Error: size is not aligned
#define LFTL_ERROR_SIZE_MISALIGNED 0x06
/// Error: a basic write is attempted during a transaction
#define LFTL_ERROR_TRANSACTION_ONGOING 0x07
/// Error: a transaction API call is attempted outside of a transaction
#define LFTL_ERROR_NO_TRANSACTION 0x08
/// Error: the same address is written twice during a transaction
#define LFTL_ERROR_TRANSACTION_OVERWRITE 0x09
/// Error: the write unit size is too large, max is defined by LFTL_WU_MAX_SIZE
#define LFTL_ERROR_WU_SIZE_TOO_LARGE 0x0A
/// Base value for errors reported by the erase function. Bits 0 to 7 may give more details.
#define LFTL_ERROR_LOW_LEVEL_ERASE 0x0100
/// Base value for error reported by the write function. Bits 0 to 7 may give more details.
#define LFTL_ERROR_LOW_LEVEL_WRITE 0x0200
/// Base value for error reported by the read function. Bits 0 to 7 may give more details.
#define LFTL_ERROR_LOW_LEVEL_READ 0x0300
/// Most likely a bug in LFTL or a hardware level issue
#define LFTL_INTERNAL_ERROR -1
/// @}

/** @struct lftl_nvm_props_struct
 *  Properties of the physical NVM
 *
 */
typedef struct lftl_nvm_props_struct {
  void*base;          /**< the base address of the entire NVM */
  uintptr_t size;     /**< the size of the entire NVM (used to select between direct access and nvm_read_t) */
  uint32_t write_size;/**< at least what the NVM is supporting, or a multiple of it */
  uint32_t erase_size;/**< at least what the NVM is supporting, or a multiple of it */
} lftl_nvm_props_t;

/**
 * \brief Callback to erase physical NVM
 *
 * It erases one or more pages. 
 * If `n_pages` = 0 it shall immediately return 0.
 * \param base_address The start address of the range to erase
 * \param n_pages The number of physical pages to erase
 * 
 * \returns 0 or an error code
 */
typedef uint8_t (*nvm_erase_t)(void*base_address, unsigned int n_pages);

/**
 * \brief Callback to write physical NVM
 *
 * It writes one or more bytes. 
 * 
 * If `size` = 0 it shall immediately return 0.
 * 
 * The destination range may cross multiple physical page boundaries.
 * 
 * The source may be within the NVM but is guaranteed to not overlap 
 * with the destination range.
 * 
 * \param dst_nvm_addr The start address of the destination. It is always aligned on WU_SIZE.
 * \param src The start address of the source. It may be misaligned if top level API are called with misaligned source address.
 * \param size The size in bytes of the data to write. It is always a multiple of WU_SIZE.
 * 
 * \returns 0 or an error code
 */
typedef uint8_t (*nvm_write_t)(void*dst_nvm_addr, const void*const src, uintptr_t size);

/**
 * \brief Callback to read physical NVM
 *
 * It reads one or more bytes. 
 * 
 * If `size` = 0 it shall immediately return 0.
 * 
 * The source range may cross multiple physical page boundaries.
 * 
 * The destination is in a regular RAM.
 * 
 * \param dst The start address of the destination
 * \param src_nvm_addr The start address of the source
 * \param size The size in bytes of the data to write
 * 
 * \returns 0 or an error code
 */
typedef uint8_t (*nvm_read_t)(void* dst, const void*const src_nvm_addr, uintptr_t size);

/**
 * \brief Callback for error handling
 *
 * It is called to handle fatal errors, it shall not return.
 * 
 * A typical implementation will longjump.
 * 
 * \param err_code The error code
 */
typedef void (*error_handler_t)(uint32_t err_code);

/** @struct lftl_ctx_struct
 *  Structure defining the context for an LFTL area.
 * 
 *  User shall initialize each member before calling any LFTL function.
 */
typedef struct lftl_ctx_struct {
  lftl_nvm_props_t*nvm_props;     /**< Properties of the targeted NVM. */
  void *area;                     /**< Base address of the LFTL area. */
  uintptr_t area_size;            /**< Total size in bytes of the LFTL area. */
  void *data;                     /**< Initialize it to ::LFTL_INVALID_POINTER. */
  uintptr_t data_size;            /**< Size in bytes of the data in this LFTL area. */
  nvm_erase_t erase;              /**< Erase function for this area. */
  nvm_write_t write;              /**< Write function for this area. */
  nvm_read_t read;                /**< Read function for this area. */
  error_handler_t error_handler;  /**< Error handler function for this area. */
  void *transaction_tracker;      /**< Initialize it ::LFTL_INVALID_POINTER. */
  void *next;                     /**< Initialize it ::LFTL_INVALID_POINTER. */
} lftl_ctx_t;

/** @name Meta information API
 * Functions in this group can be called at any time.
 * @{
 */

////////////////////////////////////////////////////////////
/// \brief Return lean-ftl library version as a string
///
/// lean-ftl follows https://semver.org/
///
/// Release versions have the format `vX.Y.Z`
/// where `X` is `MAJOR`, `Y` is `MINOR` and `Z` is `PATCH`
/// 
/// \returns the version as a null terminated string
///
////////////////////////////////////////////////////////////
const char*lftl_version();

////////////////////////////////////////////////////////////
/// \brief Return lean-ftl library version timestamp
///
/// lean-ftl version timestamp is the [Unix epoch](https://en.wikipedia.org/wiki/Unix_time) of the 
/// commit in git repository.
///
/// When a build is "dirty" or with "untracked" files
/// the version timestamp is the [Unix epoch](https://en.wikipedia.org/wiki/Unix_time) of the build
/// 
/// \returns the version timestamp
///
////////////////////////////////////////////////////////////
uint64_t lftl_version_timestamp();

////////////////////////////////////////////////////////////
/// \brief Return lean-ftl library build type
///
/// lean-ftl can be built for different goals.
/// Currently the following build types are supported:
///
///   - debug
///   - minSizeRel 
/// 
/// \returns the build type as a null terminated string
///
////////////////////////////////////////////////////////////
const char* lftl_build_type();
/** @} */

/** @name Initialization API
 * 
 * @{
 */

////////////////////////////////////////////////////////////
/// \brief Initialize LFTL library
///
/// This un-register any previously registered areas.
/// Call this before any other function  (except the Meta information API group).
///
////////////////////////////////////////////////////////////
void lftl_init_lib();

////////////////////////////////////////////////////////////
/// \brief Register an LFTL area
///
/// Register and LFTL area. This allows to 
/// be able to copy data from the LFTL area into 
/// another by using the write functions, i.e., without
/// intermediate buffering.
/// Call this for each area, after ::lftl_init_lib
/// and before any other function (except the Meta information API group).
/// \param ctx Context of the LFTL area to register
///
////////////////////////////////////////////////////////////
void lftl_register_area(lftl_ctx_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Format an LFTL area
///
/// This call is not covered by anti-tearing, this is the only one.
/// In order to erase data with anti-tearing, use ::lftl_erase_all or
/// keep track of operations using a variable in another LFTL area.
/// \param ctx Context of the target LFTL area
///
////////////////////////////////////////////////////////////
void lftl_format(lftl_ctx_t*ctx);//NOT covered by anti-tearing

/** @} */

/** @name Main API
 * All functions in this group are covered by anti-tearing.
 * They shall be called with a valid context on a properly formated LFTL area.
 * See ::lftl_format for the initial formating of an LFTL area.
 * See ::lftl_init_lib and ::lftl_register_area for the initialization of contexts.
 * @{
 */

////////////////////////////////////////////////////////////
/// \brief Retrieve the LFTL context correponding to an 
/// address, if any.
///
/// \param addr Address to search
/// \returns a valid LFTL context or LFTL_INVALID_POINTER
////////////////////////////////////////////////////////////
lftl_ctx_t*lftl_get_ctx(const void*const addr);

////////////////////////////////////////////////////////////
/// \brief Erase all the data of an LFTL area
///
/// Note that the erasure is 'logical'. At physical level, 
/// previous version of the data may still remain.
/// \param ctx Context of the target LFTL area
///
////////////////////////////////////////////////////////////
void lftl_erase_all(lftl_ctx_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Start a transaction
///
/// `transaction_tracker` is used from this call until a call to
/// ::lftl_transaction_commit or ::lftl_transaction_abort.
///
/// Performance considerations: this function erase one slot.
///
/// \param ctx Context of the target LFTL area
/// \param transaction_tracker Volatile buffer, see ::LFTL_TRANSACTION_TRACKER_SIZE
////////////////////////////////////////////////////////////
void lftl_transaction_start(lftl_ctx_t*ctx, void *const transaction_tracker);

////////////////////////////////////////////////////////////
/// \brief Commit a transaction
///
/// After this call, the transaction_tracker buffer can be discarded.
///
/// Performance considerations: this function writes at most
/// one slot + meta data.
///
/// \param ctx Context of the target LFTL area
////////////////////////////////////////////////////////////
void lftl_transaction_commit(lftl_ctx_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Abort a transaction
///
/// After this call, the transaction_tracker buffer can be discarded.
/// \param ctx Context of the target LFTL area
////////////////////////////////////////////////////////////
void lftl_transaction_abort(lftl_ctx_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Write aligned data to NVM
///
/// This function detects if the write is part of a transaction 
/// or not.
/// \param ctx          Context of the target LFTL area
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area, MUST be aligned on a write unit (WU_SIZE)
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes, MUST be multiple of write unit size (WU_SIZE)
////////////////////////////////////////////////////////////
void lftl_write(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

////////////////////////////////////////////////////////////
/// \brief Read data from LFTL area
///
/// When a transaction is on-going, read the current data,
/// i.e., the data as it was before the start of the transaction.
/// \param ctx          Context of the target LFTL area
/// \param dst          Destination address, it shall be in a volatile memory
/// \param src_nvm_addr Source address, it shall be within the target LFTL area
/// \param size         Size in bytes
////////////////////////////////////////////////////////////
void lftl_read(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size);//read current data, i.e. latest commited version

////////////////////////////////////////////////////////////
/// \brief Read data from LFTL area
///
/// When a transaction is on-going, read the new data,
/// i.e., the data written but not commited yet. Keep it mind
/// that the transaction may be aborded, so the data read by that
/// function may not be available anymore.
/// \param ctx          Context of the target LFTL area
/// \param dst          Destination address, it shall be in a volatile memory
/// \param src_nvm_addr Source address, it shall be within the target LFTL area
/// \param size         Size in bytes
////////////////////////////////////////////////////////////
void lftl_read_newer(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size);//read "new" data or current data if no transaction

////////////////////////////////////////////////////////////
/// \brief Read data from an LFTL area, a regular NVM area or regular memory
///
/// When a transaction is on-going, read the current data,
/// i.e., the data as it was before the start of the transaction.
/// \param dst          Destination address, it shall be in a volatile memory
/// \param src_nvm_addr Source address, it shall be within the target LFTL area
/// \param size         Size in bytes
////////////////////////////////////////////////////////////
void lftl_memread(void*dst, const void*const src, uintptr_t size);

////////////////////////////////////////////////////////////
/// \brief Read data from an LFTL area, a regular NVM area or regular memory
///
/// When a transaction is on-going, read the new data,
/// i.e., the data written but not commited yet. Keep it mind
/// that the transaction may be aborded, so the data read by that
/// function may not be available anymore.
/// \param dst          Destination address, it shall be in a volatile memory
/// \param src_nvm_addr Source address, it shall be within the target LFTL area
/// \param size         Size in bytes
////////////////////////////////////////////////////////////
void lftl_memread_newer(void*dst, const void*const src, uintptr_t size);
/** @} */

/** @name Low level API
 * Prefere using functions from the Main API unless you know what you are doing.
 * 
 * All functions in this group are covered by anti-tearing.
 * They shall be called with a valid context on a properly formated LFTL area.
 * See ::lftl_format for the initial formating of an LFTL area.
 * 
 * @{
 */

////////////////////////////////////////////////////////////
/// \brief Write data to NVM in an LFTL area
///
/// Update partially or totally an LFTL area. 
/// This function is not allowed when a transaction is on-going.
///
/// Performance considerations: 
/// Each call performs a full erase and write of one slot and meta-data.
/// The only exception is if size is 0, then it just returns without touching the NVM at all.
///
/// \param ctx          LFTL area context
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes
///
////////////////////////////////////////////////////////////
void lftl_basic_write(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

////////////////////////////////////////////////////////////
/// \brief Write aligned data to NVM in an LFTL area
///
/// Update partially or totally an LFTL area. 
/// This function is allowed only when a transaction is on-going.
///
/// Performance considerations: 
/// Each call writes the specified number of bytes to NVM.
///
/// \param ctx          LFTL area context
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area, MUST be aligned on a write unit (WU_SIZE)
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes, MUST be multiple of write unit size (WU_SIZE)
///
////////////////////////////////////////////////////////////
void lftl_transaction_write(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

////////////////////////////////////////////////////////////
/// \brief Read data from NVM
///
/// This function is allowed only when a transaction is on-going,
/// It reads the new data, i.e., the data written but not commited yet. 
/// Keep it mind that the transaction may be aborded, so the data read by that function may not be available anymore.
/// \param ctx          Context of the target LFTL area
/// \param dst          Destination address, it MUST be in a volatile memory
/// \param src_nvm_addr Source address, it MUST be within the target LFTL area
/// \param size         Size in bytes
////////////////////////////////////////////////////////////
void lftl_transaction_read(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size);//read "new" data during a transaction
/** @} */

/** @name Foot-guns API
 * Functions in this group are useful but can break transaction mechanism
 * if used incorrectly. They remove restrictions on data alignement and size
 * of ::lftl_write and ::lftl_transaction_write. They work well as long as you do not target the 
 * same write unit twice during a transaction. If it happens, the behavior 
 * depends on the target platform. It can be:
 * - hardware fault raised during the second write
 * - hardware fault raise when reading back
 * - no hardware fault but incorrect read value
 * 
 * All functions in this group are covered by anti-tearing.
 * They shall be called with a valid context on a properly formated LFTL area.
 * See ::lftl_format for the initial formating of an LFTL area.
 * 
 * @{
 */

////////////////////////////////////////////////////////////
/// \brief Write aligned or unaligned data to NVM
///
/// This function detects if the write is part of a transaction 
/// or not.
/// \param ctx          Context of the target LFTL area
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes
////////////////////////////////////////////////////////////
void lftl_write_any(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

////////////////////////////////////////////////////////////
/// \brief Write aligned or unaligned data to NVM in an LFTL area
///
/// Update partially or totally an LFTL area. 
/// This function is allowed only when a transaction is on-going.
///
/// Performance considerations: 
/// Each call writes the specified number of bytes to NVM.
///
/// \param ctx          LFTL area context
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes
///
////////////////////////////////////////////////////////////
void lftl_transaction_write_any(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

/** @} */

#define LFTL_META_N_ITEMS 3

#ifndef SIZE64
/// Convert a size in bytes into the minimum number of ``uint64_t``.
#define SIZE64(size) (((size)+7)/8)
#endif

#ifndef BITS_PER_BYTE
/// Number of bits in one byte.
#define BITS_PER_BYTE 8
#endif

/// Division with rounding to ceiling.
#define LFTL_DIV_CEIL(d,q) (((d)+(q)-1)/(q))


#ifdef LFTL_DEFINE_HELPERS
//helpers to declare LFTL areas
//assumes they are all within a variable named "nvm"
//example of declaration:
//typedef struct data_flash_struct {
//  LFTL_AREA(my_area,
//    uint64_t my_nvm_array_var[SIZE64(1024)];
//    LFTL_DATA(my_nvm_uint64_var,8);
//    //...more variables...
//    ,1*2)//ceiling(size of the area / FLASH_SW_PAGE_SIZE) * 2 
//  //more LFTL_AREA can be declared here
//} __attribute__ ((aligned (FLASH_SW_PAGE_SIZE))) data_flash_t;
//data_flash_t nvm __attribute__ ((section (".data_flash")));

//WU_SIZE shall be the size of write unit, in bytes
//FLASH_SW_PAGE_SIZE shall be the size of the page unit, in bytes, or a multiple of it

#ifndef STR
  #define STR_INNER(x) # x
  #define STR(x) STR_INNER(x)
#endif

#ifndef CONCAT
  #define CONCAT_INNER(x,y) x ## y
  #define CONCAT(x,y) CONCAT_INNER(x,y)
#endif

#ifndef EVAL
  #define EVAL(...) EVAL1024(__VA_ARGS__)
  #define EVAL1024(...) EVAL512(EVAL512(__VA_ARGS__))
  #define EVAL512(...) EVAL256(EVAL256(__VA_ARGS__))
  #define EVAL256(...) EVAL128(EVAL128(__VA_ARGS__))
  #define EVAL128(...) EVAL64(EVAL64(__VA_ARGS__))
  #define EVAL64(...) EVAL32(EVAL32(__VA_ARGS__))
  #define EVAL32(...) EVAL16(EVAL16(__VA_ARGS__))
  #define EVAL16(...) EVAL8(EVAL8(__VA_ARGS__))
  #define EVAL8(...) EVAL4(EVAL4(__VA_ARGS__))
  #define EVAL4(...) EVAL2(EVAL2(__VA_ARGS__))
  #define EVAL2(...) EVAL1(EVAL1(__VA_ARGS__))
  #define EVAL1(...) __VA_ARGS__
#endif


#ifndef BYTES_TO_BITS_1
  #define BYTES_TO_BITS_1 8
  #define BYTES_TO_BITS_2 16
  #define BYTES_TO_BITS_4 32
  #define BYTES_TO_BITS_8 64
#endif

#if WU_SIZE > 8
  #define LFTL_DAT_TYPE_WIDTH 64
  #define DAT_PER_WU (SIZE64(WU_SIZE))
  #define SIZE_LFTL_DAT(size) SIZE64(size)
#else
  #define LFTL_DAT_TYPE_SIZE WU_SIZE

  #define LFTL_DAT_TYPE_WIDTH CONCAT(BYTES_TO_BITS_ , LFTL_DAT_TYPE_SIZE)
  #define DAT_PER_WU 1
  #define SIZE_LFTL_DAT(size) (((size)+LFTL_DAT_TYPE_SIZE-1)/LFTL_DAT_TYPE_SIZE)
#endif

#define LFTL_DAT_TYPE_BUILDER(VAL) uint ## VAL ## _t
#define LFTL_DAT_TYPE_BUILDER2(VAL) LFTL_DAT_TYPE_BUILDER(VAL)
#define LFTL_DAT_TYPE LFTL_DAT_TYPE_BUILDER2(LFTL_DAT_TYPE_WIDTH)

typedef LFTL_DAT_TYPE lftl_dat_t;

typedef __attribute__ ((aligned (sizeof(lftl_dat_t)))) struct {
  lftl_dat_t dat[DAT_PER_WU];
} lftl_wu_t;

typedef lftl_dat_t __attribute__ ((aligned (FLASH_SW_PAGE_SIZE))) flash_sw_page_t[SIZE_LFTL_DAT(FLASH_SW_PAGE_SIZE)];

/// Round a value to the minimum number of multiples of a unit.
#define LFTL_ROUND_UP(val,unit) (LFTL_DIV_CEIL(val,unit)*(unit))

/// Convert a size into the minimum number of write units and then convert to number of ``uint64_t``.
#define LFTL_WU64(size) (SIZE64(LFTL_ROUND_UP(size,WU_SIZE)))

/// Convert a size into the minimum number of write units and then convert to number of ``lftl_dat_t``.
#define LFTL_WU_DAT(size) (SIZE_LFTL_DAT(LFTL_ROUND_UP(size,WU_SIZE)))


#define LFTL_PAGES(size) (LFTL_DIV_CEIL(size,FLASH_SW_PAGE_SIZE))

/// Declare an LFTL area
/// \param name Name of the LFTL area.
/// \param area_content Members of the LFTL area.
/// \param wear_leveling_factor Only integers are supported, minimum is 2.
#define LFTL_AREA(name, area_content, wear_leveling_factor) union {\
  flash_sw_page_t name##_pages[LFTL_PAGES(sizeof(struct {area_content})+LFTL_META_N_ITEMS*WU_SIZE)*(wear_leveling_factor)];\
  struct {area_content} name##_data;\
  struct {area_content};\
  struct {area_content} _##name##_data;\
  };

/// Macro to declare the wear leveling factor of an area
#define LFTL_WEAR_LEVELING_FACTOR(x) x

/// Legacy
#define LFTL_DATA(name, size) lftl_dat_t name[LFTL_WU_DAT(size)]

/// Declare raw data within an LFTL area.
#define LFTL_RAW_DATA(name, size) union {\
  lftl_dat_t name##_phy[LFTL_WU_DAT(size)];\
  uint8_t name[size];\
};

/// Declare a variable within an LFTL area.
#define LFTL_VAR(type, name) union {\
  lftl_dat_t name##_phy[LFTL_WU_DAT(sizeof(type))];\
  type name;\
};

/// Declare an array within an LFTL area.
#define LFTL_ARRAY(type, name, n_elem) union {\
  lftl_dat_t name##_phy[LFTL_WU_DAT(sizeof(type))*n_elem];\
  type name[n_elem];\
};

/// Declare compact within an LFTL area.
#define LFTL_COMPACT_ARRAY(type, name, n_elem) union {\
  lftl_dat_t name##_phy[LFTL_WU_DAT(sizeof(type)*n_elem)];\
  type name[n_elem];\
};

/// Read an entire LFTL area and increment destination buffer.
#define LFTL_READ_WHOLE_DATA(dst,area) do{\
    uint8_t*__dst8 = (uint8_t*)dst;\
    const size_t __size = sizeof(nvm. _## area ## _data);\
    lftl_read(&area,__dst8,&nvm. _## area ## _data,__size);\
    dst = __dst8+__size;\
  }while(0)

/// Write an entire LFTL area and increment destination buffer.
#define LFTL_WRITE_WHOLE_DATA(area,src) do{\
    uint8_t*__src8 = (uint8_t*)src;\
    const size_t __size = sizeof(nvm. _## area ## _data);\
    lftl_write(&area,&nvm. _## area ## _data,__src8,__size);\
    src = __src8+__size;\
  }while(0)

/// Set the same byte value on an entire LFTL area.
#define LFTL_MEMSET_WHOLE_AREA(area,value) do{ \
  uint8_t transaction_tracker[LFTL_TRANSACTION_TRACKER_SIZE(&area)]; \
  lftl_transaction_start(&area, transaction_tracker); \
  flash_sw_page_t buf; \
  memset(buf,value,sizeof(buf)); \
  const unsigned int n_loops = area.data_size / sizeof(buf); \
  uint32_t remaining = area.data_size; \
  flash_sw_page_t*wr_addr = area.data; \
  for(unsigned int i=0;i<n_loops;i++){ \
    lftl_transaction_write(&area,wr_addr,buf,sizeof(buf)); \
    remaining -= sizeof(buf); \
    wr_addr++; \
  } \
  lftl_transaction_write(&area,wr_addr,buf,remaining); \
  lftl_transaction_commit(&area); \
}while(0)

#endif
