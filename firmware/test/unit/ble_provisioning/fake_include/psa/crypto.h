#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int psa_status_t;
typedef unsigned int psa_algorithm_t;

#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 0x02000009U

psa_status_t psa_hash_compute(
    psa_algorithm_t algorithm,
    const uint8_t *input,
    size_t input_length,
    uint8_t *hash,
    size_t hash_size,
    size_t *hash_length);
