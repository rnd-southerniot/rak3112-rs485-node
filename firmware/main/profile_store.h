/*
 * profile_store.h — NVS persistence for a device profile blob (ADR-006 increment 2b).
 *
 * The CRM pushes a serialized device_profile_t blob (dp_serialize format) to the node over the
 * provisioning console (`prov-profile`); it is stored in the 'prov' NVS namespace under key
 * "profile". The generic reader (increment 3) loads it at boot via profile_store_load().
 */
#ifndef PROFILE_STORE_H
#define PROFILE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_profile.h"

/* Largest profile blob we accept (DP_MAX_MEAS/FIELDS bound the worst case ~ 540 B). */
#define PROFILE_BLOB_MAX 600u

/*
 * Validate `blob` (dp_deserialize) then store it in NVS 'prov'/"profile". Returns false on an
 * invalid blob (bad version/CRC/bounds) or an NVS error — an invalid profile is never persisted.
 */
bool profile_store_save(const uint8_t *blob, size_t len);

/*
 * Load + deserialize the stored profile into `store`. Returns true only if a valid profile blob is
 * present in NVS; false if absent or corrupt (caller falls back to the compiled device).
 */
bool profile_store_load(dp_profile_storage_t *store);

/* True if a profile blob is present in NVS (does not validate it). For prov-show. */
bool profile_store_present(uint8_t *device_byte_out, uint8_t *n_meas_out);

#endif /* PROFILE_STORE_H */
