/*! \file */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/// Value used for invalid pointers.
#define LFTL_INVALID_POINTER ((void*)-1)

#ifndef LFTL_WU_MAX_SIZE
  /// Largest write unit supported, in bytes: ::lftl_register_area raises
  /// ::LFTL_ERROR_WU_SIZE_TOO_LARGE above it (R2 of the "Flash requirements and
  /// assumptions" page). Define it before including this header if the NVM needs more.
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

/// Compute the size required for ``transaction_tracker`` of a large area:
/// one bit per write unit of the data, as for a regular area, then two bytes
/// per chunk (erase unit) of the data.
/// See ::lftl_transaction_start.
/// \param data_size   Size of the data in the target LFTL area, in bytes
/// \param write_size  Size of the minimum write unit in the NVM, in bytes
/// \param erase_size  Size of the erase unit in the NVM, in bytes
///
#define LFTL_TRANSACTION_TRACKER_SIZE_LARGE(data_size,write_size,erase_size) (\
  LFTL_TRANSACTION_TRACKER_SIZE_LL(data_size,write_size)\
  + 2*(((data_size)+(erase_size)-1)/(erase_size)))

/// Compute the size required for ``transaction_tracker``, for any type of area.
/// See ::lftl_transaction_start.
/// \param ctx  Pointer to lftl_ctx_t, registered
///
#define LFTL_TRANSACTION_TRACKER_SIZE(ctx) (\
  (LFTL_AREA_TYPE_LARGE == (ctx)->area_type)\
  ? LFTL_TRANSACTION_TRACKER_SIZE_LARGE((ctx)->data_size,(ctx)->nvm_props->write_size,(ctx)->nvm_props->erase_size)\
  : LFTL_TRANSACTION_TRACKER_SIZE_LL((ctx)->data_size,(ctx)->nvm_props->write_size))

/// @name Return codes
/// @{

/// Success. 
#define LFTL_SUCCESS 0x00
/// Internal error / corruption: area contains two or more valid slots with the same version
#define LFTL_ERROR_VERSION_COLLISION 0x01
/// Internal error / corruption: area does not contains any valid slot; on a large area, also a map entry that reads outside the pool after the scan (the flash changed behind the library)
#define LFTL_ERROR_NO_VALID_VERSION 0x02
/// Error: base address is not within the valid data address range for this area
#define LFTL_ERROR_FIRST_NOT_IN_DATA 0x03
/// Error: last address is not within the valid data address range for this area
#define LFTL_ERROR_LAST_NOT_IN_DATA 0x04
/// Error: base address is not aligned
#define LFTL_ERROR_BASE_MISALIGNED 0x05
/// Error: size is not aligned
#define LFTL_ERROR_SIZE_MISALIGNED 0x06
/// Error: a basic write, ::lftl_erase_all or ::lftl_transaction_start is attempted during a transaction
#define LFTL_ERROR_TRANSACTION_ONGOING 0x07
/// Error: a transaction API call is attempted outside of a transaction
#define LFTL_ERROR_NO_TRANSACTION 0x08
/// Error: the same address is written twice during a transaction
#define LFTL_ERROR_TRANSACTION_OVERWRITE 0x09
/// Error: the write unit size is too large, max is defined by LFTL_WU_MAX_SIZE
#define LFTL_ERROR_WU_SIZE_TOO_LARGE 0x0A
/// Error: a transaction API call is attempted on an EWLF area, which does not support transactions
#define LFTL_ERROR_EWLF_TRANSACTION_UNSUPPORTED 0x0B
/// Error: an area or its NVM is badly configured, see ::lftl_register_area, ::lftl_register_area_ewlf and ::lftl_register_area_large; or a call is made on a type of area it does not apply to
#define LFTL_ERROR_BAD_CONFIG 0x0C
/// Error: an EWLF or a large area is read or written while the repair its scan found is pending, see ::lftl_complete_scan
#define LFTL_ERROR_SCAN_PENDING 0x0D
/// Error: an EWLF or a large area is stuck: it has no room left to make a decision solid, see ::lftl_scan_status
#define LFTL_ERROR_STUCK 0x0E
/// Error: an update or a transaction of a large area needs more free erase units than its pool has; nothing was changed
#define LFTL_ERROR_LARGE_NO_SPACE 0x0F
/// Error: the data of a large area does not match its checksum, see ::lftl_check_data
#define LFTL_ERROR_DATA_CHECKSUM 0x10
/// Base value for errors reported by the erase function. Bits 0 to 7 may give more details.
#define LFTL_ERROR_LOW_LEVEL_ERASE 0x0100
/// Base value for error reported by the write function. Bits 0 to 7 may give more details.
#define LFTL_ERROR_LOW_LEVEL_WRITE 0x0200
/// Base value for error reported by the read function. Bits 0 to 7 may give more details.
#define LFTL_ERROR_LOW_LEVEL_READ 0x0300
/// Most likely a bug in LFTL or a hardware level issue
#define LFTL_INTERNAL_ERROR -1
/// @}

