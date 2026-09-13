#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "airdap_network_indicator.h"

static void test_patterns(void)
{
    for (unsigned wifi = AIRDAP_WIFI_STOPPED; wifi <= AIRDAP_WIFI_ONLINE; ++wifi) {
        airdap_mode_snapshot_t mode = {.wifi = (airdap_wifi_state_t) wifi};
        airdap_network_indicator_t indicator = {0};
        airdap_network_indicator_step(&indicator, &mode, false, 20);
        assert(!indicator.network_on);
        airdap_network_indicator_step(&indicator, &mode, true, 20);
        assert(indicator.network_on == (wifi != AIRDAP_WIFI_STOPPED));

        /* Provisioning overrides both the route and Wi-Fi status. */
        mode.provisioning = AIRDAP_PROVISIONING_ACTIVE;
        airdap_network_indicator_step(&indicator, &mode, false, 20);
        assert(indicator.network_on);
        airdap_network_indicator_step(&indicator, &mode, false, 99);
        assert(indicator.network_on);
        airdap_network_indicator_step(&indicator, &mode, false, 1);
        assert(!indicator.network_on);
        airdap_network_indicator_step(&indicator, &mode, true, 100);
        assert(indicator.network_on);

        mode.provisioning = AIRDAP_PROVISIONING_TIMED_OUT;
        airdap_network_indicator_step(&indicator, &mode, false, 20);
        assert(!indicator.network_on);
    }
}

static void test_retry_phase_and_transitions(void)
{
    airdap_mode_snapshot_t mode = {.wifi = AIRDAP_WIFI_CONNECTING};
    airdap_network_indicator_t indicator = {0};
    airdap_network_indicator_step(&indicator, &mode, true, 20);
    assert(indicator.network_on);
    for (unsigned elapsed = 20; elapsed <= 2000; elapsed += 20) {
        /* Repeated failures/retries must not keep restarting the lit phase. */
        mode.wifi = elapsed % 40 == 0 ? AIRDAP_WIFI_CONNECTING : AIRDAP_WIFI_DISCONNECTED;
        airdap_network_indicator_step(&indicator, &mode, true, 20);
        assert(indicator.network_on == (elapsed % 1000 < 500));
    }
    airdap_network_indicator_step(&indicator, &mode, true, 700);
    assert(!indicator.network_on);
    mode.wifi = AIRDAP_WIFI_ONLINE;
    airdap_network_indicator_step(&indicator, &mode, true, 20);
    assert(indicator.network_on);
    airdap_network_indicator_step(&indicator, &mode, true, UINT32_MAX);
    assert(indicator.network_on);

    mode.provisioning = AIRDAP_PROVISIONING_ACTIVE;
    airdap_network_indicator_step(&indicator, &mode, true, 20);
    airdap_network_indicator_step(&indicator, &mode, true, 100);
    assert(!indicator.network_on);
    mode.provisioning = AIRDAP_PROVISIONING_SUCCEEDED;
    airdap_network_indicator_step(&indicator, &mode, true, 20);
    assert(indicator.network_on);

    mode.wifi = AIRDAP_WIFI_CONNECTING;
    airdap_network_indicator_step(&indicator, &mode, true, 20);
    airdap_network_indicator_step(&indicator, &mode, true, 700);
    assert(!indicator.network_on);
    airdap_network_indicator_step(&indicator, &mode, false, 20);
    assert(!indicator.network_on);
    airdap_network_indicator_step(&indicator, &mode, true, 20);
    assert(indicator.network_on);
    airdap_network_indicator_step(&indicator, &mode, true, UINT32_MAX);
    assert(indicator.network_on); /* UINT32_MAX % 1000 = 295 ms. */
    airdap_network_indicator_step(&indicator, &mode, true, 205);
    assert(!indicator.network_on);
}

static void expect_route_light(bool on)
{
    airdap_network_indicator_t indicator = {0};
    airdap_mode_snapshot_t mode;
    assert(airdap_mode_state_get(&mode) == AIRDAP_MODE_STATE_OK);
    airdap_network_indicator_step(&indicator, &mode,
        airdap_mode_state_network_data_enabled(), 20);
    assert(indicator.network_on == on);
}

static bool unexpected_pin_operation(void *context)
{
    (void) context;
    assert(false);
    return false;
}

static void test_route_policy(void)
{
    const airdap_dap_ownership_backend_t backend = {
        .line_reset = unexpected_pin_operation,
        .release_pins = unexpected_pin_operation,
    };
    assert(airdap_dap_ownership_initialize(&backend) == AIRDAP_DAP_OWNERSHIP_OK);
    airdap_mode_state_init();
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) == AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) == AIRDAP_MODE_STATE_OK);
    expect_route_light(true); /* AUTO without USB. */
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_ATTACHED) == AIRDAP_MODE_STATE_OK);
    expect_route_light(false);
    assert(airdap_mode_state_set_dap_route(AIRDAP_DAP_ROUTE_NETWORK) == AIRDAP_MODE_DAP_ALLOWED);
    expect_route_light(true);
    assert(airdap_mode_state_set_dap_route(AIRDAP_DAP_ROUTE_USB) == AIRDAP_MODE_DAP_ALLOWED);
    expect_route_light(false);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_DETACHED) == AIRDAP_MODE_STATE_OK);
    expect_route_light(false); /* Explicit USB stays off even without a host. */
    assert(airdap_mode_state_set_dap_route(AIRDAP_DAP_ROUTE_AUTO) == AIRDAP_MODE_DAP_ALLOWED);
    expect_route_light(true);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_STOPPED) == AIRDAP_MODE_STATE_OK);
    expect_route_light(false);
}

int main(void)
{
    test_patterns();
    test_retry_phase_and_transitions();
    test_route_policy();
    puts("Network indicator tests passed");
    return 0;
}
