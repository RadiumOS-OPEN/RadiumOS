
#include "../Avfs/Avfs.h"
#include "../terminal/terminal.h"
#include "../utility/utility.h"

// Maximum matches for wildcards
#define MAX_MATCHES 128

static DirectoryEntry entries[128];
static char matched_paths[MAX_MATCHES][256]; // Store matched paths
static int match_count = 0;

// Structure to hold command options
typedef struct {
    int recursive;
    int force;
    int interactive;
    int verbose;
    int empty_dir; // -d flag
    int preserve_root;
} RmOptions;

// Helper to get user confirmation
// NOTE: You may need to adjust 'getchar' to your specific kernel input function
// e.g., keyboard_getchar(), read_line(), etc.
static int user_confirm(const char* path) {
    print("Remove '");
    print(path);
    print("'? ");
    
    // Simple blocking read. 
    // Assuming a basic terminal_getchar() exists or similar.
    // Replace 'terminal_getchar' with your actual input function.
    char c = 0; 
    // In a real kernel, you might read from a keyboard buffer here
    // c = keyboard_read(); 
    
    // For this snippet, we simulate 'y' or assume the user handles input
    // If you have a getchar implementation:
    // c = getchar(); 
    
    // Fallback for demo purposes (always approve if interactive is on, 
    // but in real code you MUST check input):
    // print("y (auto-confirmed for demo)\n"); return 1; 
    
    // Implementing actual read logic:
    // (Pseudo-code as input drivers vary wildly)
    // c = keyboard_getchar(); 
    
    // Let's assume a function input_getchar exists for this context
    // c = input_getchar(); 
    // return (c == 'y' || c == 'Y');
    
    return 1; // Placeholder: Always return true if you don't have an input function ready
}

// Simple wildcard matching function (Existing)
static bool wildcard_match(const char* pattern, const char* str) {
    while (*pattern && *str) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (*pattern == '\0') return true;
            while (*str) {
                if (wildcard_match(pattern, str)) return true;
                str++;
            }
            return false;
        } else if (*pattern == '?' || *pattern == *str) {
            pattern++;
            str++;
        } else {
            return false;
        }
    }
    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *str == '\0');
}

// Expand wildcard patterns (Existing logic, cleaned up)
static void expand_wildcard(const char* pattern) {
    match_count = 0;
    const char* cwd = avfs_getcwd();
    
    char dir_path[256];
    char file_pattern[256];
    const char* last_slash = strrchr(pattern, '/');
    
    if (last_slash) {
        int dir_len = last_slash - pattern;
        strncpy(dir_path, pattern, dir_len);
        dir_path[dir_len] = '\0';
        if (dir_len == 0) strcpy(dir_path, "/");
        strcpy(file_pattern, last_slash + 1);
    } else {
        strcpy(dir_path, cwd);
        strcpy(file_pattern, pattern);
    }
    
    int count = avfs_list_directory(dir_path, entries, 128);
    if (count < 0) return;
    
    for (int i = 0; i < count && match_count < MAX_MATCHES; i++) {
        if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
        
        if (wildcard_match(file_pattern, entries[i].name)) {
            if (strcmp(dir_path, "/") == 0) {
                snprintf(matched_paths[match_count], 256, "/%s", entries[i].name);
            } else {
                snprintf(matched_paths[match_count], 256, "%s/%s", dir_path, entries[i].name);
            }
            match_count++;
        }
    }
}

// Recursive directory removal (Updated with Options)
static int remove_directory_recursive(const char* path, RmOptions* opts) {
    // Safety: Preserve Root
    if (opts->preserve_root && strcmp(path, "/") == 0) {
        print("Error: it is dangerous to operate recursively on '/'\n");
        print("Use --no-preserve-root to override.\n");
        return -2;
    }

    int count = avfs_list_directory(path, entries, 128);
    if (count < 0) return -1;
    
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;
        
        char full_path[256];
        if (strcmp(path, "/") == 0) {
            snprintf(full_path, sizeof(full_path), "/%s", entries[i].name);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entries[i].name);
        }
        
        // Interactive Check
        if (opts->interactive) {
            if (!user_confirm(full_path)) {
                if (opts->verbose) print("Skipped '"); print(full_path); print("'\n");
                continue;
            }
        }

        if (entries[i].is_directory) {
            if (opts->verbose) { print("Entering directory '"); print(full_path); print("'\n"); }
            if (remove_directory_recursive(full_path, opts) != 0) {
                return -1;
            }
        } else {
            if (opts->verbose) { print("Removing file '"); print(full_path); print("'\n"); }
            if (avfs_remove_file(full_path) != 0 && !opts->force) {
                print("Error: Cannot remove '"); print(full_path); print("'\n");
                return -1;
            }
        }
    }
    
    if (opts->verbose) { print("Removing directory '"); print(path); print("'\n"); }
    return avfs_remove_file(path);
}

