// The default desktop wallpaper: the mascot (iso/boot/grub/splash.png) converted to a transparent PNG by the makefile.
// The file is empty when ImageMagick was not available at build time; the desktop then draws its plain gradient.
__asm__(
    ".section .rodata\n"
    ".balign 4\n"
    ".global wallpaper_default_png\n"
    "wallpaper_default_png:\n"
    ".incbin \"build/wallpaper.png\"\n"
    ".global wallpaper_default_png_end\n"
    "wallpaper_default_png_end:\n"
    ".byte 0\n"
    ".previous\n"
);
