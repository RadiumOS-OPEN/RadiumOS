#include "../utility/utility.h"
#include "../terminal/terminal.h"

#define MAX_WIDTH   40
#define MAX_LINES   64
#define MAX_LINE_LEN 80

/* ------------------------------------------------------------------ */
/*  Split all argv[1..] into logical lines, respecting '\n' escapes    */
/*  and hard-wrapping at MAX_LINE_LEN-1 chars.  Argv tokens are joined */
/*  with a single space so "cowsay hello world" works naturally.       */
/* ------------------------------------------------------------------ */
static int SplitInputLines(int argc, char* argv[],
                           char all_lines[MAX_LINES][MAX_LINE_LEN])
{
    int total_lines = 0;
    int line_pos    = 0;

    for (int i = 1; i < argc && total_lines < MAX_LINES; i++) {
        if (!argv[i]) continue;

        /* space-separate tokens (skip leading space on first token) */
        if (i > 1 && line_pos < MAX_LINE_LEN - 1)
            all_lines[total_lines][line_pos++] = ' ';

        for (char* p = argv[i]; *p && total_lines < MAX_LINES; p++) {

            if (*p == '\\' && *(p + 1) == 'n') {
                /* explicit \n escape sequence */
                all_lines[total_lines][line_pos] = '\0';
                total_lines++;
                line_pos = 0;
                p++;   /* skip the 'n' */
                continue;
            }

            if (*p == '\n') {
                /* real newline byte */
                all_lines[total_lines][line_pos] = '\0';
                total_lines++;
                line_pos = 0;
                continue;
            }

            all_lines[total_lines][line_pos++] = *p;

            /* hard-wrap when we hit the column limit */
            if (line_pos >= MAX_LINE_LEN - 1) {
                all_lines[total_lines][line_pos] = '\0';
                total_lines++;
                line_pos = 0;
            }
        }
    }

    /* flush the last partial line */
    if (line_pos > 0 && total_lines < MAX_LINES) {
        all_lines[total_lines][line_pos] = '\0';
        total_lines++;
    }

    return total_lines;
}

/* ------------------------------------------------------------------ */
/*  Print a horizontal rule of `width` dashes with given end-caps      */
/* ------------------------------------------------------------------ */
static void PrintBorderLine(char left, char fill, char right, size_t width)
{
    printr("%c", left);
    for (size_t i = 0; i < width; i++) printr("%c", fill);
    printr("%c\n", right);
}

/* ------------------------------------------------------------------ */
/*  cowsay                                                             */
/* ------------------------------------------------------------------ */
void cowsay_command(int argc, char* argv[])
{
    if (argc == 1) {
        printr("Usage: cowsay [message]\n");
        return;
    }

    char all_lines[MAX_LINES][MAX_LINE_LEN];
    int  total_lines = SplitInputLines(argc, argv, all_lines);

    if (total_lines == 0) {
        printr("No message to display.\n");
        return;
    }
    print("\n");
    /* ---- find display width (capped at MAX_WIDTH) ----------------- */
    size_t box_w = 0;
    for (int i = 0; i < total_lines; i++) {
        size_t len = strlen(all_lines[i]);
        if (len > box_w) box_w = len;
    }
    if (box_w > MAX_WIDTH) box_w = MAX_WIDTH;

    /* ---- top border:  / _________ \  ----------------------------- */
    printr(" ");
    PrintBorderLine('/', '_', '\\', box_w + 2);

    /* ---- message lines -------------------------------------------- */
    for (int i = 0; i < total_lines; i++) {
        size_t len = strlen(all_lines[i]);

        /* choose side-glyph: single line → < >, multi → | | */
        char L = (total_lines == 1) ? '<' : '|';
        char R = (total_lines == 1) ? '>' : '|';

        printr("%c ", L);

        if (len > MAX_WIDTH) {
            for (size_t c = 0; c < MAX_WIDTH; c++)
                printr("%c", all_lines[i][c]);
            len = MAX_WIDTH;
        } else {
            printr("%s", all_lines[i]);
        }

        /* right-pad to box_w */
        for (size_t j = len; j < box_w; j++) printr(" ");

        printr(" %c\n", R);
    }

    /* ---- bottom border:  \ --------- /  -------------------------- */
    printr(" ");
    PrintBorderLine('\\', '-', '/', box_w + 2);

    /* ---- cow art -------------------------------------------------- */
    printr("      \\   ^__^\n");
    printr("       \\  (oo)\\_______\n");
    printr("          (__)\\       )\\/\\\n");
    printr("              ||----w |\n");
    printr("              ||     ||\n");

    /* ---- attribution ---------------------------------------------- */
    char c[2] = { 0x4, '\0' };
    print(c);
    print("\n");
}