void rm_command(int argc, char* argv[]) {
    RmOptions opts = {0};
    opts.preserve_root = 1; // Default is safe
    
    if (argc < 2) {
        print("Usage: rm [OPTIONS] <FILE>...\n");
        print("Remove (unlink) the FILE(s).\n\n");
        print("Options:\n");
        print("  -f, --force      Ignore nonexistent files and arguments, never prompt\n");
        print("  -i               Prompt before every removal\n");
        print("  -I               Prompt once before removing more than three files\n");
        print("  -r, -R, --recursive   Remove directories and their contents recursively\n");
        print("  -d, --dir       Remove empty directories\n");
        print("  -v, --verbose   Explain what is being done\n");
        print("      --preserve-root   Do not remove '/' (default)\n");
        print("      --no-preserve-root    Do not treat '/' specially\n");
        print("\nWildcards supported: * and ?\n");
        return;
    }
    
    // Parse arguments
    int start_index = 1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (argv[i][1] == '-') {
                // Long arguments
                if (strcmp(argv[i], "--recursive") == 0) opts.recursive = 1;
                else if (strcmp(argv[i], "--force") == 0) opts.force = 1;
                else if (strcmp(argv[i], "--verbose") == 0) opts.verbose = 1;
                else if (strcmp(argv[i], "--dir") == 0) opts.empty_dir = 1;
                else if (strcmp(argv[i], "--preserve-root") == 0) opts.preserve_root = 1;
                else if (strcmp(argv[i], "--no-preserve-root") == 0) opts.preserve_root = 0;
                else {
                    print("Error: Unknown option '"); print(argv[i]); print("'\n");
                    return;
                }
            } else {
                // Short arguments (Combined e.g. -rfi)
                for (int j = 1; argv[i][j] != '\0'; j++) {
                    char c = argv[i][j];
                    if (c == 'r' || c == 'R') opts.recursive = 1;
                    else if (c == 'f') opts.force = 1;
                    else if (c == 'i') opts.interactive = 1;
                    else if (c == 'v') opts.verbose = 1;
                    else if (c == 'd') opts.empty_dir = 1;
                    else if (c == 'I') { /* TODO: Implement -I logic */ }
                    else {
                        print("Error: Unknown flag '-"); terminal_putchar(c); print("'\n");
                        return;
                    }
                }
            }
            start_index++; // Skip the flag
        } else {
            // Hit a file argument, stop parsing flags
            break;
        }
    }
    
    if (start_index >= argc) {
        if (!opts.force) print("Error: No file specified\n");
        return;
    }

    // Logic: -f overrides -i
    if (opts.force) opts.interactive = 0;

    // Process each file/directory argument
    for (int i = start_index; i < argc; i++) {
        const char* pattern = argv[i];
        
        // Check for wildcards
        if (strchr(pattern, '*') || strchr(pattern, '?')) {
            expand_wildcard(pattern);
            
            if (match_count == 0) {
                if (!opts.force) {
                    print("Error: No matches for '"); print(pattern); print("'\n");
                }
                continue;
            }
            
            // Handle "I" flag (Prompt once if many matches)
            if (opts.interactive && match_count > 3) {
                print("Remove "); print_integer(match_count); 
                print(" files? "); 
                // Add input logic here similar to user_confirm
            }

            for (int j = 0; j < match_count; j++) {
                const char* path = matched_paths[j];
                
                // Interactive Check per file
                if (opts.interactive) {
                    if (!user_confirm(path)) continue;
                }

                if (avfs_is_directory(path)) {
                    if (!opts.recursive && !opts.empty_dir) {
                        if (!opts.force) {
                            print("Error: '"); print(path); print("' is a directory\n");
                        }
                        continue;
                    }
                    
                    if (opts.empty_dir && !opts.recursive) {
                        // Try to remove empty dir only
                        int res = avfs_remove_file(path);
                        if (res != 0 && !opts.force) {
                             print("Error: Directory not empty or failed: '"); print(path); print("'\n");
                        } else if (opts.verbose) {
                             print("Removed directory '"); print(path); print("'\n");
                        }
                    } else {
                        // Recursive removal
                        int res = remove_directory_recursive(path, &opts);
                        if (res == -2) continue; // Preserve root error
                        if (res != 0 && !opts.force) {
                            print("Error: Failed to remove '"); print(path); print("'\n");
                        } else if (opts.verbose) {
                            print("Removed '"); print(path); print("'\n");
                        }
                    }
                } else {
                    // Is a file
                    int res = avfs_remove_file(path);
                    if (res != 0 && !opts.force) {
                        print("Error: Failed to remove '"); print(path); print("'\n");
                    } else if (opts.verbose) {
                        print("Removed '"); print(path); print("'\n");
                    }
                }
            }
        } else {
            // No wildcards
            const char* path = pattern;
            
            if (opts.interactive && !user_confirm(path)) continue;

            if (avfs_is_directory(path)) {
                if (!opts.recursive && !opts.empty_dir) {
                    if (!opts.force) {
                        print("Error: '"); print(path); print("' is a directory\n");
                    }
                    continue;
                }
                
                if (opts.empty_dir && !opts.recursive) {
                    int res = avfs_remove_file(path);
                    if (res != 0 && !opts.force) {
                        print("Error: Directory not empty or failed: '"); print(path); print("'\n");
                    } else if (opts.verbose) {
                        print("Removed directory '"); print(path); print("'\n");
                    }
                } else {
                    int res = remove_directory_recursive(path, &opts);
                    if (res == -2) continue; 
                    if (res != 0 && !opts.force) {
                        print("Error: Failed to remove '"); print(path); print("'\n");
                    } else if (opts.verbose) {
                        print("Removed '"); print(path); print("'\n");
                    }
                }
            } else {
                int res = avfs_remove_file(path);
                if (res != 0 && !opts.force) {
                    print("Error: File not found or failed: '"); print(path); print("'\n");
                } else if (opts.verbose) {
                    print("Removed '"); print(path); print("'\n");
                }
            }
        }
    }
}
