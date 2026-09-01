#include <stddef.h>
#include <stdatomic.h>

#include "airdap_mode_state.h"

enum {
    MODE_USB_PRESENT = 1U << 0,
    MODE_WIFI_SHIFT = 1,
    MODE_WIFI_MASK = 0x3U << MODE_WIFI_SHIFT,
    MODE_PROVISIONING_SHIFT = 3,
    MODE_PROVISIONING_MASK = 0x3U << MODE_PROVISIONING_SHIFT,
    MODE_OTA_SHIFT = 5,
    MODE_OTA_MASK = 0x3U << MODE_OTA_SHIFT,
    MODE_INITIALIZED = 1U << 7,
    /* Separate epochs detect policy ABA without making Wi-Fi churn invalidate
     * an otherwise stable wired transaction. */
    MODE_USB_OTA_EPOCH_SHIFT = 8,
    MODE_USB_OTA_EPOCH_MASK = 0xFFU << MODE_USB_OTA_EPOCH_SHIFT,
    MODE_WIFI_EPOCH_SHIFT = 16,
    MODE_WIFI_EPOCH_MASK = 0xFFU << MODE_WIFI_EPOCH_SHIFT,
};

static atomic_uint mode_control;

static unsigned int replace_field(
    unsigned int control,
    unsigned int mask,
    unsigned int shift,
    unsigned int value)
{
    return (control & ~mask) | (value << shift);
}

static unsigned int field_value(
    unsigned int control,
    unsigned int mask,
    unsigned int shift)
{
    return (control & mask) >> shift;
}

static unsigned int increment_field(
    unsigned int control,
    unsigned int mask,
    unsigned int shift)
{
    return replace_field(
        control,
        mask,
        shift,
        (field_value(control, mask, shift) + 1U) & 0xFFU);
}

