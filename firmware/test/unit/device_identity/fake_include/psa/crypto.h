#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;

#define PSA_SUCCESS ((psa_status_t) 0)
#define PSA_ERROR_GENERIC_ERROR ((psa_status_t) -132)
#define PSA_ALG_SHA_256 ((psa_algorithm_t) 0x02000009U)

psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_compute(
    psa_algorithm_t alg,
    const uint8_t *input,
    size_t input_length,
    uint8_t *hash,
    size_t hash_size,
    size_t *hash_length);
