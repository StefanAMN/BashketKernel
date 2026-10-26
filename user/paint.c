/* paint: draw on the screen with the mouse.
 *
 *   paint
 *
 * The program has the screen to itself (/dev/fb0, mapped into its memory)
 * and reads the mouse (/dev/mouse). Hold the left button to draw; click a
 * colour along the top to draw with it; the right button, a click on the
 * box in the top right corner, or a line typed starting with q, ends the
 * program, and the text screen comes back.
 *
 * What it shows about the system: the screen is memory a program writes
 * to (the same pixels /dev/fb0 reads and writes as a file), the mouse is a
 * file of movements that it reads, poll() waits for the mouse and the
 * keyboard at once, and the pointer is something the program itself draws.
 * A window system is this, with more programs.
 */
#include "ulib.h"

#define BAR_HEIGHT   32          /* The strip of colours along the top */
#define BOX_WIDTH    48
#define BACKGROUND   0x00202428u
#define POINTER_SIZE 9           /* The pointer is a plus sign this many pixels across */

static const unsigned palette[] = { 0x00FFFFFFu, 0x00E04040u, 0x0040C060u, 0x004080F0u, 0x00F0D040u, 0x00C060E0u, BACKGROUND };
#define COLOURS (int)(sizeof(palette) / sizeof(palette[0]))

static struct fb_info screen;
static int fb = -1;
static unsigned char *pixels;    /* The screen itself, mapped (pitch bytes per row) */
static unsigned *canvas;         /* What is on the screen, less the pointer: needed to take the pointer off again */

/* Send one row segment of the canvas to the screen */
static void show(int x, int y, int width) {
    if (y < 0 || y >= (int)screen.height) return;
    if (x < 0) { width += x; x = 0; }
    if (x + width > (int)screen.width) width = (int)screen.width - x;
    if (width <= 0) return;
    memcpy(pixels + (long)y * screen.pitch + (long)x * 4, canvas + (long)y * screen.width + x, (unsigned long)width * 4);
}

/* Fill a rectangle of the canvas with `colour` and put it on the screen */
static void fill(int x0, int y0, int x1, int y1, unsigned colour) {
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) canvas[(long)y * screen.width + x] = colour;
        show(x0, y, x1 - x0);
    }
}

/* A 3x3 dot of the drawing colour, kept off the bar */
static void dot(int x, int y, unsigned colour) {
    for (int row = y - 1; row <= y + 1; row++) {
        if (row < BAR_HEIGHT || row >= (int)screen.height) continue;
        for (int col = x - 1; col <= x + 1; col++) {
            if (col >= 0 && col < (int)screen.width) canvas[(long)row * screen.width + col] = colour;
        }
        show(x - 1, row, 3);
    }
}

/* A line of dots from one point to another (Bresenham's algorithm: step
 * along the longer axis, and along the other whenever the error says so) */
static void line(int x0, int y0, int x1, int y1, unsigned colour) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int step_x = x0 < x1 ? 1 : -1, step_y = y0 < y1 ? 1 : -1;
    int error = dx - dy;
    for (;;) {
        dot(x0, y0, colour);
        if (x0 == x1 && y0 == y1) break;
        int twice = 2 * error;
        if (twice > -dy) { error -= dy; x0 += step_x; }
        if (twice < dx) { error += dx; y0 += step_y; }
    }
}

/* The pointer is drawn straight to the screen, never into the canvas, so
 * taking it away is showing the canvas again */
static void pointer(int x, int y, int visible) {
    unsigned row[POINTER_SIZE];
    for (int i = -POINTER_SIZE / 2; i <= POINTER_SIZE / 2; i++) {
        int py = y + i;
        if (py < 0 || py >= (int)screen.height) continue;
        if (!visible) {
            show(x - POINTER_SIZE / 2, py, POINTER_SIZE);
            continue;
        }
        /* The horizontal stroke on the middle row, one pixel of the
         * vertical stroke on the others; black edged, so it shows on
         * any colour */
        int from = i == 0 ? x - POINTER_SIZE / 2 : x, count = i == 0 ? POINTER_SIZE : 1;
        if (from < 0) { count += from; from = 0; }
        if (from + count > (int)screen.width) count = (int)screen.width - from;
        if (count <= 0) continue;
        for (int k = 0; k < count; k++) row[k] = ((i + k) & 1) ? 0x00000000u : 0x00FFFFFFu;
        memcpy(pixels + (long)py * screen.pitch + (long)from * 4, row, (unsigned long)count * 4);
    }
}

