// Stand-in for the platform part of SDL_syswm.h (the bundled one needs windows.h): AssaultCube only looks at it under X11.
#ifndef SDL_syswm_h_
#define SDL_syswm_h_
#include <SDL_stdinc.h>
#include <SDL_video.h>
#include <SDL_version.h>
typedef enum { SDL_SYSWM_UNKNOWN } SDL_SYSWM_TYPE;
typedef struct SDL_SysWMinfo { SDL_version version; SDL_SYSWM_TYPE subsystem; } SDL_SysWMinfo;
#endif