static airdap_mode_state_result_t next_control(
    unsigned int current,
    airdap_mode_event_t event,
    unsigned int *next)
{
    *next = current;
    const airdap_wifi_state_t wifi = (airdap_wifi_state_t) field_value(
        current,
        MODE_WIFI_MASK,
        MODE_WIFI_SHIFT);
    const airdap_provisioning_state_t provisioning =
        (airdap_provisioning_state_t) field_value(
            current,
            MODE_PROVISIONING_MASK,
            MODE_PROVISIONING_SHIFT);
    const airdap_mode_ota_state_t ota =
        (airdap_mode_ota_state_t) field_value(
            current,
            MODE_OTA_MASK,
            MODE_OTA_SHIFT);

    switch (event) {
    case AIRDAP_MODE_EVENT_USB_ATTACHED:
        *next |= MODE_USB_PRESENT;
        break;
    case AIRDAP_MODE_EVENT_USB_DETACHED:
        *next &= ~MODE_USB_PRESENT;
        break;
    case AIRDAP_MODE_EVENT_WIFI_STOPPED:
        *next = replace_field(
            current,
            MODE_WIFI_MASK,
            MODE_WIFI_SHIFT,
            AIRDAP_WIFI_STOPPED);
        break;
    case AIRDAP_MODE_EVENT_WIFI_DISCONNECTED:
        if (wifi == AIRDAP_WIFI_STOPPED) {
            return AIRDAP_MODE_STATE_INVALID_TRANSITION;
        }
        *next = replace_field(
            current,
            MODE_WIFI_MASK,
            MODE_WIFI_SHIFT,
            AIRDAP_WIFI_DISCONNECTED);
        break;
    case AIRDAP_MODE_EVENT_WIFI_CONNECTING:
        *next = replace_field(
            current,
            MODE_WIFI_MASK,
            MODE_WIFI_SHIFT,
            AIRDAP_WIFI_CONNECTING);
        break;
    case AIRDAP_MODE_EVENT_WIFI_ONLINE:
        if (wifi != AIRDAP_WIFI_CONNECTING) {
            return AIRDAP_MODE_STATE_INVALID_TRANSITION;
        }
        *next = replace_field(
            current,
            MODE_WIFI_MASK,
            MODE_WIFI_SHIFT,
            AIRDAP_WIFI_ONLINE);
        break;
    case AIRDAP_MODE_EVENT_PROVISIONING_STARTED:
        if (provisioning == AIRDAP_PROVISIONING_ACTIVE) {
            return AIRDAP_MODE_STATE_INVALID_TRANSITION;
        }
        *next = replace_field(
            current,
            MODE_PROVISIONING_MASK,
            MODE_PROVISIONING_SHIFT,
            AIRDAP_PROVISIONING_ACTIVE);
        break;
    case AIRDAP_MODE_EVENT_PROVISIONING_SUCCEEDED:
        if (provisioning != AIRDAP_PROVISIONING_ACTIVE) {
            return AIRDAP_MODE_STATE_INVALID_TRANSITION;
        }
        *next = replace_field(
            current,
            MODE_PROVISIONING_MASK,
            MODE_PROVISIONING_SHIFT,
            AIRDAP_PROVISIONING_SUCCEEDED);
        break;
    case AIRDAP_MODE_EVENT_PROVISIONING_TIMED_OUT:
        if (provisioning != AIRDAP_PROVISIONING_ACTIVE) {
            return AIRDAP_MODE_STATE_INVALID_TRANSITION;
        }
        *next = replace_field(
            current,
            MODE_PROVISIONING_MASK,
            MODE_PROVISIONING_SHIFT,
            AIRDAP_PROVISIONING_TIMED_OUT);
        break;
    case AIRDAP_MODE_EVENT_PROVISIONING_RESET:
        *next = replace_field(
            current,
            MODE_PROVISIONING_MASK,
            MODE_PROVISIONING_SHIFT,
            AIRDAP_PROVISIONING_IDLE);
        break;
    case AIRDAP_MODE_EVENT_OTA_STARTED:
        if (ota != AIRDAP_OTA_IDLE) {
            return AIRDAP_MODE_STATE_INVALID_TRANSITION;
        }
        *next = replace_field(
            current,
            MODE_OTA_MASK,
            MODE_OTA_SHIFT,
            AIRDAP_OTA_RECEIVING);
        break;
    case AIRDAP_MODE_EVENT_OTA_ABORTED:
    case AIRDAP_MODE_EVENT_OTA_FAILED:
        if (ota != AIRDAP_OTA_RECEIVING) {
            return AIRDAP_MODE_STATE_INVALID_TRANSITION;
        }
        *next = replace_field(
            current,
            MODE_OTA_MASK,
            MODE_OTA_SHIFT,
            AIRDAP_OTA_IDLE);
        break;
    case AIRDAP_MODE_EVENT_OTA_COMMITTED:
        if (ota != AIRDAP_OTA_RECEIVING) {
            return AIRDAP_MODE_STATE_INVALID_TRANSITION;
        }
        *next = replace_field(
            current,
            MODE_OTA_MASK,
            MODE_OTA_SHIFT,
            AIRDAP_OTA_READY_TO_REBOOT);
        break;
    case AIRDAP_MODE_EVENT_OTA_RESET:
        *next = replace_field(
            current,
            MODE_OTA_MASK,
            MODE_OTA_SHIFT,
            AIRDAP_OTA_IDLE);
        break;
    default:
        return AIRDAP_MODE_STATE_INVALID_ARGUMENT;
    }

    if (event == AIRDAP_MODE_EVENT_USB_ATTACHED ||
        event == AIRDAP_MODE_EVENT_USB_DETACHED ||
        event >= AIRDAP_MODE_EVENT_OTA_STARTED) {
        *next = increment_field(
            *next,
            MODE_USB_OTA_EPOCH_MASK,
            MODE_USB_OTA_EPOCH_SHIFT);
    } else if (event >= AIRDAP_MODE_EVENT_WIFI_STOPPED &&
               event <= AIRDAP_MODE_EVENT_WIFI_ONLINE) {
        *next = increment_field(
            *next,
            MODE_WIFI_EPOCH_MASK,
            MODE_WIFI_EPOCH_SHIFT);
    }
    return AIRDAP_MODE_STATE_OK;
}

void airdap_mode_state_init(void)
{
    atomic_store(&mode_control, MODE_INITIALIZED);
}

airdap_mode_state_result_t airdap_mode_state_transition(
    airdap_mode_event_t event)
{
    unsigned int current = atomic_load(&mode_control);
    for (;;) {
        if ((current & MODE_INITIALIZED) == 0U) {
            return AIRDAP_MODE_STATE_INVALID_STATE;
        }

        unsigned int next;
        const airdap_mode_state_result_t result = next_control(
            current,
            event,
            &next);
        if (result != AIRDAP_MODE_STATE_OK) {
            return result;
        }
        if (atomic_compare_exchange_weak(&mode_control, &current, next)) {
            if (event == AIRDAP_MODE_EVENT_USB_ATTACHED) {
                (void) airdap_dap_ownership_revoke_owner(
                    AIRDAP_DAP_OWNER_NETWORK);
            }
            return AIRDAP_MODE_STATE_OK;
        }
    }
}

