#pragma once

typedef struct {
    const char *firmware_version;
} airdap_device_identity_t;

const airdap_device_identity_t *airdap_device_identity_get(void);
