/**
 * @file virtio_net.c
 * @brief virtio network card driver (see virtio_net.h)
 */

#include "virtio_net.h"
#include "random.h"
#include "virtqueue.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "kprintf.h"
#include "kstring.h"
#include "sched.h"
#include "spinlock.h"
#include "ioapic.h"
#include "interrupts.h"

extern void register_irq_handler(uint8_t irq, void (*handler)(struct registers *regs));

#define VIRTIO_VENDOR        0x1AF4
#define VIRTIO_NET_LEGACY_ID 0x1000

#define FEATURE_MAC (1u << 5)  // The card has a MAC address in its configuration space

#define QUEUE_RX 0
#define QUEUE_TX 1

#define RX_BUFFERS 32
#define TX_BUFFERS 16
#define MAX_QUEUE_SIZE 1024    // Size of the descriptor-to-buffer tables below

// Every frame on a virtqueue is preceded by this header. It carries
// offloading information (checksums, segmentation) that this driver does not
// use: it is sent as zeros and ignored on receive.
struct virtio_net_header {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t header_len;
    uint16_t gso_size;
    uint16_t checksum_start;
    uint16_t checksum_offset;
} __attribute__((packed));

#define HEADER_SIZE sizeof(struct virtio_net_header)

static bool present = false;
static uint16_t io_base;
static uint8_t mac_address[6];

static virtqueue_t rx_queue, tx_queue;

// One page per buffer. Which buffer a finished request used is found through
// the request id (its descriptor number).
static uint64_t rx_phys[RX_BUFFERS], tx_phys[TX_BUFFERS];
static uint8_t rx_buffer_of[MAX_QUEUE_SIZE], tx_buffer_of[MAX_QUEUE_SIZE];
static bool tx_in_use[TX_BUFFERS];

static uint64_t rx_count = 0, tx_count = 0;

// Protects both virtqueues and the buffer tables above
static spinlock_t card_lock = SPINLOCK_RANKED("virtio-net", LOCK_RANK_DEVICE);

static void virtio_net_irq(struct registers *regs) {
    (void)regs;
    // Reading the ISR register makes the card lower its interrupt line
    uint8_t isr = inb(io_base + VIRTIO_REG_ISR_STATUS);
    random_add_event(isr); // Packets arrive when the rest of the world sends them
    if (isr & 1) {
        // Under the lock, so that a receiver between "nothing there" and
        // "asleep" (virtio_net_wait_rx) can not miss this
        uint64_t flags = spinlock_acquire_irqsave(&card_lock);
        sched_wakeup(&rx_queue);
        spinlock_release_irqrestore(&card_lock, flags);
    }
}

static bool post_rx_buffer(unsigned buffer) {
    virtqueue_buffer_t b = { rx_phys[buffer], PAGE_SIZE };
    int id = virtqueue_add(&rx_queue, &b, 0, 1); // One buffer, written by the card
    if (id < 0 || id >= MAX_QUEUE_SIZE) return false;
    rx_buffer_of[id] = (uint8_t)buffer;
    return true;
}

