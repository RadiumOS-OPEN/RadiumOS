// Entry point for the RadiumOS build: the engine's main() is renamed ac_main (-Dmain=ac_main for main.cpp only) so that sensible
// defaults for the software renderer come first on the command line: `app /disk/ac.app` plays in 640x360, enlarged 2x to 1280x720.
#include <stdlib.h>
#include <stdio.h>
int ac_main(int argc, char **argv);

int main(int argc, char **argv)
{
    static char *args[40];
    int n = 0;
    args[n++] = argc > 0 ? argv[0] : (char *)"ac";
    static char home[200];
    snprintf(home, sizeof(home), "--home=%s", getenv("AC_HOME") ? getenv("AC_HOME") : "/user");   // AC_HOME: host test build only
    args[n++] = home;
    args[n++] = (char *)"-w640";
    args[n++] = (char *)"-h360";
    args[n++] = (char *)"-t0";
    // display lists are not implemented by softgl; with no arguments start a bot deathmatch on ac_desert right away,
    // `app /disk/ac.app HOST [PORT]` joins a server instead
    static char join[160];
    int first = 1;
    if (argc >= 2 && argv[1][0] != '-') {
        snprintf(join, sizeof(join), "-emdldlist 0; mapstartonce = []; bind F12 hmods; sleep 1500 [ connect %s %s ]", argv[1], argc >= 3 && argv[2][0] != '-' ? argv[2] : "0");
        args[n++] = join;
        first = argc >= 3 && argv[2][0] != '-' ? 3 : 2;
    } else {
        args[n++] = (char *)(argc < 2 ? "-emdldlist 0; mapstartonce = []; bind F12 hmods; sleep 1500 [ tmp_mode = 8; tmp_isbotteammode = 0; tmp_botmap = ac_desert; tmp_numbots = 4; startbotmap ]"
                                      : "-emdldlist 0; mapstartonce = []; bind F12 hmods");
    }
    for (int i = first; i < argc && n < 38; i++) args[n++] = argv[i];
    args[n] = 0;
    return ac_main(n, args);
}
