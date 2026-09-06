#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_DEBUG_SHELL_MAX_COMMANDS = 24,
    AIRDAP_DEBUG_SHELL_COMMAND_NAME_MAX_LENGTH = 31,
};

typedef enum {
    AIRDAP_DEBUG_SHELL_STYLE_DEFAULT = 0,
    AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
    AIRDAP_DEBUG_SHELL_STYLE_WARNING,
    AIRDAP_DEBUG_SHELL_STYLE_ERROR,
    AIRDAP_DEBUG_SHELL_STYLE_COMMAND,
} airdap_debug_shell_style_t;

typedef struct {
    void *session;
    void (*vprintf)(
        airdap_debug_shell_style_t style,
        const char *format,
        va_list arguments,
        void *context);
    void *output_context;
} airdap_debug_shell_invocation_t;

typedef int (*airdap_debug_shell_command_handler_t)(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context);

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    const char *details;
    airdap_debug_shell_command_handler_t handler;
    void *context;
} airdap_debug_shell_command_t;

/* Registered descriptors and their strings must remain valid for the lifetime
 * of the shell. Registration is restricted to startup and rejected after the
 * registry is frozen. */

typedef enum {
    AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED = 0,
    AIRDAP_DEBUG_SHELL_COMMAND_INVALID,
    AIRDAP_DEBUG_SHELL_COMMAND_DUPLICATE,
    AIRDAP_DEBUG_SHELL_COMMAND_FULL,
    AIRDAP_DEBUG_SHELL_COMMAND_FROZEN,
} airdap_debug_shell_command_register_result_t;

typedef struct {
    const airdap_debug_shell_command_t
        *entries[AIRDAP_DEBUG_SHELL_MAX_COMMANDS];
    size_t count;
    bool frozen;
} airdap_debug_shell_command_registry_t;

void airdap_debug_shell_command_registry_init(
    airdap_debug_shell_command_registry_t *registry);

airdap_debug_shell_command_register_result_t
airdap_debug_shell_command_register(
    airdap_debug_shell_command_registry_t *registry,
    const airdap_debug_shell_command_t *command);

void airdap_debug_shell_command_registry_freeze(
    airdap_debug_shell_command_registry_t *registry);

size_t airdap_debug_shell_command_count(
    const airdap_debug_shell_command_registry_t *registry);

const airdap_debug_shell_command_t *airdap_debug_shell_command_at(
    const airdap_debug_shell_command_registry_t *registry,
    size_t index);

const airdap_debug_shell_command_t *airdap_debug_shell_command_find(
    const airdap_debug_shell_command_registry_t *registry,
    const char *name,
    size_t name_length);

const char *airdap_debug_shell_command_complete(
    const airdap_debug_shell_command_registry_t *registry,
    const char *prefix,
    size_t match_index);

int airdap_debug_shell_command_help(
    const airdap_debug_shell_command_registry_t *registry,
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation);

void airdap_debug_shell_printf(
    const airdap_debug_shell_invocation_t *invocation,
    airdap_debug_shell_style_t style,
    const char *format,
    ...);

#ifdef __cplusplus
}
#endif
