#include "test.h"
#include "mouse.h"
#include "console.h"
#include "file.h"
#include "vfs.h"
#include "sched.h"
#include "kstring.h"
#include "kerrno.h"

// The tests are the mouse: they feed the driver the bytes one would send.
static void send_packet(uint8_t first, uint8_t x, uint8_t y) {
    mouse_handle_byte(first);
    mouse_handle_byte(x);
    mouse_handle_byte(y);
}

// One event, as read from the device
static bool next_event(int *buttons, int *dx, int *dy) {
    uint8_t event[MOUSE_EVENT_SIZE];
    if (mouse_read(event, sizeof(event), false) != MOUSE_EVENT_SIZE) return false;
    *buttons = event[0];
    *dx = (int8_t)event[1];
    *dy = (int8_t)event[2];
    return true;
}

TEST(mouse_packets_become_events) {
    int buttons = 0, dx = 0, dy = 0;
    mouse_flush();
    EXPECT_TRUE(!next_event(&buttons, &dx, &dy));

    send_packet(0x08, 5, 3);                         // Right 5, up 3
    EXPECT_TRUE(next_event(&buttons, &dx, &dy));
    EXPECT_TRUE(buttons == 0x08 && dx == 5 && dy == 3);

    // Negative movements: the sign is in the first byte, the rest in two's complement
    send_packet(0x08 | 0x10 | 0x20, (uint8_t)-7, (uint8_t)-2);
    EXPECT_TRUE(next_event(&buttons, &dx, &dy));
    EXPECT_TRUE(dx == -7 && dy == -2);

    // A press with no movement is an event; the same state again is not
    send_packet(0x08 | MOUSE_LEFT, 0, 0);
    send_packet(0x08 | MOUSE_LEFT, 0, 0);
    EXPECT_TRUE(next_event(&buttons, &dx, &dy));
    EXPECT_TRUE(buttons == (0x08 | MOUSE_LEFT) && dx == 0 && dy == 0);
    EXPECT_TRUE(!next_event(&buttons, &dx, &dy));

    // Movements with the buttons unchanged add up while nobody reads; a
    // change of buttons starts a new event, so a click keeps its place
    send_packet(0x08 | MOUSE_LEFT, 10, 0);
    send_packet(0x08 | MOUSE_LEFT, 10, 4);
    send_packet(0x08, 1, 0);                         // Released, one further right
    EXPECT_TRUE(next_event(&buttons, &dx, &dy));
    EXPECT_TRUE(buttons == (0x08 | MOUSE_LEFT) && dx == 20 && dy == 4);
    EXPECT_TRUE(next_event(&buttons, &dx, &dy));
    EXPECT_TRUE(buttons == 0x08 && dx == 1 && dy == 0);

    // More than one event can carry is handed out in several
    send_packet(0x08, 120, 0);
    send_packet(0x08, 120, 0);
    send_packet(0x08 | 0x10, (uint8_t)(256 - 200), 0);   // 200 to the left: needs the ninth bit
    int total = 0, events = 0;
    while (next_event(&buttons, &dx, &dy)) {
        EXPECT_TRUE(dx >= -127 && dx <= 127);
        total += dx;
        events++;
    }
    EXPECT_EQ(total, 40);
    EXPECT_EQ(events, 1);                                // 120 + 120 - 200 was added up before anyone read
    send_packet(0x08, 127, 0);
    send_packet(0x08, 127, 0);
    send_packet(0x08, 127, 0);
    total = events = 0;
    while (next_event(&buttons, &dx, &dy)) { total += dx; events++; }
    EXPECT_TRUE(total == 381 && events == 3);

    // A buffer too small for one event is refused; a larger one takes whole events
    uint8_t small[2], several[8];
    EXPECT_EQ(mouse_read(small, sizeof(small), false), -EINVAL);
    send_packet(0x08 | MOUSE_RIGHT, 1, 1);
    send_packet(0x08, 1, 1);
    send_packet(0x08 | MOUSE_MIDDLE, 1, 1);
    EXPECT_EQ(mouse_read(several, sizeof(several), false), 6);
    EXPECT_TRUE(several[0] == (0x08 | MOUSE_RIGHT) && several[3] == 0x08);
    EXPECT_EQ(mouse_read(several, sizeof(several), false), 3);
    mouse_flush();
}

