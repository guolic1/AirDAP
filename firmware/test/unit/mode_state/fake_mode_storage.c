#include "airdap_mode_storage.h"

airdap_dap_route_t fake_saved_route;
bool fake_mode_save_error;
bool fake_mode_load_error;
unsigned fake_mode_writes;
void (*fake_mode_save_hook)(void);

bool airdap_mode_storage_load(airdap_dap_route_t *route)
{
    if (fake_mode_load_error) return false;
    *route = fake_saved_route;
    return true;
}

bool airdap_mode_storage_save(airdap_dap_route_t route)
{
    ++fake_mode_writes;
    if (fake_mode_save_hook != 0) fake_mode_save_hook();
    if (fake_mode_save_error) return false;
    fake_saved_route = route;
    return true;
}
