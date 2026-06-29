/*
 * device_profile.h — data-driven device model: a profile (bus + measurand table + payload table)
 * that one generic Modbus reader and one generic ADR-005 encoder consume at runtime (ADR-006).
 *
 * This header + device_profile.c are PURE C (no SDK) so the numeric core builds host-side under
 * ctest. The on-target reader (increment 3) walks `meas[]` via modbus_master_read + dp_decode();
 * the NVS loader (increment 2) populates a device_profile_t from a provisioned blob.
 */
#ifndef DEVICE_PROFILE_H
#define DEVICE_PROFILE_H

#include <stddef.h>
#include <stdint.h>

/* Modbus register data type for a measurand. */
typedef enum {
    DP_U16 = 0,
    DP_I16,
    DP_U32,
    DP_I32,
    DP_F32,
} dp_dtype_t;

/* Word/byte order for 32-bit values across two registers. regs[0] is the lower wire address.
 * Bytes are named A B C D from the most-significant on the wire: regs[0]=A B, regs[1]=C D. */
typedef enum {
    DP_ABCD = 0, /* big-endian: value = A B C D */
    DP_CDAB,     /* word-swapped: value = C D A B (Selec MFM384) */
    DP_BADC,     /* byte-swapped within words: value = B A D C */
    DP_DCBA,     /* fully reversed: value = D C B A */
} dp_word_t;

/* Fixed-point encoding of a payload field on the wire. */
typedef enum {
    DP_ENC_U8 = 0,
    DP_ENC_I8,
    DP_ENC_U16,
    DP_ENC_I16,
    DP_ENC_U32,
    DP_ENC_I32,
} dp_enc_t;

/* One measurand read from the device. engineering_value = raw * scale + offset. */
typedef struct {
    uint16_t reg;    /* wire (0-based) start address */
    uint8_t fc;      /* 3 = holding, 4 = input registers */
    dp_dtype_t type; /* register data type */
    dp_word_t word;  /* word order (ignored for 16-bit types) */
    float scale;     /* raw -> engineering multiplier */
    float offset;    /* raw -> engineering offset */
} dp_measurand_t;

/* One field placed into the ADR-005 payload body. wire = round(values[value_index] * scale). */
typedef struct {
    uint8_t value_index; /* index into the measurand/values array */
    uint8_t offset;      /* byte offset within the body (after the 3-byte header) */
    dp_enc_t enc;        /* fixed-point wire encoding */
    float scale;         /* engineering -> wire multiplier */
} dp_field_t;

/* A complete device profile. Slave/unit ID is intentionally absent (discovered by scan). */
typedef struct {
    uint8_t device_byte; /* ADR-005 payload device-type byte */
    uint32_t baud;       /* bus baud rate */
    char parity;         /* 'N' | 'E' | 'O' */
    uint8_t stop_bits;   /* 1 | 2 */
    uint8_t default_fc;  /* function code if a measurand leaves fc unset */
    dp_word_t default_word;
    uint8_t scan_fc; /* slave-ID discovery probe */
    uint16_t scan_reg;
    uint16_t scan_qty;
    uint8_t n_meas;
    const dp_measurand_t *meas;
    uint8_t total_len; /* full payload length incl. 3-byte header (<= 53) */
    uint8_t n_fields;
    const dp_field_t *fields;
} device_profile_t;

/* Registers consumed by a data type: 1 for 16-bit, 2 for 32-bit/float. */
uint8_t dp_dtype_regs(dp_dtype_t type);

/*
 * Decode `regs` (each already host-endian, big-endian-extracted by modbus_parse_read_response) into
 * an engineering float. `regs` must hold dp_dtype_regs(type) words. Word order applies to 32-bit
 * types only. Returns raw * scale + offset (float32 types: the IEEE-754 value * scale + offset).
 */
float dp_decode(const uint16_t *regs, dp_dtype_t type, dp_word_t word, float scale, float offset);

/*
 * Encode an ADR-005 payload from engineering `values` (indexed by dp_field_t.value_index) using the
 * profile's payload table. Writes the 3-byte header [schema][device_byte][flags] then each field
 * (big-endian, saturating, never wrapping). Returns profile->total_len, or 0 on bad args / cap too
 * small / a field that runs past total_len.
 */
size_t dp_encode_payload(const device_profile_t *profile, const float *values, uint8_t flags,
                         uint8_t *out, size_t cap);

#endif /* DEVICE_PROFILE_H */
