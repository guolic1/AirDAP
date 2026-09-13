#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "airdap_mode_storage.h"
#include "nvs.h"

static esp_err_t open_error, get_error, set_error, commit_error;
static bool opened, present;
static uint8_t durable, pending;
static unsigned writes, commits;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!opened && strcmp(name, "airdap_mode") == 0);
    assert(mode == NVS_READONLY || mode == NVS_READWRITE);
    if (open_error != ESP_OK) return open_error;
    opened = true;
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(opened && handle == 1); opened = false; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(opened && handle == 1 && strcmp(key, "dap_route") == 0);
    if (get_error != ESP_OK) return get_error;
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    *value = durable;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(opened && handle == 1 && strcmp(key, "dap_route") == 0);
    ++writes;
    if (set_error != ESP_OK) return set_error;
    pending = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(opened && handle == 1);
    ++commits;
    if (commit_error != ESP_OK) return commit_error;
    durable = pending;
    present = true;
    return ESP_OK;
}

int main(void)
{
    airdap_dap_route_t route = AIRDAP_DAP_ROUTE_USB;
    open_error = ESP_ERR_NVS_NOT_FOUND;
    assert(airdap_mode_storage_load(&route) && route == AIRDAP_DAP_ROUTE_AUTO);
    open_error = ESP_OK;
    assert(airdap_mode_storage_load(&route) && route == AIRDAP_DAP_ROUTE_AUTO);
    assert(writes == 0 && commits == 0 && !opened);
    for (int selected = 0; selected <= AIRDAP_DAP_ROUTE_NETWORK; ++selected) {
        assert(airdap_mode_storage_save((airdap_dap_route_t) selected));
        assert(!opened && writes == (unsigned) selected + 1 && writes == commits);
        assert(airdap_mode_storage_load(&route) && route == (airdap_dap_route_t) selected);
    }
    commit_error = ESP_FAIL;
    assert(!airdap_mode_storage_save(AIRDAP_DAP_ROUTE_AUTO) && !opened);
    assert(airdap_mode_storage_load(&route) && route == AIRDAP_DAP_ROUTE_NETWORK);
    commit_error = ESP_OK;
    set_error = ESP_FAIL;
    const unsigned previous_commits = commits;
    assert(!airdap_mode_storage_save(AIRDAP_DAP_ROUTE_AUTO) && !opened && commits == previous_commits);
    set_error = ESP_OK;
    open_error = ESP_FAIL;
    assert(!airdap_mode_storage_save(AIRDAP_DAP_ROUTE_AUTO));
    assert(!airdap_mode_storage_load(&route) && route == AIRDAP_DAP_ROUTE_NETWORK);
    open_error = ESP_OK;
    get_error = ESP_FAIL;
    assert(!airdap_mode_storage_load(&route) && !opened);
    get_error = ESP_OK;
    durable = AIRDAP_DAP_ROUTE_TOGGLE;
    assert(!airdap_mode_storage_load(&route) && route == AIRDAP_DAP_ROUTE_NETWORK && !opened);
    assert(!airdap_mode_storage_save(AIRDAP_DAP_ROUTE_TOGGLE));
    assert(!airdap_mode_storage_save(AIRDAP_DAP_ROUTE_NETWORK_AUTO_TOGGLE));
    puts("Mode NVS storage tests passed");
    return 0;
}
