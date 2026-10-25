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
#include "smp.h"
#include "interrupts.h"


#define FEATURE_MAC (1ULL << 5) // The card has a MAC address in its configuration space

#define QUEUE_RX 0
#define QUEUE_TX 1

#define RX_BUFFERS 32
#define TX_BUFFERS 16
#define MAX_QUEUE_SIZE 1024    // Size of the descriptor-to-buffer tables below

// Every frame on a virtqueue is preceded by this header. It carries
// offloading information (checksums, segmentation) that this driver does not
// use: it is sent as zeros and ignored on receive.
//
// Its length depends on the interface (virtio 1.2, section 5.1.6): the last
// field exists on a modern device always, and on a legacy one only if a
// feature we do not ask for (merged receive buffers) was negotiated.
struct virtio_net_header {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t header_len;
    uint16_t gso_size;
    uint16_t checksum_start;
    uint16_t checksum_offset;
    uint16_t num_buffers;
} __attribute__((packed));

#define HEADER_SIZE_MODERN sizeof(struct virtio_net_header)
#define HEADER_SIZE_LEGACY (sizeof(struct virtio_net_header) - sizeof(uint16_t))

static bool present = false;
static virtio_device_t card;
static size_t header_size;      // Of the two above, the one this card uses
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

// The card's interrupt handler: wake the receive thread if frames came in
static void virtio_net_irq(struct registers *regs) {
    (void)regs;
    // Reading the ISR register makes the card lower its interrupt line
    uint8_t isr = virtio_isr(&card);
    random_add_event(isr); // Packets arrive when the rest of the world sends them
    if (isr & 1) {
        // Under the lock, so that a receiver between "nothing there" and
        // "asleep" (virtio_net_wait_rx) can not miss this
        uint64_t flags = spinlock_acquire_irqsave(&card_lock);
        sched_wakeup(&rx_queue);
        spinlock_release_irqrestore(&card_lock, flags);
    }
}

// The receive queue's MSI-X handler: frames came in (the vector is the
// queue's own, so nothing needs asking; with MSI-X the ISR register is
// not used, virtio 1.2, 4.1.4.5)
static void virtio_net_msi(void *arg) {
    (void)arg;
    random_add_event(rx_count);
    uint64_t flags = spinlock_acquire_irqsave(&card_lock);
    sched_wakeup(&rx_queue);
    spinlock_release_irqrestore(&card_lock, flags);
}

// Give receive buffer number `buffer` to the card to fill. Caller holds
// card_lock (or is setting the card up). False if the queue is full.
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
    if (!virtio_pci_find(VIRTIO_TYPE_NET, 0, &dev)) {
        kprintf("virtio-net: no network card attached\n");
        return -1;
    }

    // The only optional feature we want is the MAC address
    uint64_t features = 0;
    if (!virtio_open(&card, &dev) || !virtio_negotiate(&card, FEATURE_MAC, &features)) {
        kprintf("virtio-net: device offers no interface this kernel can use\n");
        return -1;
    }
    header_size = card.modern ? HEADER_SIZE_MODERN : HEADER_SIZE_LEGACY;

    // Received frames get a vector of their own if the card can send
    // messages. Sent ones need none: their buffers are collected by the
    // next send, so the transmit queue raises no interrupt at all.
    if (virtio_msix_start(&card)) {
        uint8_t vector = register_msi_handler("virtio-net rx", virtio_net_msi, NULL);
        if (!vector || !virtio_msix_assign(&card, QUEUE_RX, 0, vector, smp_next_interrupt_cpu())) {
            card.queue_vector[QUEUE_RX] = VIRTIO_MSI_NO_VECTOR;
        }
    }

    bool ok = virtqueue_setup(&rx_queue, &card, QUEUE_RX) && virtqueue_setup(&tx_queue, &card, QUEUE_TX) &&
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
        virtio_fail(&card);
        return -1;
    }

    if (features & FEATURE_MAC) {
        for (int i = 0; i < 6; i++) mac_address[i] = virtio_config8(&card, (uint32_t)i);
    } else {
        // No address given: make one up in the "locally administered" range
        static const uint8_t fallback[6] = { 0x02, 0x00, 0x00, 0xBA, 0x5C, 0xE7 };
        memcpy(mac_address, fallback, 6);
    }

    uint8_t irq = card.irq;
    if (!virtio_msix_finish(&card) && irq > 0 && irq < 16) {
        register_irq_handler(irq, virtio_net_irq);
        ioapic_route_isa_irq_to(irq, smp_next_interrupt_cpu());
    }

    virtio_driver_ok(&card);

    // Give the card somewhere to put incoming frames
    for (unsigned i = 0; i < RX_BUFFERS && i < rx_queue.size; i++) post_rx_buffer(i);
    virtqueue_kick(&rx_queue);

    present = true;
    char how[48];
    virtio_describe_interrupts(&card, how, sizeof(how));
    kprintf("virtio-net: card ready, MAC %02x:%02x:%02x:%02x:%02x:%02x, %s interface, interrupts: %s\n",
            mac_address[0], mac_address[1], mac_address[2], mac_address[3], mac_address[4], mac_address[5],
            virtio_interface_name(&card), how);
    return 0;
}

bool virtio_net_present(void) { return present; }
void virtio_net_get_mac(uint8_t mac[6]) { memcpy(mac, mac_address, 6); }
uint64_t virtio_net_rx_count(void) { return rx_count; }
uint64_t virtio_net_tx_count(void) { return tx_count; }

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
            if (len > header_size && len <= PAGE_SIZE) {
                result = len - header_size;
                if (result > max) result = max;
                const uint8_t *data = (const uint8_t *)(rx_phys[buffer] + pmm_get_hhdm_offset());
                memcpy(frame, data + header_size, result);
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

        memset(data, 0, header_size + frame_len);   // Zero header, and zero padding for a short frame
        memcpy(data + header_size, frame, len);

        virtqueue_buffer_t b = { tx_phys[buffer], (uint32_t)(header_size + frame_len) };
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