// Bytes can be lost, and then the driver is reading the middle of a packet
// as its start. Bit 3 of a first byte is always set; movement bytes mostly
// do not have it.
TEST(mouse_finds_the_start_of_a_packet_again) {
    int buttons = 0, dx = 0, dy = 0;
    mouse_flush();

    mouse_handle_byte(0x02);                         // Not a first byte: dropped
    mouse_handle_byte(0x00);                         // Nor this
    send_packet(0x08, 4, 4);
    EXPECT_TRUE(next_event(&buttons, &dx, &dy));
    EXPECT_TRUE(dx == 4 && dy == 4);
    EXPECT_TRUE(!next_event(&buttons, &dx, &dy));

    // Overflow: the device says its numbers are meaningless. The buttons
    // still count.
    send_packet(0x08 | 0x40 | MOUSE_LEFT, 0xFF, 0xFF);
    EXPECT_TRUE(next_event(&buttons, &dx, &dy));
    EXPECT_TRUE(buttons == (0x08 | MOUSE_LEFT) && dx == 0 && dy == 0);

    // Far more events than the queue holds, alternating buttons so that
    // none can be merged: nothing breaks, the distance is all there, and
    // the last state of the buttons is the true one
    for (int i = 0; i < 500; i++) send_packet((uint8_t)(0x08 | (i % 2 ? MOUSE_LEFT : 0)), 1, 0);
    int total = 0, last = -1;
    while (next_event(&buttons, &dx, &dy)) { total += dx; last = buttons; }
    EXPECT_EQ(total, 500);
    EXPECT_EQ(last, 0x08 | MOUSE_LEFT);
    mouse_flush();
}

static volatile int64_t reader_got;
static volatile uint8_t reader_event[MOUSE_EVENT_SIZE];
static task_t *reader_task;

// Reads one event from the mouse, sleeping until there is one
static void blocked_reader(void) {
    uint8_t event[MOUSE_EVENT_SIZE];
    int64_t n = mouse_read(event, sizeof(event), true);
    for (int i = 0; i < MOUSE_EVENT_SIZE; i++) reader_event[i] = event[i];
    reader_got = n;
}

TEST(mouse_read_sleeps_until_the_mouse_moves) {
    mouse_flush();
    reader_got = -1;
    reader_task = task_create("mouse-reader", blocked_reader);
    EXPECT_TRUE(reader_task != NULL);
    TEST_WAIT(reader_task->state == TASK_BLOCKED, 20);
    sched_sleep_ticks(5);
    EXPECT_EQ(reader_got, -1);                       // Still waiting, and not spinning

    send_packet(0x08 | MOUSE_LEFT, 9, 0);
    TEST_WAIT(reader_got != -1, 50);
    EXPECT_EQ(reader_got, MOUSE_EVENT_SIZE);
    EXPECT_TRUE(reader_event[0] == (0x08 | MOUSE_LEFT) && reader_event[1] == 9);
    size_t reaped = 0;
    TEST_WAIT((reaped += sched_reap()) >= 1, 100);
    mouse_flush();
}

