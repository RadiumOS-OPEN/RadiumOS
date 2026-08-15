// =============================================================================
// RSH Plugin Loader  --  rsh_plugin.h
//
// Bridges the RSH v2.3 script engine to the keyboard.c command-registration
// system.  A ".rsh" plugin file must define:
//
//   function information
//       map.new RadiTest
//       map.set RadiTest Name "my-command"   ← used as the shell command name
//       map.set RadiTest Ver  1.0
//       map.set RadiTest Author "scp_2801"
//   endfunction
//
//   function command
//       % $1 $2 ... are the user's arguments
//       echo $1
//   endfunction
//
// There MUST NOT be a main() function or a ^entrypoint directive.
// The function named "information" is called once at load time to discover
// the command name.  "command" is called every time the user types that name.
//
// Usage:
//   #include "rsh_plugin.h"
//
//   // At init (e.g. inside kernel_main or your shell init):
//   rsh_plugin_load("/plugins/example.rsh");
//
//   // That's it.  The plugin is now a first-class shell command.
// =============================================================================

#ifndef RSH_PLUGIN_H
#define RSH_PLUGIN_H

#include <stdint.h>
#include <stdbool.h>

// Maximum number of simultaneously loaded RSH plugins.
#define RSH_MAX_PLUGINS 16

// Maximum length of a command name extracted from the plugin's RadiTest map.
#define RSH_CMD_NAME_MAX 64

// Maximum number of arguments forwarded to command().
// Must be <= MAX_ARGUMENTS from keyboard.h.
#define RSH_MAX_ARGS 16

// ── Opaque plugin handle ──────────────────────────────────────────────────────
// One ScriptCtx is allocated per plugin so plugins don't share variable/
// function state.  The ctx is owned by the plugin table; never free manually.

typedef struct RshPlugin {
    char  cmd_name[RSH_CMD_NAME_MAX]; // name from RadiTest["Name"]
    char  version[32];
    char  author[64];
    char  path[256];                  // source path, kept for reload
    bool  loaded;
} RshPlugin;

// ── Public API ────────────────────────────────────────────────────────────────

// Load a plugin from a path in AVFS.
// Calls script_init on an internal context, runs the first-pass (which
// populates function definitions), then calls information() to read the
// RadiTest metadata map, then registers the extracted Name with
// register_command().
//
// Returns: pointer to the filled RshPlugin slot, or NULL on failure.
RshPlugin *rsh_plugin_load(const char *path);
// Unload a plugin by command name (unregisters it from the command table
// if your kernel supports deregistration; otherwise just marks it inactive).
void rsh_plugin_unload(const char *cmd_name);

// List all loaded plugins to the terminal.
void rsh_plugin_list(void);

// Reload a plugin by command name (drop + reload from stored path).
RshPlugin *rsh_plugin_reload(const char *cmd_name);

#endif // RSH_PLUGIN_H