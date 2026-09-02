#pragma once

#define AIRDAP_DEBUG_SHELL_COMMAND_LIST(X) \
    X("help", "List available commands", help_command) \
    X("identity", "Show the shared device identity", identity_command) \
    X( \
        "config-status", \
        "Show safe persistent-config status", \
        config_status_command) \
    X("status", "Show voltages, uptime, and free heap", status_command) \
    X("wifi", "Manage Wi-Fi credentials and show state", wifi_command) \
    X( \
        "swd-idcode", \
        "Read the target DP IDCODE [clock_khz]", \
        swd_idcode_command) \
    X("restart", "Restart the AirDAP firmware", restart_command)