// /dev/fb0 is the screen. While a program has it open for writing the
// console keeps its hands off video memory, and takes the screen back when
// the program lets go.
TEST(framebuffer_device_and_console_take_turns) {
    uint8_t *pixels;
    uint32_t width, height, pitch;
    EXPECT_TRUE(console_framebuffer(&pixels, &width, &height, &pitch));
    if (!console_framebuffer(&pixels, &width, &height, &pitch)) return;
    EXPECT_TRUE(width >= 320 && height >= 200 && pitch >= width * 4);
    size_t size = (size_t)pitch * height;

    vnode_t *node = vfs_find(NULL, "/dev/fb0");
    EXPECT_TRUE(node != NULL && node->size == size && vfs_find(NULL, "/dev/mouse") != NULL);

    // Reading does not take the screen
    file_t *reader = NULL;
    EXPECT_EQ(file_open("/dev/fb0", O_RDONLY, &reader), 0);
    EXPECT_TRUE(!console_graphics_active());
    static uint32_t row[64], back[64];
    EXPECT_EQ(file_read(reader, row, sizeof(row)), (int64_t)sizeof(row));
    EXPECT_EQ(memcmp(row, pixels, sizeof(row)), 0);
    file_close(reader);

    // Writing does. Paint the top left corner a colour no text has.
    file_t *screen = NULL;
    EXPECT_EQ(file_open("/dev/fb0", O_RDWR, &screen), 0);
    EXPECT_TRUE(console_graphics_active());
    for (size_t i = 0; i < 64; i++) row[i] = 0x00123456;
    EXPECT_EQ(file_write(screen, row, sizeof(row)), (int64_t)sizeof(row));
    EXPECT_EQ(((uint32_t *)pixels)[0], 0x00123456u);
    EXPECT_EQ(((uint32_t *)pixels)[63], 0x00123456u);

    // Console output now: enough of it to scroll the whole screen away.
    // It must not reach video memory.
    for (uint32_t i = 0; i < height / 8 + 5; i++) kprintf("    (output while a program has the screen %u)\n", i);
    console_refresh();
    EXPECT_EQ(((uint32_t *)pixels)[0], 0x00123456u);
    EXPECT_EQ(((uint32_t *)pixels)[63], 0x00123456u);
    EXPECT_TRUE(!console_needs_refresh());

    // The file reads back what was written; its end is the screen's end
    EXPECT_EQ(file_seek(screen, 0, SEEK_SET), 0);
    EXPECT_EQ(file_read(screen, back, sizeof(back)), (int64_t)sizeof(back));
    EXPECT_EQ(memcmp(back, row, sizeof(row)), 0);
    EXPECT_EQ(file_seek(screen, (int64_t)size - 8, SEEK_SET), (int64_t)size - 8);
    EXPECT_EQ(file_read(screen, back, sizeof(back)), 8);            // Cut short at the end
    EXPECT_EQ(file_read(screen, back, sizeof(back)), 0);
    EXPECT_EQ(file_write(screen, row, sizeof(row)), -ENOSPC);       // Nothing beyond it

    // A second program with it open: the screen comes back only when both
    // have let go
    file_t *second = NULL;
    EXPECT_EQ(file_open("/dev/fb0", O_WRONLY, &second), 0);
    file_close(screen);
    EXPECT_TRUE(console_graphics_active());
    console_refresh();
    EXPECT_EQ(((uint32_t *)pixels)[0], 0x00123456u);
    file_close(second);
    EXPECT_TRUE(!console_graphics_active());

    // And then the text is back, all of it
    console_refresh();
    EXPECT_TRUE(((uint32_t *)pixels)[0] != 0x00123456u);
    size_t different = 0;
    for (uint32_t y = 0; y < height; y += 7) {
        for (uint32_t x = 0; x < width; x += 5) {
            if (((uint32_t *)(pixels + (size_t)y * pitch))[x] != console_get_pixel(x, y)) different++;
        }
    }
    EXPECT_EQ(different, (size_t)0);
}

void test_suite_mouse(void) {
    kprintf("Suite: Mouse and framebuffer devices\n");
    RUN_TEST(mouse_packets_become_events);
    RUN_TEST(mouse_finds_the_start_of_a_packet_again);
    RUN_TEST(mouse_read_sleeps_until_the_mouse_moves);
    RUN_TEST(framebuffer_device_and_console_take_turns);
}
