#ifndef BOOTSPLASH_H
#define BOOTSPLASH_H

/**
 * @file bootsplash.h
 * @brief Terminal-based boot loading screen for RadiumOS
 *
 * Renders a centered ASCII-art radiation logo, the "RadiumOS" wordmark,
 * and a copyright line pinned to the bottom-left corner of the screen —
 * the terminal equivalent of a classic OS loading screen.
 */

/**
 * @brief Draw the full boot splash screen.
 *
 * Clears the terminal, then draws the centered logo/wordmark and the
 * bottom-left copyright line, repainting for a fixed hold duration.
 * Called once at boot, from kernel_main() — not exposed as a shell
 * command.
 */
void show_boot_splash(void);

#endif // BOOTSPLASH_H