/// Status the ::nvm_write_t callback returns when the flash refused a program
/// and programmed nothing, because a target write unit is not erased (PROGERR
/// on the STM32L5 and STM32U5). An EWLF area steps over the write unit it was
/// refused; everywhere else, and for any other non-zero status, the error is
/// fatal (::LFTL_ERROR_LOW_LEVEL_WRITE).
#define LFTL_NVM_STATUS_REFUSED 6

#ifndef LFTL_EWLF_CERTIFICATES
  /// Default number of certificate write units in each log unit of an EWLF
  /// area, for the areas that leave ::lftl_ctx_struct::ewlf_certificates at 0:
  /// the number of consecutive log switch attempts, each cut inside the same
  /// two program windows, after which the area is stuck.
  #define LFTL_EWLF_CERTIFICATES 32
#endif

/// Number of log units of an EWLF area, on top of its data units.
#define LFTL_EWLF_LOG_UNITS 2

#ifndef LFTL_EWLF_RESERVED_PAIRS
  /// Number of reserved pair positions in each log unit of an EWLF area: the
  /// room a full log unit keeps to make a decision solid.
  #define LFTL_EWLF_RESERVED_PAIRS 8
#endif

/**
 * \brief Callback to erase physical NVM
 *
 * It erases one or more pages.
 * If `n_pages` = 0 it shall immediately return 0.
 *
 * The erased value it leaves is only constrained for EWLF areas, where erased
 * bytes must read 0xFF (R6 and N1 of the "Flash requirements and assumptions" page).
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
 * It is only ever asked to program erased write units, each at most once
 * between two erases, so it never needs to overwrite programmed cells
 * (R3 and N3 of the "Flash requirements and assumptions" page).
 *
 * When `size` covers several write units it shall program them one at a time,
 * from the lowest address up, each by a program operation of its own which
 * has ended before the next one starts (R10 of the same page): a power cut
 * then leaves at most one weak write unit, the ones below it programmed and
 * the ones above it untouched, the only case the anti-tearing results cover.
 *
 * When the flash refuses a program because a target write unit is not erased,
 * and programs nothing, it shall return ::LFTL_NVM_STATUS_REFUSED: a write
 * unit that a power cut left weak can read erased and still be refused, and
 * an EWLF area then steps over it.
 *
 * The destination range may cross multiple physical page boundaries.
 * 
 * The source may be within the NVM but is guaranteed to not overlap 
 * with the destination range.
 * 
 * \param dst_nvm_addr The start address of the destination. It is always aligned on the NVM's `write_size`.
 * \param src The start address of the source. It may be misaligned if top level API are called with misaligned source address.
 * \param size The size in bytes of the data to write. It is always a multiple of the NVM's `write_size`.
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
 * On a flash where reading a write unit that a power cut left weak can fault
 * (an ECC error), an EWLF area needs the callback to survive the fault and
 * to fill that write unit with a value that is neither all 0xFF nor all
 * 0x00: the unit then counts as started and proves nothing.
 * 
 * \param dst The start address of the destination
 * \param src_nvm_addr The start address of the source
 * \param size The size in bytes of the data to read
 *
 * \returns 0 or an error code
 */
typedef uint8_t (*nvm_read_t)(void* dst, const void*const src_nvm_addr, uintptr_t size);

/**
 * \brief Callback for error handling
 *
 * It is called to handle fatal errors, it shall not return.
 * If it returns, the library loops forever.
 *
 * Before it is called, the transaction of the area the error happened on,
 * if any, is aborted: a recovery path must start a new transaction.
 * Transactions on other areas are left as they are.
 *
 * A typical implementation will longjump.
 * 
 * \param err_code The error code
 */
typedef void (*error_handler_t)(uint32_t err_code);

/** @struct lftl_nvm_props_struct
 *  Properties of the physical NVM
 *
 *  \rst
 *  What the NVM must provide, and what lean-ftl does not rely on: :ref:`flash`.
 *  \endrst
 */
typedef struct lftl_nvm_props_struct {
  void*base;            /**< the base address of the entire NVM */
  uintptr_t size;       /**< the size of the entire NVM (used to select between direct access and nvm_read_t) */
  uint32_t write_size;  /**< at least what the NVM is supporting, or a multiple of it: a power of two dividing `erase_size`, at most ::LFTL_WU_MAX_SIZE. On flash with ECC, at least the ECC codeword, so that each meta-data item of a regular area, and each marker, confirm and certificate of an EWLF area, is its own codeword (R2 of the "Flash requirements and assumptions" page) */
  uint32_t erase_size;  /**< at least what the NVM is supporting, or a multiple of it; an area is whole erase units (R1 of the "Flash requirements and assumptions" page) */
  bool zero_overwrite;  /**< true if the NVM accepts programming a write unit to all zeros whatever it currently holds, as the STM32L5 and STM32U5 document (N3 of the "Flash requirements and assumptions" page): lean-ftl then pins, at the first access to a regular area after a reset, the word a power cut left weak, instead of leaving the decision to be made again at the next boot. false, the value of a zeroed initializer, when the NVM does not or when it is unknown: lean-ftl then never programs a unit that does not read erased. EWLF areas do not use it: they only program write units that read erased, and step over one the flash refuses (::LFTL_NVM_STATUS_REFUSED). */
  nvm_erase_t erase;    /**< Erase function for this area. */
  nvm_write_t write;    /**< Write function for this area. */
  nvm_read_t read;      /**< Read function for this area. */
  error_handler_t error_handler;  /**< Error handler function for this area. */
  void *next;                     /**< Initialize it ::LFTL_INVALID_POINTER. */
} lftl_nvm_props_t;

