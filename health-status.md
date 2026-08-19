# Health Status
This outlines the concept to support the health status feature.

## Standard health status API
This is an API which assumes the bulk of erase operations are due to normal operations,
i.e., few tearing event happened and `lftl_*write` happened a lot.

If this assumption holds:
- for regular areas:
    - All erase units within one area are likely to have seen a similar number of erase.
    - That number of erase is meta data's `version` field divided by the number of erase units in that area.
- for EWLF areas:
    - All erase units within one area are likely to have seen a similar number of erase.
    - That number of erase is the meta data's `version` field divided by the number of erase units in that area and further divided by how many records are packed into one erase unit.

### API
`void lftl_health_status(lftl_ctx_t*ctx, uint32_t*n_erase)`

- `ctx` is the LFTL area context
- `n_erase` is the output: the number of erase that each erase unit is likely to have seen so far.


## Advanced health status API
Some system designers may not accept the assumption made by the standard health status.
In that case, they can use the advanced API. Note that this API is enabled at build time as it needs to track
flash operations from the start.

This API is enabled/disabled explicitely for each regular area. When enabled, it adds a system EWLF area to track the number of erase operations.
Each erase unit is associated with a 20 bit saturating counter.
Before each erase operation, the associated counter is updated.

Limitations:
- It may not be usable on Flash with small erase units such as 256 bytes and huge LFTL areas because a full tracking record is then likely to be bigger than a single erase unit, this is not supported by EWLF.
- The count can be pessimistic if tearing events happened after the tracking write but before the erase operation.
- The EWLF areas are covered using the standard health status assumptions.

### API
`void lftl_health_status_stats(lftl_ctx_t*ctx, health_status_stats_t*stats)`

- `ctx` is the LFTL area context
- `stats` is the output: it gives the min/average/max number of erase across the selected area as well as the physical index of the erase unit with the extreme values.

