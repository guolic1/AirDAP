#pragma once

typedef struct {
    char device_id[17];
} airdap_device_identity_t;

const airdap_device_identity_t *airdap_device_identity_get(void);