/// Type of area registered with ::lftl_register_area, ::lftl_register_area_ewlf or ::lftl_register_area_large.
typedef enum {
  LFTL_AREA_TYPE_STD = 0,
  LFTL_AREA_TYPE_EWLF = 1,
  LFTL_AREA_TYPE_LARGE = 2,
} lftl_area_type_t;

/// What the scan of an EWLF or a large area found left to do, see ::lftl_scan_status.
typedef enum {
  LFTL_REPAIR_NONE = 0,   /**< nothing: the area is readable and writable */
  LFTL_REPAIR_BURY = 1,   /**< a copy of the decision in the live log unit: one pair */
  LFTL_REPAIR_CARRY = 2,  /**< a copy of the decision in the other log unit, already erased: one pair */
  LFTL_REPAIR_SWITCH = 3, /**< a log switch: at most one reserved pair, one erase, one certificate, one pair */
} lftl_repair_t;

/// State of an area after its scan, see ::lftl_scan_status.
typedef struct {
  uint32_t version;        /**< version of the data the area shows, or will show once the pending repair is done */
  uint16_t remaining_cuts; /**< the smallest number of further power cuts, each at the worst instant, after which the area is stuck; 0 when it is */
  uint8_t pending;         /**< a ::lftl_repair_t: the repair ::lftl_complete_scan will do */
  bool stuck;              /**< the area is stuck: only ::lftl_read_unproven and ::lftl_format work */
} lftl_scan_status_t;

/// What one reading of a log unit of an EWLF area gave. Internal.
typedef struct {
  int16_t started;      /**< last started pair position, -1 if none */
  int16_t newest;       /**< position of the newest valid entry, -1 if none */
  uint16_t gen;         /**< generation of the unit, meaningful when `newest` is not -1 */
  uint16_t certs_free;  /**< certificate write units reading erased */
  uint8_t flags;        /**< proven, closed, certified, newest entry confirmed */
} lftl_ewlf_log_unit_t;