airdap_mode_state_result_t airdap_mode_state_get(
    airdap_mode_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return AIRDAP_MODE_STATE_INVALID_ARGUMENT;
    }

    const unsigned int control = atomic_load(&mode_control);
    if ((control & MODE_INITIALIZED) == 0U) {
        return AIRDAP_MODE_STATE_INVALID_STATE;
    }

    snapshot->usb_present = (control & MODE_USB_PRESENT) != 0U;
    snapshot->wifi = (airdap_wifi_state_t) field_value(
        control,
        MODE_WIFI_MASK,
        MODE_WIFI_SHIFT);
    snapshot->provisioning = (airdap_provisioning_state_t) field_value(
        control,
        MODE_PROVISIONING_MASK,
        MODE_PROVISIONING_SHIFT);
    snapshot->ota = (airdap_mode_ota_state_t) field_value(
        control,
        MODE_OTA_MASK,
        MODE_OTA_SHIFT);
    snapshot->dap_owner = airdap_dap_ownership_current();
    return AIRDAP_MODE_STATE_OK;
}

static airdap_mode_dap_result_t dap_admission_for_control(
    unsigned int control,
    airdap_dap_owner_t requested_owner,
    bool authenticated)
{
    if (requested_owner != AIRDAP_DAP_OWNER_USB &&
        requested_owner != AIRDAP_DAP_OWNER_NETWORK &&
        requested_owner != AIRDAP_DAP_OWNER_DIAGNOSTIC) {
        return AIRDAP_MODE_DAP_INVALID_ARGUMENT;
    }

    if ((control & MODE_INITIALIZED) == 0U) {
        return AIRDAP_MODE_DAP_INVALID_STATE;
    }
    if (requested_owner == AIRDAP_DAP_OWNER_NETWORK && !authenticated) {
        return AIRDAP_MODE_DAP_UNAUTHENTICATED;
    }
    if ((airdap_mode_ota_state_t) field_value(
            control,
            MODE_OTA_MASK,
            MODE_OTA_SHIFT) != AIRDAP_OTA_IDLE) {
        return AIRDAP_MODE_DAP_BUSY;
    }

    switch (requested_owner) {
    case AIRDAP_DAP_OWNER_USB:
        if ((control & MODE_USB_PRESENT) == 0U) {
            return AIRDAP_MODE_DAP_OFFLINE;
        }
        break;
    case AIRDAP_DAP_OWNER_NETWORK:
        if ((control & MODE_USB_PRESENT) != 0U) {
            return AIRDAP_MODE_DAP_BUSY;
        }
        if ((airdap_wifi_state_t) field_value(
                control,
                MODE_WIFI_MASK,
                MODE_WIFI_SHIFT) != AIRDAP_WIFI_ONLINE) {
            return AIRDAP_MODE_DAP_OFFLINE;
        }
        break;
    case AIRDAP_DAP_OWNER_DIAGNOSTIC:
        if ((control & MODE_USB_PRESENT) == 0U) {
            return AIRDAP_MODE_DAP_OFFLINE;
        }
        break;
    default:
        return AIRDAP_MODE_DAP_INVALID_ARGUMENT;
    }

    const airdap_dap_owner_t owner = airdap_dap_ownership_current();
    if (owner != AIRDAP_DAP_OWNER_NONE && owner != requested_owner) {
        return AIRDAP_MODE_DAP_BUSY;
    }
    return AIRDAP_MODE_DAP_ALLOWED;
}

airdap_mode_dap_result_t airdap_mode_state_dap_admission(
    airdap_dap_owner_t requested_owner,
    bool authenticated)
{
    return dap_admission_for_control(
        atomic_load(&mode_control),
        requested_owner,
        authenticated);
}

static airdap_mode_dap_result_t ownership_result(
    airdap_dap_ownership_result_t result)
{
    switch (result) {
    case AIRDAP_DAP_OWNERSHIP_OK:
    case AIRDAP_DAP_OWNERSHIP_ALREADY_OWNER:
        return AIRDAP_MODE_DAP_ALLOWED;
    case AIRDAP_DAP_OWNERSHIP_BUSY:
        return AIRDAP_MODE_DAP_BUSY;
    case AIRDAP_DAP_OWNERSHIP_OFFLINE:
        return AIRDAP_MODE_DAP_OFFLINE;
    case AIRDAP_DAP_OWNERSHIP_INVALID_ARGUMENT:
        return AIRDAP_MODE_DAP_INVALID_ARGUMENT;
    case AIRDAP_DAP_OWNERSHIP_NOT_OWNER:
    case AIRDAP_DAP_OWNERSHIP_INVALID_STATE:
    default:
        return AIRDAP_MODE_DAP_INVALID_STATE;
    }
}