int virtio_net_init(void) {
    if (present) return 0;
    pci_init();

    pci_device_t dev;
    if (!pci_find_device(VIRTIO_VENDOR, VIRTIO_NET_LEGACY_ID, &dev)) {
        kprintf("virtio-net: no network card attached\n");
        return -1;
    }

    uint32_t bar0 = pci_read32(&dev, PCI_BAR0);
    if (!(bar0 & 1)) {
        kprintf("virtio-net: device has no legacy I/O port interface\n");
        return -1;
    }
    io_base = (uint16_t)(bar0 & ~3u);
    pci_enable_io_and_bus_master(&dev);

    outb(io_base + VIRTIO_REG_DEVICE_STATUS, 0);
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // The only optional feature we want is the MAC address
    uint32_t features = inl(io_base + VIRTIO_REG_DEVICE_FEATURES);
    outl(io_base + VIRTIO_REG_DRIVER_FEATURES, features & FEATURE_MAC);

    bool ok = virtqueue_setup(&rx_queue, io_base, QUEUE_RX) && virtqueue_setup(&tx_queue, io_base, QUEUE_TX) &&
              rx_queue.size <= MAX_QUEUE_SIZE && tx_queue.size <= MAX_QUEUE_SIZE;

    for (unsigned i = 0; ok && i < RX_BUFFERS; i++) {
        rx_phys[i] = (uint64_t)pmm_alloc_frame();
        ok = rx_phys[i] != 0;
    }
    for (unsigned i = 0; ok && i < TX_BUFFERS; i++) {
        tx_phys[i] = (uint64_t)pmm_alloc_frame();
        ok = tx_phys[i] != 0;
    }
    if (!ok) {
        kprintf("virtio-net: could not set up the queues\n");
        outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    if (features & FEATURE_MAC) {
        for (int i = 0; i < 6; i++) mac_address[i] = inb(io_base + VIRTIO_REG_DEVICE_CONFIG + i);
    } else {
        // No address given: make one up in the "locally administered" range
        static const uint8_t fallback[6] = { 0x02, 0x00, 0x00, 0xBA, 0x5C, 0xE7 };
        memcpy(mac_address, fallback, 6);
    }

    uint8_t irq = pci_read8(&dev, PCI_INTERRUPT_LINE);
    if (irq > 0 && irq < 16) {
        register_irq_handler(irq, virtio_net_irq);
        ioapic_route_isa_irq(irq);
    }

    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    // Give the card somewhere to put incoming frames
    for (unsigned i = 0; i < RX_BUFFERS && i < rx_queue.size; i++) post_rx_buffer(i);
    virtqueue_kick(&rx_queue);

    present = true;
    kprintf("virtio-net: card ready, MAC %02x:%02x:%02x:%02x:%02x:%02x, IRQ %u\n",
            mac_address[0], mac_address[1], mac_address[2], mac_address[3], mac_address[4], mac_address[5], irq);
    return 0;
}

bool virtio_net_present(void) { return present; }
void virtio_net_get_mac(uint8_t mac[6]) { memcpy(mac, mac_address, 6); }
uint64_t virtio_net_rx_count(void) { return rx_count; }
uint64_t virtio_net_tx_count(void) { return tx_count; }

bool virtio_net_rx_pending(void) {
    return present && virtqueue_has_used(&rx_queue);
}

void virtio_net_wait_rx(void) {
    uint64_t flags = spinlock_acquire_irqsave(&card_lock);
    while (present && !virtqueue_has_used(&rx_queue)) {
        sched_sleep_uninterruptible(&rx_queue, &card_lock, 0);
    }
    spinlock_release_irqrestore(&card_lock, flags);
}

size_t virtio_net_poll(void *frame, size_t max) {
    if (!present || !frame) return 0;

    uint64_t flags = spinlock_acquire_irqsave(&card_lock);
    uint16_t id;
    uint32_t len;
    size_t result = 0;

    if (virtqueue_collect(&rx_queue, &id, &len)) {
        // Both values come from the device: check them before use
        if (id < MAX_QUEUE_SIZE && rx_buffer_of[id] < RX_BUFFERS) {
            unsigned buffer = rx_buffer_of[id];
            if (len > HEADER_SIZE && len <= PAGE_SIZE) {
                result = len - HEADER_SIZE;
                if (result > max) result = max;
                const uint8_t *data = (const uint8_t *)(rx_phys[buffer] + pmm_get_hhdm_offset());
                memcpy(frame, data + HEADER_SIZE, result);
                rx_count++;
            }
            post_rx_buffer(buffer); // Back on the queue for the next frame
            virtqueue_kick(&rx_queue);
        }
    }

    spinlock_release_irqrestore(&card_lock, flags);
    return result;
}

int virtio_net_send(const void *frame, size_t len) {
    if (!present || !frame || len < 14 || len > ETH_FRAME_MAX) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&card_lock);

    // Buffers of frames the card has finished sending are free again
    uint16_t id;
    while (virtqueue_collect(&tx_queue, &id, NULL)) {
        if (id < MAX_QUEUE_SIZE && tx_buffer_of[id] < TX_BUFFERS) tx_in_use[tx_buffer_of[id]] = false;
    }

    int buffer = -1;
    for (int i = 0; i < TX_BUFFERS; i++) {
        if (!tx_in_use[i]) { buffer = i; break; }
    }

    int result = -1;
    if (buffer >= 0) {
        uint8_t *data = (uint8_t *)(tx_phys[buffer] + pmm_get_hhdm_offset());
        size_t frame_len = len < ETH_FRAME_MIN ? ETH_FRAME_MIN : len;

        memset(data, 0, HEADER_SIZE + frame_len);   // Zero header, and zero padding for a short frame
        memcpy(data + HEADER_SIZE, frame, len);

        virtqueue_buffer_t b = { tx_phys[buffer], (uint32_t)(HEADER_SIZE + frame_len) };
        int request = virtqueue_add(&tx_queue, &b, 1, 0); // One buffer, read by the card
        if (request >= 0 && request < MAX_QUEUE_SIZE) {
            tx_buffer_of[request] = (uint8_t)buffer;
            tx_in_use[buffer] = true;
            virtqueue_kick(&tx_queue);
            tx_count++;
            result = 0;
        }
    }

    spinlock_release_irqrestore(&card_lock, flags);
    return result;
}
