/* clear: erase the screen and put the cursor at the top left. */
#include "ulib.h"

int main(void) {
    printf("\033[2J\033[H");
    return 0;
}
