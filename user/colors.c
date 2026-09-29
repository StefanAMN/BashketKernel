/* colors: show the sixteen terminal colours as text and as background. */
#include "ulib.h"

int main(void) {
    static const char *const names[8] = { "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white" };

    for (int i = 0; i < 8; i++) {
        /* ESC [ 3x m sets the text colour, 9x the bright one, 4x the background */
        printf("\033[%dm%8s\033[0m  \033[%dmbright %s\033[0m  \033[%dm      \033[0m\n",
               30 + i, names[i], 90 + i, names[i], 40 + i);
    }
    printf("\033[1;33mbold yellow\033[0m and back to normal\n");
    return 0;
}
