// Pieces of Cataclysm: DDA's platform layer that the curses build gets from SDL or Windows code.
#include "cursesdef.h"
#include "point.h"

bool window_contains_point_relative( const catacurses::window &, const point & )
{
    return false;   // no mouse yet
}

// thread_local destructors: the app is single threaded and the process ends by leaving radium_app_main
extern "C" int __cxa_thread_atexit_impl( void ( * )( void * ), void *, void * )
{
    return 0;
}
