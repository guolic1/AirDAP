#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "airdap_debug_shell_commands.h"

static bool valid_name(const char *name)
{
    if (name == NULL || name[0] < 'a' || name[0] > 'z') {
        return false;
    }

    size_t length = 0U;
    while (name[length] != '\0') {
        const char value = name[length];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-')) {
            return false;
        }
        ++length;
        if (length > AIRDAP_DEBUG_SHELL_COMMAND_NAME_MAX_LENGTH) {
            return false;
        }
    }
    return true;
}

static bool valid_command(const airdap_debug_shell_command_t *command)
{
    return command != NULL && valid_name(command->name) &&
        command->usage != NULL && command->usage[0] != '\0' &&
        command->summary != NULL && command->summary[0] != '\0' &&
        command->details != NULL && command->details[0] != '\0' &&
        command->handler != NULL;
}

void airdap_debug_shell_command_registry_init(
    airdap_debug_shell_command_registry_t *registry)
{
    if (registry != NULL) {
        memset(registry, 0, sizeof(*registry));
    }
}

airdap_debug_shell_command_register_result_t
airdap_debug_shell_command_register(
    airdap_debug_shell_command_registry_t *registry,
    const airdap_debug_shell_command_t *command)
{
    if (registry == NULL || !valid_command(command)) {
        return AIRDAP_DEBUG_SHELL_COMMAND_INVALID;
    }
    if (registry->frozen) {
        return AIRDAP_DEBUG_SHELL_COMMAND_FROZEN;
    }
    for (size_t index = 0U; index < registry->count; ++index) {
        if (strcmp(registry->entries[index]->name, command->name) == 0) {
            return AIRDAP_DEBUG_SHELL_COMMAND_DUPLICATE;
        }
    }
    if (registry->count >= AIRDAP_DEBUG_SHELL_MAX_COMMANDS) {
        return AIRDAP_DEBUG_SHELL_COMMAND_FULL;
    }

    registry->entries[registry->count] = command;
    ++registry->count;
    return AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED;
}

void airdap_debug_shell_command_registry_freeze(
    airdap_debug_shell_command_registry_t *registry)
{
    if (registry != NULL) {
        registry->frozen = true;
    }
}

size_t airdap_debug_shell_command_count(
    const airdap_debug_shell_command_registry_t *registry)
{
    return registry != NULL ? registry->count : 0U;
}

const airdap_debug_shell_command_t *airdap_debug_shell_command_at(
    const airdap_debug_shell_command_registry_t *registry,
    size_t index)
{
    if (registry == NULL || index >= registry->count) {
        return NULL;
    }
    return registry->entries[index];
}

const airdap_debug_shell_command_t *airdap_debug_shell_command_find(
    const airdap_debug_shell_command_registry_t *registry,
    const char *name,
    size_t name_length)
{
    if (registry == NULL || name == NULL || name_length == 0U) {
        return NULL;
    }
    for (size_t index = 0U; index < registry->count; ++index) {
        const airdap_debug_shell_command_t *command = registry->entries[index];
        if (strlen(command->name) == name_length &&
            strncmp(command->name, name, name_length) == 0) {
            return command;
        }
    }
    return NULL;
}

const char *airdap_debug_shell_command_complete(
    const airdap_debug_shell_command_registry_t *registry,
    const char *prefix,
    size_t match_index)
{
    if (registry == NULL || prefix == NULL) {
        return NULL;
    }

    const size_t prefix_length = strlen(prefix);
    size_t current_match = 0U;
    for (size_t index = 0U; index < registry->count; ++index) {
        const char *name = registry->entries[index]->name;
        if (strncmp(name, prefix, prefix_length) != 0) {
            continue;
        }
        if (current_match == match_index) {
            return name;
        }
        ++current_match;
    }
    return NULL;
}

int airdap_debug_shell_command_help(
    const airdap_debug_shell_command_registry_t *registry,
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation)
{
    if (registry == NULL || arguments == NULL || invocation == NULL) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_WARNING,
            "usage: help [command]\n");
        return 1;
    }

    if (*arguments != '\0') {
        const char *command_end = arguments;
        while (*command_end != '\0' && *command_end != ' ') {
            ++command_end;
        }
        const size_t command_length = (size_t) (command_end - arguments);
        while (*command_end == ' ') {
            ++command_end;
        }
        if (command_length == 0U || *command_end != '\0') {
            airdap_debug_shell_printf(
                invocation,
                AIRDAP_DEBUG_SHELL_STYLE_WARNING,
                "usage: help [command]\n");
            return 1;
        }

        const airdap_debug_shell_command_t *command =
            airdap_debug_shell_command_find(
                registry,
                arguments,
                command_length);
        if (command == NULL) {
            airdap_debug_shell_printf(
                invocation,
                AIRDAP_DEBUG_SHELL_STYLE_ERROR,
                "help: command not found: %.*s\n",
                (int) command_length,
                arguments);
            return 1;
        }
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_DEFAULT,
            "usage: %s\nsummary: %s\ndetails: %s\n",
            command->usage,
            command->summary,
            command->details);
        return 0;
    }

    size_t name_width = 0U;
    const size_t command_count = airdap_debug_shell_command_count(registry);
    for (size_t index = 0U; index < command_count; ++index) {
        const airdap_debug_shell_command_t *command =
            airdap_debug_shell_command_at(registry, index);
        const size_t name_length = strlen(command->name);
        if (name_length > name_width) {
            name_width = name_length;
        }
    }
    for (size_t index = 0U; index < command_count; ++index) {
        const airdap_debug_shell_command_t *command =
            airdap_debug_shell_command_at(registry, index);
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_COMMAND,
            "%-*s",
            (int) name_width,
            command->name);
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_DEFAULT,
            " %s\n",
            command->summary);
    }
    return 0;
}

void airdap_debug_shell_printf(
    const airdap_debug_shell_invocation_t *invocation,
    airdap_debug_shell_style_t style,
    const char *format,
    ...)
{
    if (invocation == NULL || invocation->vprintf == NULL || format == NULL) {
        return;
    }

    va_list arguments;
    va_start(arguments, format);
    invocation->vprintf(
        style,
        format,
        arguments,
        invocation->output_context);
    va_end(arguments);
}