/* Draw the palette along the top, with the chosen colour's box larger */
static void draw_bar(int chosen) {
    fill(0, 0, (int)screen.width, BAR_HEIGHT, 0x00101010u);
    for (int i = 0; i < COLOURS; i++) {
        int x = 4 + i * BOX_WIDTH;
        /* The chosen colour's box is larger */
        int inset = i == chosen ? 2 : 8;
        fill(x + inset, inset, x + BOX_WIDTH - 4 - inset + 4, BAR_HEIGHT - inset, palette[i]);
    }
    /* The way out: a red box with a white middle, top right */
    fill((int)screen.width - BOX_WIDTH, 4, (int)screen.width - 4, BAR_HEIGHT - 4, 0x00C02020u);
    fill((int)screen.width - BOX_WIDTH + 14, 12, (int)screen.width - 18, BAR_HEIGHT - 12, 0x00FFFFFFu);
}

int main(void) {
    if (fb_info(&screen) != 0 || screen.bpp != 32) {
        fprintf(2, "paint: no screen to draw on\n");
        return 1;
    }
    int mouse = (int)open("/dev/mouse", O_RDONLY);
    if (mouse < 0) {
        fprintf(2, "paint: no mouse\n");
        return 1;
    }
    canvas = malloc((unsigned long)screen.width * screen.height * 4);
    if (!canvas) {
        fprintf(2, "paint: not enough memory for a %ux%u picture\n", screen.width, screen.height);
        return 1;
    }
    /* Opening the screen for writing takes it from the console; closing it
     * (or dying) gives it back */
    fb = (int)open("/dev/fb0", O_RDWR);
    if (fb < 0) {
        fprintf(2, "paint: cannot open /dev/fb0\n");
        return 1;
    }
    /* The screen as memory: the mapping holds it too, until it is unmapped */
    size_t screen_bytes = (size_t)screen.pitch * screen.height;
    pixels = mmap_file_shared(fb, screen_bytes, 0, PROT_READ | PROT_WRITE);
    if (!pixels) {
        fprintf(2, "paint: cannot map /dev/fb0\n");
        return 1;
    }

    int chosen = 0;
    fill(0, BAR_HEIGHT, (int)screen.width, (int)screen.height, BACKGROUND);
    draw_bar(chosen);

    int x = (int)screen.width / 2, y = (int)screen.height / 2;
    int was_down = 0, strokes = 0;
    pointer(x, y, 1);
    printf("paint: %ux%u, pointer at %d,%d\n", screen.width, screen.height, x, y); /* (To the serial port: the screen is ours) */

    for (;;) {
        /* The mouse and the keyboard at once: whichever has something */
        struct pollfd ready[2] = { { mouse, POLLIN, 0 }, { 0, POLLIN, 0 } };
        if (poll(ready, 2, -1) < 0) continue;
        if (ready[1].revents & POLLIN) {
            char typed[64];
            long n = read(0, typed, sizeof(typed));
            if (n <= 0 || typed[0] == 'q') goto done;    /* A line starting with q, or the end of input */
        }
        if (!(ready[0].revents & POLLIN)) continue;
        unsigned char events[3 * 16];
        long got = read(mouse, events, sizeof(events));
        if (got < 3) break;

        for (long at = 0; at + 3 <= got; at += 3) {
            int buttons = events[at] & 7;
            int new_x = x + (signed char)events[at + 1];
            int new_y = y - (signed char)events[at + 2];   /* The mouse counts upwards, the screen downwards */
            if (new_x < 0) new_x = 0;
            if (new_y < 0) new_y = 0;
            if (new_x >= (int)screen.width) new_x = (int)screen.width - 1;
            if (new_y >= (int)screen.height) new_y = (int)screen.height - 1;

            pointer(x, y, 0);
            int down = buttons & 1;
            if (buttons & 2) goto done;
            if (down && !was_down && new_y < BAR_HEIGHT) {
                /* A click on the bar: the way out, or a colour */
                if (new_x >= (int)screen.width - BOX_WIDTH) goto done;
                int box = (new_x - 4) / BOX_WIDTH;
                if (new_x >= 4 && box < COLOURS) {
                    chosen = box;
                    draw_bar(chosen);
                }
            } else if (down && new_y >= BAR_HEIGHT) {
                /* Held: join where the pointer was to where it is. Just
                 * pressed: a dot. */
                if (was_down && y >= BAR_HEIGHT) line(x, y, new_x, new_y, palette[chosen]);
                else dot(new_x, new_y, palette[chosen]);
                if (!was_down) strokes++;
            }
            was_down = down;
            x = new_x;
            y = new_y;
            pointer(x, y, 1);
        }
    }
done:
    munmap(pixels, screen_bytes);
    close(fb);   /* Neither holds the screen now: the console repaints its text */
    close(mouse);
    printf("paint: %d strokes; pointer last at %d,%d\n", strokes, x, y);
    return 0;
}