/// What an EWLF or a large area knows about its log since its scan. Internal.
typedef struct {
  lftl_ewlf_log_unit_t unit[2];
  uint32_t version;     /**< version of the decision */
  int16_t rec_started;  /**< EWLF: last started record position in the decision's data unit, -2 until read */
  uint8_t d_unit;       /**< EWLF: data unit of the decision's record; large: log unit of the decision's pair */
  uint8_t d_pos;        /**< EWLF: position of the decision's record in its unit */
  uint8_t live;         /**< index of the live log unit */
  uint8_t pending;      /**< a ::lftl_repair_t */
  uint8_t stuck;        /**< no room for the pending repair, or a write found none */
  uint8_t data_check;   /**< large: 0 data not checked since the scan, 1 sound, 2 corrupt */
  int16_t d_q;          /**< large: position of the decision's pair in its log unit */
  uint16_t head;        /**< large: the pool unit filled last, 0xFFFF before the first */
} lftl_ewlf_state_t;

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
  void *transaction_tracker;      /**< Initialize it ::LFTL_INVALID_POINTER. */
  void *next;                     /**< Initialize it ::LFTL_INVALID_POINTER. */
  lftl_area_type_t area_type;     /**< Set by ::lftl_register_area, ::lftl_register_area_ewlf or ::lftl_register_area_large. Do not set manually. */
  uint16_t ewlf_certificates;     /**< EWLF and large areas only: number of certificate write units in each log unit, 0 (the value a zeroed or shorter initializer leaves) for ::LFTL_EWLF_CERTIFICATES. Part of the on-flash format of the area, like `data_size`: an area whose value changes must be formatted again. */
  lftl_ewlf_state_t ewlf;         /**< EWLF and large areas only: internal, needs no initialization. */
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
/// Release versions have the format `X.Y.Z` (the git tag is `vX.Y.Z`)
/// where `X` is `MAJOR`, `Y` is `MINOR` and `Z` is `PATCH`.
/// Other builds append `git describe`'s `-N-gHASH`, `-dirty` when the
/// working tree has uncommitted changes, and `-untracked` when it has
/// untracked files.
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
/// When a build is "dirty" (uncommitted changes)
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
/// It returns the CMake build type; release packages contain:
///
///   - Debug
///   - MinSizeRel
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
/// Register an LFTL area. This allows to 
/// be able to copy data from the LFTL area into 
/// another by using the write functions, i.e., without
/// intermediate buffering.
/// Call this for each area, after ::lftl_init_lib
/// and before any other function (except the Meta information API group).
///
/// The configuration is checked first, and the area is not registered if
/// the check fails: ::LFTL_ERROR_WU_SIZE_TOO_LARGE if the NVM's write unit
/// is larger than ::LFTL_WU_MAX_SIZE, ::LFTL_ERROR_BAD_CONFIG if the write
/// unit is not a power of 2 dividing the erase unit, if the area is not
/// whole erase units within its NVM, if its data is not whole write units,
/// or if it holds less than 2 slots.
/// \param ctx Context of the LFTL area to register
///
////////////////////////////////////////////////////////////
void lftl_register_area(lftl_ctx_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Register a physical NVM
///
/// Registers the properties of a physical NVM so that addresses within
/// it can be recognized even outside of any LFTL area, e.g. by
/// ::lftl_memread and ::lftl_memread_newer.
/// This is called automatically by ::lftl_register_area and
/// ::lftl_register_area_ewlf for their own NVM, so it only needs to be
/// called directly to register a physical NVM that is not (yet) backing
/// any LFTL area.
/// Registering an NVM whose base address is already covered by a
/// previously registered NVM is a no-op.
/// \param ctx Properties of the physical NVM to register
///
////////////////////////////////////////////////////////////
void lftl_register_nvm(lftl_nvm_props_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Register an LFTL EWLF area
///
/// Register an LFTL EWLF area. This allows to 
/// be able to copy data from the LFTL area into 
/// another by using the write functions, i.e., without
/// intermediate buffering.
/// Call this for each area, after ::lftl_init_lib
/// and before any other function (except the Meta information API group).
///
/// The configuration is checked first, as for ::lftl_register_area, except
/// that an EWLF area needs at least 4 erase units, 2 for its log and from 2
/// to 256 for its data, that at least 2 records (the data, a start marker and
/// a meta field each, see ::LFTL_EWLF_RECORD_SIZE) must fit in one erase unit,
/// and that a log unit must hold its certificates
/// (::lftl_ctx_struct::ewlf_certificates), ::LFTL_EWLF_RESERVED_PAIRS reserved
/// pairs and at least 2 regular ones. The NVM's erased bytes must read 0xFF
/// (R6 of the "Flash requirements and assumptions" page). The on-flash format
/// of EWLF areas changed in v0.4.0: an area written by an earlier version is
/// not read and must be formatted again.
///
/// Nothing is read here. The first call that needs the area's content scans
/// it, reading only: ::lftl_scan_status, ::lftl_complete_scan,
/// ::lftl_read_unproven, or a read or a write. After a reset, set
/// `ctx->data` to ::LFTL_INVALID_POINTER or register again.
/// \param ctx Context of the LFTL area to register
///
////////////////////////////////////////////////////////////
void lftl_register_area_ewlf(lftl_ctx_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Register an LFTL large area
///
/// A large area cuts its data into chunks of one erase unit each and keeps
/// them in a pool of erase units; a map of where each chunk is, with a
/// CRC-64 of the whole data, is the data of the pairs of an EWLF log. An
/// update rewrites only the chunks it touches, a transaction is atomic
/// whatever the number of chunks it touches, and a power cut is never
/// followed by a rollback (LARGE.md). Declare it with ::LFTL_AREA_LARGE: the
/// pool units, then 2 log units.
///
/// The configuration is checked first, as for ::lftl_register_area, except
/// that the pool must hold the data and one more erase unit, at most 65535
/// erase units, and that a log unit must hold its certificates
/// (::lftl_ctx_struct::ewlf_certificates), ::LFTL_EWLF_RESERVED_PAIRS reserved
/// pairs and at least 2 regular ones, a pair holding the map. The NVM's
/// erased bytes must read 0xFF and its write callback must report a refused
/// program (R6 and R9 of the "Flash requirements and assumptions" page).
///
/// Nothing is read here. The first call that needs the area's content scans
/// it, as for an EWLF area; the first call that uses its data checks the
/// data against its checksum, unless ::lftl_check_data did. After a reset,
/// set `ctx->data` to ::LFTL_INVALID_POINTER or register again.
/// \param ctx Context of the LFTL area to register
///
////////////////////////////////////////////////////////////
void lftl_register_area_large(lftl_ctx_t*ctx);

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
/// \brief Retrieve the std LFTL context correponding to an
/// address, if any.
///
/// Only searches areas registered with ::lftl_register_area.
/// See ::lftl_get_ctx to search all area types at once.
///
/// \param addr Address to search
/// \returns a valid LFTL context or LFTL_INVALID_POINTER
////////////////////////////////////////////////////////////
lftl_ctx_t*lftl_get_ctx_std(const void*const addr);

////////////////////////////////////////////////////////////
/// \brief Retrieve the EWLF LFTL context correponding to an
/// address, if any.
///
/// Only searches areas registered with ::lftl_register_area_ewlf.
/// See ::lftl_get_ctx to search all area types at once.
///
/// \param addr Address to search
/// \returns a valid LFTL context or LFTL_INVALID_POINTER
////////////////////////////////////////////////////////////
lftl_ctx_t*lftl_get_ctx_ewlf(const void*const addr);

////////////////////////////////////////////////////////////
/// \brief Retrieve the large LFTL context correponding to an
/// address, if any.
///
/// Only searches areas registered with ::lftl_register_area_large.
/// See ::lftl_get_ctx to search all area types at once.
///
/// \param addr Address to search
/// \returns a valid LFTL context or LFTL_INVALID_POINTER
////////////////////////////////////////////////////////////
lftl_ctx_t*lftl_get_ctx_large(const void*const addr);

////////////////////////////////////////////////////////////
/// \brief Retrieve the LFTL context correponding to an
/// address, if any.
///
/// Searches areas of every type: std (::lftl_register_area), then
/// EWLF (::lftl_register_area_ewlf), then large
/// (::lftl_register_area_large). See ::lftl_get_ctx_std,
/// ::lftl_get_ctx_ewlf and ::lftl_get_ctx_large to search only one area type.
///
/// \param addr Address to search
/// \returns a valid LFTL context or LFTL_INVALID_POINTER
////////////////////////////////////////////////////////////
lftl_ctx_t*lftl_get_ctx(const void*const addr);

////////////////////////////////////////////////////////////
/// \brief Indicates if the LFTL context is an EWLF area.
///
/// \param ctx A valid LFTL context
/// \returns 0 or 1
////////////////////////////////////////////////////////////
bool lftl_is_ewlf(const lftl_ctx_t*const ctx);

////////////////////////////////////////////////////////////
/// \brief Indicates if the LFTL context is a large area.
///
/// \param ctx A valid LFTL context
/// \returns 0 or 1
////////////////////////////////////////////////////////////
bool lftl_is_large(const lftl_ctx_t*const ctx);

////////////////////////////////////////////////////////////
/// \brief State of an area after its scan: remaining cuts, pending repair
///
/// For an EWLF or a large area: scans the area if nothing did since the
/// reset. The scan reads the two log units once and checks one record or
/// one map, and programs or erases nothing. When the last session was cut,
/// or when a write unit that an older cut left weak reads differently, the
/// scan may find a repair to do before the area can be shown:
/// `status->pending` says which, and ::lftl_complete_scan does it. Until
/// then ::lftl_read and the write functions raise ::LFTL_ERROR_SCAN_PENDING
/// on this area.
///
/// A regular area has no log: its scan locates its current slot, nothing
/// is ever pending, it is never stuck, and `status->remaining_cuts` is
/// 0xFFFF.
///
/// `status->remaining_cuts` is the number of further power cuts, each at the
/// worst instant, that the area can take before it is stuck. It is back to
/// the number of certificates after a log switch that completed. The
/// application applies the threshold it wants: before a write, or after a
/// reset before calling ::lftl_complete_scan, for instance to ask the
/// user to move to a stable power supply first.
///
/// Raises ::LFTL_ERROR_NO_VALID_VERSION if the area holds no version.
/// \param ctx Context of the area
/// \param status Output: the state of the area
////////////////////////////////////////////////////////////
void lftl_scan_status(lftl_ctx_t*ctx, lftl_scan_status_t*status);

////////////////////////////////////////////////////////////
/// \brief Do the repair the scan of an area found pending
///
/// Scans the area if nothing did since the reset. Does nothing more when no
/// repair is pending: it can be called unconditionally after a reset, on an
/// area of any type (a regular area never has one).
/// Otherwise programs a copy of the decision above everything a power cut
/// may have left, from the readings of the scan, and the area is readable
/// and writable once it returns.
///
/// Raises ::LFTL_ERROR_STUCK, with nothing programmed, when the area has
/// no room left for the repair, and ::LFTL_ERROR_NO_VALID_VERSION if the
/// area holds no version.
/// \param ctx Context of the area
////////////////////////////////////////////////////////////
void lftl_complete_scan(lftl_ctx_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Read an area whatever its state: pending repair or stuck
///
/// Like ::lftl_read when no repair is pending, and always on a regular
/// area. With one pending, or with the area stuck, reads the data the scan
/// chose: the one ::lftl_complete_scan makes current when called in the
/// same session. On a large area the data is checked against its checksum
/// first, as by ::lftl_read; ::lftl_read_unchecked reads without the check.
/// Its content is a version the application wrote; what is not proven is
/// that a later boot chooses the same one, if this session ends before the
/// repair is done: the other candidate is the version written just before
/// or just after it.
///
/// An application that uses it to decide whether to go on must keep the
/// state of that decision in another area: this one accepts no write
/// before the repair.
/// \param ctx Context of the area
/// \param dst Destination address (in volatile memory)
/// \param src_nvm_addr Source logical address
/// \param size Size in bytes of the data to read
////////////////////////////////////////////////////////////
void lftl_read_unproven(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size);

////////////////////////////////////////////////////////////
/// \brief Check the data of an area against its checksum
///
/// Optional. Reads the whole data, computes its CRC-64 and compares it with
/// the one the area keeps; the result holds until the next reset, or the
/// next call. When the application does not call it, the check is done
/// once per reset anyway, before the first call that uses the data the
/// area holds: a read, a write that does not replace the whole data, the
/// start of a transaction, the read of a pending area by
/// ::lftl_read_unproven. Calling it is how the application chooses when
/// the check takes its time (about 0.7 s per megabyte on the STM32U5A5).
///
/// Works while a repair is pending or the area is stuck: it checks the data
/// the scan chose. A mismatch is reported by the return value, not by an
/// error; from then on the calls that use the data raise
/// ::LFTL_ERROR_DATA_CHECKSUM, until a write of the whole data,
/// ::lftl_erase_all or ::lftl_repair_checksum. ::lftl_read_unchecked reads
/// the data whatever its checksum.
///
/// Large areas only: raises ::LFTL_ERROR_BAD_CONFIG on another type.
/// \param ctx Context of the area
/// \returns true when the data matches its checksum
////////////////////////////////////////////////////////////
bool lftl_check_data(lftl_ctx_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Read the data of an area without checking it against its checksum
///
/// Like ::lftl_read_unproven, but runs no check and needs none: it reads
/// what the area holds in a sound area, in a corrupt one, in one not
/// checked yet, while a repair is pending and when the area is stuck. It is
/// how an application saves what is still right in an area whose data
/// fails its checksum, before ::lftl_repair_checksum or a write of the
/// whole data.
///
/// Large areas only: raises ::LFTL_ERROR_BAD_CONFIG on another type.
/// \param ctx Context of the area
/// \param dst Destination address (in volatile memory)
/// \param src_nvm_addr Source logical address
/// \param size Size in bytes of the data to read
////////////////////////////////////////////////////////////
void lftl_read_unchecked(lftl_ctx_t*ctx, void*dst, const void*const src_nvm_addr, uintptr_t size);

////////////////////////////////////////////////////////////
/// \brief Make the checksum of an area that of its data as it reads now
///
/// Computes the CRC-64 of the data as the area reads it and commits it,
/// with the data unchanged: one atomic commit, after which the area is
/// sound and its chunks can be repaired by ordinary writes, one at a time,
/// with no need to hold the whole data elsewhere. It does not make the data
/// right, it declares it accepted: it is for an application that has looked
/// at it with ::lftl_read_unchecked. Commits nothing when the checksum
/// already matches.
///
/// Needs an area with no repair pending, and no transaction open
/// (::LFTL_ERROR_TRANSACTION_ONGOING). Large areas only: raises
/// ::LFTL_ERROR_BAD_CONFIG on another type.
/// \param ctx Context of the area
////////////////////////////////////////////////////////////
void lftl_repair_checksum(lftl_ctx_t*ctx);

////////////////////////////////////////////////////////////
/// \brief Erase all the data of an LFTL area
///
/// Note that the erasure is 'logical'. At physical level, 
/// previous version of the data may still remain.
/// See ::lftl_format for erasing all data at physical level.
///
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
/// Performance considerations: on a regular area this function erase one
/// slot. On a large area it erases nothing: each chunk the transaction
/// touches gets a free erase unit, erased at its first write; its size is
/// bounded by the free units of the pool (::LFTL_ERROR_LARGE_NO_SPACE).
///
/// \param dst_ctx Context of the target LFTL area
/// \param transaction_tracker Volatile buffer, see ::LFTL_TRANSACTION_TRACKER_SIZE
////////////////////////////////////////////////////////////
void lftl_transaction_start(lftl_ctx_t*dst_ctx, void *const transaction_tracker);

////////////////////////////////////////////////////////////
/// \brief Commit a transaction
///
/// After this call, the transaction_tracker buffer can be discarded.
///
/// Performance considerations: this function writes at most
/// one slot + meta data.
///
/// \param dst_ctx Context of the target LFTL area
////////////////////////////////////////////////////////////
void lftl_transaction_commit(lftl_ctx_t*dst_ctx);

////////////////////////////////////////////////////////////
/// \brief Abort a transaction
///
/// After this call, the transaction_tracker buffer can be discarded.
/// \param dst_ctx Context of the target LFTL area
////////////////////////////////////////////////////////////
void lftl_transaction_abort(lftl_ctx_t*dst_ctx);

////////////////////////////////////////////////////////////
/// \brief Write aligned data to NVM
///
/// This function detects if the write is part of a transaction 
/// or not.
/// \param dst_ctx          Context of the target LFTL area
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area, MUST be aligned on a write unit (LFTL_WU_SIZE)
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes, MUST be multiple of write unit size (LFTL_WU_SIZE)
////////////////////////////////////////////////////////////
void lftl_write(lftl_ctx_t*dst_ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

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
/// \param src          Source address
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
/// \param src          Source address
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
/// \param dst_ctx      LFTL area context
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes
///
////////////////////////////////////////////////////////////
void lftl_basic_write(lftl_ctx_t*dst_ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

////////////////////////////////////////////////////////////
/// \brief Write aligned data to NVM in an LFTL area
///
/// Update partially or totally an LFTL area. 
/// This function is allowed only when a transaction is on-going.
///
/// Performance considerations: 
/// Each call writes the specified number of bytes to NVM.
///
/// \param dst_ctx      LFTL area context
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area, MUST be aligned on a write unit (LFTL_WU_SIZE)
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes, MUST be multiple of write unit size (LFTL_WU_SIZE)
///
////////////////////////////////////////////////////////////
void lftl_transaction_write(lftl_ctx_t*dst_ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

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
 * Functions in this group are useful but can surprise during a transaction.
 * They remove restrictions on data alignement and size
 * of ::lftl_write and ::lftl_transaction_write. During a transaction, each
 * write unit can still be written only once: two writes that share a write
 * unit, even at different bytes, raise ::LFTL_ERROR_TRANSACTION_OVERWRITE,
 * which aborts the transaction.
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
/// \param dst_ctx      Context of the target LFTL area
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes
////////////////////////////////////////////////////////////
void lftl_write_any(lftl_ctx_t*dst_ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

////////////////////////////////////////////////////////////
/// \brief Write aligned or unaligned data to NVM in an LFTL area
///
/// Update partially or totally an LFTL area. 
/// This function is allowed only when a transaction is on-going.
///
/// Performance considerations: 
/// Each call writes the specified number of bytes to NVM.
///
/// \param dst_ctx      LFTL area context
/// \param dst_nvm_addr Destination address, it MUST be within the target LFTL area
/// \param src          Source address, if it is in NVM, it MUST be in the same LFTL area as ctx or outside of any LFTL area
/// \param size         Size in bytes
///
////////////////////////////////////////////////////////////
void lftl_transaction_write_any(lftl_ctx_t*dst_ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);

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


#define LFTL_MAX(a,b) ( (a) > (b) ? (a) : (b) )

#ifdef LFTL_DEFINE_HELPERS
//helpers to declare LFTL areas
//assumes they are all within a variable named "nvm"
//example of declaration:
//typedef struct data_flash_struct {
//  LFTL_AREA(my_area,
//    uint64_t my_nvm_array_var[SIZE64(1024)];
//    LFTL_DATA(my_nvm_uint64_var,8);
//    //...more variables...
//    ,2)//wear leveling factor: the macro reserves that many slots
//  //more LFTL_AREA can be declared here
//} __attribute__ ((aligned (LFTL_PAGE_SIZE))) data_flash_t;
//data_flash_t nvm __attribute__ ((section (".data_flash")));

//LFTL_WU_SIZE shall be the size of write unit, in bytes
//LFTL_PAGE_SIZE shall be the size of the page unit, in bytes, or a multiple of it

#ifndef STR
  #define STR_INNER(x) # x
  #define STR(x) STR_INNER(x)
#endif

#ifndef CONCAT
  #define CONCAT_INNER(x,y) x ## y
  #define CONCAT(x,y) CONCAT_INNER(x,y)
#endif

#ifndef BYTES_TO_BITS_1
  #define BYTES_TO_BITS_1 8
  #define BYTES_TO_BITS_2 16
  #define BYTES_TO_BITS_4 32
  #define BYTES_TO_BITS_8 64
#endif

#if LFTL_WU_SIZE > 8
  #define LFTL_DAT_TYPE_WIDTH 64
  #define DAT_PER_WU (SIZE64(LFTL_WU_SIZE))
  #define SIZE_LFTL_DAT(size) SIZE64(size)
#else
  #define LFTL_DAT_TYPE_SIZE LFTL_WU_SIZE

  #define LFTL_DAT_TYPE_WIDTH CONCAT(BYTES_TO_BITS_ , LFTL_DAT_TYPE_SIZE)
  #define DAT_PER_WU 1
  #define SIZE_LFTL_DAT(size) (((size)+LFTL_DAT_TYPE_SIZE-1)/LFTL_DAT_TYPE_SIZE)
#endif

#define LFTL_DAT_TYPE_BUILDER(VAL) uint ## VAL ## _t
#define LFTL_DAT_TYPE_BUILDER2(VAL) LFTL_DAT_TYPE_BUILDER(VAL)
#define LFTL_DAT_TYPE LFTL_DAT_TYPE_BUILDER2(LFTL_DAT_TYPE_WIDTH)

typedef LFTL_DAT_TYPE lftl_dat_t;

/** @struct lftl_wu_struct
 *  Struct which has the same size as a NVM write unit.
 *  It is used to defined the type `lftl_wu_t`.
 * 
 *  `lftl_wu_t` has the same size and the same alignement as a NVM write unit.
 *
 */
typedef struct lftl_wu_struct {
  lftl_dat_t dat[DAT_PER_WU];
} __attribute__ ((aligned (sizeof(lftl_dat_t)))) lftl_wu_t;

typedef lftl_dat_t __attribute__ ((aligned (LFTL_PAGE_SIZE))) flash_sw_page_t[SIZE_LFTL_DAT(LFTL_PAGE_SIZE)];

/// Round a value to the minimum number of multiples of a unit.
#define LFTL_ROUND_UP(val,unit) (LFTL_DIV_CEIL(val,unit)*(unit))

/// Convert a size into the minimum number of write units and then convert to number of ``uint64_t``.
#define LFTL_WU64(size) (SIZE64(LFTL_ROUND_UP(size,LFTL_WU_SIZE)))

/// Convert a size into the minimum number of write units and then convert to number of ``lftl_dat_t``.
#define LFTL_WU_DAT(size) (SIZE_LFTL_DAT(LFTL_ROUND_UP(size,LFTL_WU_SIZE)))


#define LFTL_PAGES(size) (LFTL_DIV_CEIL(size,LFTL_PAGE_SIZE))

/// Size in bytes of one meta-data item (version, checksum, checksum2), as laid
/// out at run time: one write unit, but at least sizeof(uint32_t).
#define LFTL_META_ITEM_SIZE ((LFTL_WU_SIZE) > 4 ? (LFTL_WU_SIZE) : 4)

/// Declare an LFTL area
/// \param name Name of the LFTL area.
/// \param area_content Members of the LFTL area.
/// \param wear_leveling_factor Only integers are supported, minimum is 2.
#define LFTL_AREA(name, area_content, wear_leveling_factor) union {\
  flash_sw_page_t name##_pages[LFTL_PAGES(sizeof(struct {area_content})+LFTL_META_N_ITEMS*LFTL_META_ITEM_SIZE)*(wear_leveling_factor)];\
  struct {area_content} name##_data;\
  struct {area_content};\
  struct {area_content} _##name##_data;\
  };

/// Size in bytes of an EWLF record's start marker, as laid out at run time:
/// one write unit.
#define LFTL_EWLF_FLAG_SIZE (LFTL_WU_SIZE)

/// Size in bytes of an EWLF record's meta field (a 32-bit version and a
/// 64-bit checksum), as laid out at run time: 12 bytes rounded up to whole
/// write units.
#define LFTL_EWLF_META_SIZE LFTL_ROUND_UP(12,LFTL_WU_SIZE)

#define LFTL_EWLF_RECORDS_PER_PAGE(record_size) (LFTL_PAGE_SIZE/(record_size))
/// Size in bytes of one EWLF record of `area_content`, as laid out at run time:
/// the start marker, the data rounded up to whole write units, then the meta
/// field.
#define LFTL_EWLF_RECORD_SIZE(area_content) (\
  LFTL_EWLF_FLAG_SIZE + LFTL_EWLF_META_SIZE \
  + LFTL_ROUND_UP(sizeof(struct {area_content}),LFTL_WU_SIZE))

/// Declare an LFTL area with Extreme Wear Leveling Factor: the data units
/// that hold `wear_leveling_factor` records, 2 at least, plus the
/// ::LFTL_EWLF_LOG_UNITS log units. The log units bound the factor the area
/// reaches whatever the number of data units: twice the regular pairs of a
/// log unit.
/// \param name Name of the LFTL area.
/// \param area_content Members of the LFTL area.
/// \param wear_leveling_factor Only integers are supported, minimum is 2.
#define LFTL_AREA_EWLF(name, area_content, wear_leveling_factor) union {\
  _Static_assert(LFTL_EWLF_RECORDS_PER_PAGE(LFTL_EWLF_RECORD_SIZE(area_content)) >= 2, "EWLF area " #name ": at least 2 records must fit in a page");\
  flash_sw_page_t name##_pages[LFTL_EWLF_LOG_UNITS + LFTL_MAX(2,LFTL_DIV_CEIL(wear_leveling_factor,LFTL_EWLF_RECORDS_PER_PAGE(LFTL_EWLF_RECORD_SIZE(area_content))))];\
  struct {area_content} name##_data;\
  struct {area_content};\
  struct {area_content} _##name##_data;\
  };

/// Number of chunks of a large area of `area_content`: one per erase unit
/// of its data.
#define LFTL_LARGE_CHUNKS(area_content) LFTL_PAGES(sizeof(struct {area_content}))

/// Declare a large area: `pool_pages` erase units that hold the chunks of
/// the data, one erase unit each, then the ::LFTL_EWLF_LOG_UNITS log units.
/// The pool needs the chunks of the data and one more erase unit at least;
/// to allow a transaction or an update of `k` chunks, the chunks plus `k`;
/// for wear, the chunks never rewritten plus the chunk writes over the life
/// of the area divided by the endurance of an erase unit (LARGE.md, "Sizing").
/// \param name Name of the LFTL area.
/// \param area_content Members of the LFTL area.
/// \param pool_pages Number of erase units of the pool.
#define LFTL_AREA_LARGE(name, area_content, pool_pages) union {\
  _Static_assert((pool_pages) >= LFTL_LARGE_CHUNKS(area_content) + 1, "large area " #name ": the pool must hold the data and one more page");\
  flash_sw_page_t name##_pages[(pool_pages) + LFTL_EWLF_LOG_UNITS];\
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

/// Set the same byte value on an entire LFTL area (regular areas only: it uses a transaction).
#define LFTL_MEMSET_WHOLE_AREA(ctx,value) do{ \
  uint8_t transaction_tracker[LFTL_TRANSACTION_TRACKER_SIZE(&ctx)]; \
  lftl_transaction_start(&ctx, transaction_tracker); \
  flash_sw_page_t buf; \
  memset(buf,value,sizeof(buf)); \
  const unsigned int n_loops = ctx.data_size / sizeof(buf); \
  uint32_t remaining = ctx.data_size; \
  flash_sw_page_t*wr_addr = ctx.area; /*logical addresses: ctx.data is the current slot*/ \
  for(unsigned int i=0;i<n_loops;i++){ \
    lftl_transaction_write(&ctx,wr_addr,buf,sizeof(buf)); \
    remaining -= sizeof(buf); \
    wr_addr++; \
  } \
  if(remaining) lftl_transaction_write(&ctx,wr_addr,buf,remaining); \
  lftl_transaction_commit(&ctx); \
}while(0)

#endif
