#!/usr/bin/env bash

if [[ -z "${BASH_VERSION:-}" ]]; then
    echo "get_env.sh: Bash is required" >&2
    return 1 2>/dev/null || exit 1
fi

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "get_env.sh: source this script so it can update the current shell" >&2
    exit 1
fi

_airdap_firmware_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
_airdap_environment_dir="${_airdap_firmware_dir}/.airdap-env"
_airdap_config_file="${_airdap_environment_dir}/idf-path.txt"

if [[ ! -f "${_airdap_config_file}" ]]; then
    echo "get_env.sh: ESP-IDF is not configured; run tools/setup.py first" >&2
    unset _airdap_firmware_dir _airdap_environment_dir _airdap_config_file
    return 1
fi

mapfile -t _airdap_config_lines < "${_airdap_config_file}"
if [[ ${#_airdap_config_lines[@]} -eq 1 && -n "${_airdap_config_lines[0]}" ]]; then
    # Configurations written before provenance was recorded used one line.
    _airdap_idf_mode=legacy
elif [[ ${#_airdap_config_lines[@]} -eq 2 \
    && -n "${_airdap_config_lines[0]}" \
    && ("${_airdap_config_lines[1]}" == managed \
        || "${_airdap_config_lines[1]}" == external) ]]; then
    _airdap_idf_mode="${_airdap_config_lines[1]}"
else
    echo "get_env.sh: ${_airdap_config_file} must contain an ESP-IDF path and managed/external mode" >&2
    unset _airdap_firmware_dir _airdap_environment_dir _airdap_config_file _airdap_config_lines
    return 1
fi

_airdap_idf_path="${_airdap_config_lines[0]}"
_airdap_export_script="${_airdap_idf_path}/export.sh"
if [[ ! -f "${_airdap_export_script}" ]]; then
    echo "get_env.sh: configured ESP-IDF is missing ${_airdap_export_script}" >&2
    unset _airdap_firmware_dir _airdap_environment_dir _airdap_config_file
    unset _airdap_config_lines _airdap_idf_path _airdap_export_script
    return 1
fi

export IDF_TOOLS_PATH="${_airdap_environment_dir}"
unset IDF_PYTHON_ENV_PATH
export IDF_SKIP_TOOLS_CHECK=1

_airdap_managed_idf_path="${_airdap_environment_dir}/esp-idf"
if [[ "${_airdap_idf_mode}" == managed ]]; then
    export IDF_SKIP_CHECK_SUBMODULES=1
elif [[ "${_airdap_idf_mode}" == external ]]; then
    unset IDF_SKIP_CHECK_SUBMODULES
else
    if [[ -d "${_airdap_managed_idf_path}" ]]; then
        _airdap_managed_idf_path=$(CDPATH= cd -- "${_airdap_managed_idf_path}" && pwd -P)
    fi
    if [[ "${_airdap_idf_path}" == "${_airdap_managed_idf_path}" ]]; then
        export IDF_SKIP_CHECK_SUBMODULES=1
    else
        unset IDF_SKIP_CHECK_SUBMODULES
    fi
fi

# shellcheck source=/dev/null
. "${_airdap_export_script}"
_airdap_status=$?

unset _airdap_firmware_dir _airdap_environment_dir _airdap_config_file
unset _airdap_config_lines _airdap_idf_path _airdap_idf_mode _airdap_export_script
unset _airdap_managed_idf_path
if [[ ${_airdap_status} -ne 0 ]]; then
    unset _airdap_status
    return 1
fi
unset _airdap_status
return 0
