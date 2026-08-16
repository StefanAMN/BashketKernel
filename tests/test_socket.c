#include "test.h"
#include "socket.h"

TEST(socket_checksum_send_test) {
    uint8_t payload[] = {0x45, 0x00, 0x00, 0x1C, 0x00, 0x01, 0x00, 0x00};
    uint16_t csum = net_checksum(payload, sizeof(payload));
    EXPECT_TRUE(csum != 0);

    socket_t *sock = socket_create(8080);
    EXPECT_TRUE(sock != NULL);

    int sent = socket_sendto(sock, 0x0A000202, 8080, payload, sizeof(payload));
    EXPECT_TRUE(sent == (int)sizeof(payload));
}

void test_suite_socket(void) {
    kprintf("Suite: Phase 29 UDP/IP Network Socket Stack\n");
    RUN_TEST(socket_checksum_send_test);
}