static unsigned int dap_policy_stamp(
    unsigned int control,
    airdap_dap_owner_t owner)
{
    unsigned int stamp = control &
        (MODE_USB_PRESENT | MODE_OTA_MASK | MODE_USB_OTA_EPOCH_MASK);
    if (owner == AIRDAP_DAP_OWNER_NETWORK) {
        stamp |= control & (MODE_WIFI_MASK | MODE_WIFI_EPOCH_MASK);
    }
    return stamp;
}

airdap_mode_dap_result_t airdap_mode_state_dap_acquire(
    airdap_dap_owner_t requested_owner,
    bool authenticated,
    airdap_dap_ownership_claim_t *claim)
{
    if (claim == NULL) {
        return AIRDAP_MODE_DAP_INVALID_ARGUMENT;
    }
    *claim = (airdap_dap_ownership_claim_t) {0};

    unsigned int before = atomic_load(&mode_control);
    airdap_mode_dap_result_t result = dap_admission_for_control(
        before,
        requested_owner,
        authenticated);
    if (result == AIRDAP_MODE_DAP_BUSY &&
        requested_owner == AIRDAP_DAP_OWNER_USB &&
        airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NETWORK) {
        const airdap_dap_ownership_result_t revoke_result =
            airdap_dap_ownership_revoke_owner(AIRDAP_DAP_OWNER_NETWORK);
        if (revoke_result == AIRDAP_DAP_OWNERSHIP_OK ||
            revoke_result == AIRDAP_DAP_OWNERSHIP_NOT_OWNER) {
            before = atomic_load(&mode_control);
            result = dap_admission_for_control(
                before,
                requested_owner,
                authenticated);
        } else {
            result = ownership_result(revoke_result);
        }
    }
    if (result == AIRDAP_MODE_DAP_ALLOWED) {
        const airdap_dap_ownership_result_t acquire_result =
            airdap_dap_ownership_acquire(
            requested_owner,
            claim);
        result = requested_owner == AIRDAP_DAP_OWNER_DIAGNOSTIC &&
            acquire_result == AIRDAP_DAP_OWNERSHIP_ALREADY_OWNER
            ? AIRDAP_MODE_DAP_BUSY
            : ownership_result(acquire_result);
    }
    if (result != AIRDAP_MODE_DAP_ALLOWED) {
        return result;
    }

    const unsigned int after = atomic_load(&mode_control);
    if (dap_policy_stamp(before, requested_owner) !=
        dap_policy_stamp(after, requested_owner)) {
        const airdap_dap_ownership_result_t release_result =
            airdap_dap_ownership_release(claim);
        if (release_result != AIRDAP_DAP_OWNERSHIP_OK &&
            release_result != AIRDAP_DAP_OWNERSHIP_NOT_OWNER) {
            return ownership_result(release_result);
        }
        const airdap_mode_dap_result_t latest = dap_admission_for_control(
            after,
            requested_owner,
            authenticated);
        return latest == AIRDAP_MODE_DAP_ALLOWED
            ? AIRDAP_MODE_DAP_BUSY
            : latest;
    }
    return result;
}

airdap_mode_dap_result_t airdap_mode_state_dap_operation_begin(
    airdap_dap_owner_t requested_owner,
    bool authenticated,
    const airdap_dap_ownership_claim_t *claim,
    airdap_dap_ownership_operation_t *operation)
{
    if (claim == NULL || operation == NULL) {
        return AIRDAP_MODE_DAP_INVALID_ARGUMENT;
    }

    const unsigned int before = atomic_load(&mode_control);
    airdap_mode_dap_result_t result = dap_admission_for_control(
        before,
        requested_owner,
        authenticated);
    if (result != AIRDAP_MODE_DAP_ALLOWED) {
        return result;
    }
    result = ownership_result(airdap_dap_ownership_operation_begin(
        claim,
        operation));
    if (result != AIRDAP_MODE_DAP_ALLOWED) {
        return result;
    }

    const unsigned int after = atomic_load(&mode_control);
    if (dap_policy_stamp(before, requested_owner) !=
        dap_policy_stamp(after, requested_owner)) {
        airdap_dap_ownership_operation_end(operation);
        const airdap_mode_dap_result_t latest = dap_admission_for_control(
            after,
            requested_owner,
            authenticated);
        return latest == AIRDAP_MODE_DAP_ALLOWED
            ? AIRDAP_MODE_DAP_BUSY
            : latest;
    }
    return AIRDAP_MODE_DAP_ALLOWED;
}
