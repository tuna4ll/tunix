#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../kernel/include/dma.h"
#include "../../kernel/include/module.h"
#include "../../kernel/include/pci.h"
#include "../../kernel/include/vmm.h"
#include "../../kernel/include/net/net.h"

extern const struct module_descriptor __this_module;

#define ARENA_BASE 0x30000000UL
#define ARENA_BYTES (16UL * 1024UL * 1024UL)
#define REGISTER_BYTES 0x2000UL

#define REG_PCIE_PHYMISC 0x1000U
#define PCIE_PHYMISC_FORCE_RCV_DET (1U << 2)
#define REG_PM_CTRL 0x12F8U
#define PM_CTRL_MAC_ASPM_CHK (1U << 30)
#define PM_CTRL_L1_ENTRY_TIMER_MASK 0xFU
#define PM_CTRL_L1_ENTRY_TIMER_SHIFT 16
#define PM_CTRL_CLK_SWH_L1 (1U << 13)
#define PM_CTRL_ASPM_L0S_EN (1U << 12)
#define PM_CTRL_SERDES_BUFS_RX_L1_EN (1U << 7)
#define PM_CTRL_SERDES_PD_EX_L1 (1U << 6)
#define PM_CTRL_SERDES_PLL_L1_EN (1U << 5)
#define PM_CTRL_SERDES_L1_EN (1U << 4)
#define PM_CTRL_ASPM_L1_EN (1U << 3)
#define REG_LTSSM_ID_CTRL 0x12FCU
#define LTSSM_ID_EN_WRO 0x1000U
#define REG_TWSI_CTRL 0x218U
#define TWSI_CTRL_SW_LDSTART 0x800U
#define REG_TWSI_DEBUG 0x1108U
#define TWSI_DEBUG_DEV_EXIST 0x20000000U
#define REG_CLK_GATING_CTRL 0x1814U
#define MASTER_CTRL_CLK_SEL_DIS (1U << 12)

#define REG_MASTER_CTRL 0x1400U
#define REG_IDLE_STATUS 0x1410U
#define REG_MDIO_CTRL 0x1414U
#define REG_MAC_CTRL 0x1480U
#define REG_MAC_STA_ADDR 0x1488U
#define REG_MTU 0x149CU
#define REG_RFD0_HEAD_ADDR_LO 0x1550U
#define REG_RFD_RING_SIZE 0x1560U
#define REG_RX_BUF_SIZE 0x1564U
#define REG_RRD0_HEAD_ADDR_LO 0x1568U
#define REG_RRD_RING_SIZE 0x1578U
#define REG_TPD_PRI0_ADDR_LO 0x1580U
#define REG_TPD_RING_SIZE 0x1584U
#define REG_TXQ_CTRL 0x1590U
#define REG_RXQ_CTRL 0x15A0U
#define REG_MB_RFD0_PROD_IDX 0x15E0U
#define REG_TPD_PRI0_PIDX 0x15F2U
#define REG_TPD_PRI0_CIDX 0x15F6U

#define MASTER_CTRL_SOFT_RST 0x1U
#define MDIO_CTRL_BUSY (1U << 27)
#define MDIO_CTRL_START (1U << 23)
#define MDIO_CTRL_OP_READ (1U << 21)
#define MDIO_CTRL_REG_SHIFT 16
#define MAC_CTRL_TX_EN 0x1U
#define MAC_CTRL_RX_EN 0x2U
#define MAC_CTRL_DUPLX (1U << 5)
#define MAC_CTRL_SPEED_SHIFT 20
#define TXQ_CTRL_EN (1U << 5)
#define RXQ_CTRL_EN (1U << 31)

#define MII_BMCR 0x00U
#define MII_BMSR 0x01U
#define MII_ADVERTISE 0x04U
#define MII_CTRL1000 0x09U
#define MII_PHYSID1 0x02U
#define MII_GIGA_PSSR 0x11U
#define BMSR_LINK_UP 0x0004U
#define GIGA_PSSR_RESOLVED 0x0800U
#define GIGA_PSSR_DUPLEX 0x2000U
#define GIGA_PSSR_1000MBS 0x8000U
#define GIGA_PSSR_100MBS 0x4000U

#define RX_SLOTS 64U
#define TX_SLOTS 16U
#define FRAME_BYTES 1536U
#define RRS_UPDATED (1U << 31)
#define RRS_ERR_SUM (1U << 20)
#define RRS_LEN_ERR (1U << 30)

struct model_tpd {
    uint16_t length;
    uint16_t vlan;
    uint32_t word1;
    uint64_t address;
};

struct model_rrd {
    uint32_t word0;
    uint32_t hash;
    uint16_t vlan;
    uint16_t flag;
    uint32_t word3;
};

static uint8_t *arena;
static uint64_t arena_used;
static volatile uint8_t *device_registers;
static const struct pci_driver *bound_driver;
static const struct net_adapter *bound_adapter;
static pthread_t model_thread;
static volatile int model_running;
static uint64_t clock_ns;

static int bus_master;
static int eeprom_present;
static unsigned eeprom_loads;
static uint16_t phy_registers[32];
static uint16_t tpd_consumer;
static volatile int transmit_paused;
static uint16_t model_rfd_next;
static uint16_t model_rrd_next;

static uint8_t sent_frames[64][FRAME_BYTES];
static unsigned sent_lengths[64];
static unsigned sent_count;

static uint8_t received_frames[64][FRAME_BYTES];
static unsigned received_lengths[64];
static unsigned received_count;

static unsigned failures;

static void check(const char *name, int ok) {
    printf("ATL1CTEST %s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static char driver_log[65536];
static size_t driver_log_used;

void kprintf(const char *fmt, ...) {
    char line[512];
    va_list arguments;
    va_start(arguments, fmt);
    int length = vsnprintf(line, sizeof(line), fmt, arguments);
    va_end(arguments);
    if (length < 0) return;
    printf("  [driver] %s", line);
    if (driver_log_used + (size_t)length + 1 < sizeof(driver_log)) {
        memcpy(driver_log + driver_log_used, line, (size_t)length);
        driver_log_used += (size_t)length;
        driver_log[driver_log_used] = '\0';
    }
}

static unsigned log_count(size_t from, const char *needle) {
    unsigned seen = 0;
    const char *at = driver_log + from;
    while ((at = strstr(at, needle)) != NULL) {
        seen++;
        at += strlen(needle);
    }
    return seen;
}

void panic(const char *message) {
    printf("ATL1CTEST panic: %s\n", message);
    exit(1);
}

uint64_t time_uptime_ns(void) {
    clock_ns += 1000ULL;
    return clock_ns;
}

uint64_t time_realtime_ns(void) { return clock_ns; }
uint64_t time_tsc_frequency(void) { return 1000000000ULL; }

void *dma_alloc_below(uint64_t bytes, uint64_t alignment, uint64_t limit,
                      uint64_t *physical) {
    (void)limit;
    if (alignment < 4096ULL) alignment = 4096ULL;
    arena_used = (arena_used + alignment - 1ULL) & ~(alignment - 1ULL);
    if (arena_used + bytes > ARENA_BYTES) return NULL;
    void *block = arena + arena_used;
    arena_used += bytes;
    memset(block, 0, bytes);
    *physical = (uint64_t)(uintptr_t)block;
    return block;
}

void *dma_alloc(uint64_t bytes, uint64_t alignment, uint64_t *physical) {
    return dma_alloc_below(bytes, alignment, 0, physical);
}

void dma_free(void *pointer, uint64_t bytes) {
    (void)pointer;
    (void)bytes;
}

uint64_t vmm_map_device(uint64_t physical, uint64_t bytes) {
    (void)bytes;
    return physical;
}

void pci_enable_bus_mastering(const struct pci_device *device) {
    (void)device;
    bus_master = 1;
}

uint8_t pci_find_capability(const struct pci_device *device, uint8_t id) {
    (void)device;
    return id == 0x10U ? 0x40U : 0;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    (void)bus;
    (void)slot;
    (void)function;
    return offset == 0x48U ? (2U << 12) : 0;
}

uint64_t pci_bar_address(const struct pci_device *device, unsigned index) {
    return index == 0 ? device->bar[0] & ~0xFULL : 0;
}

int pci_register_driver(struct pci_driver *driver) {
    bound_driver = driver;
    return 0;
}

void pci_unregister_driver(struct pci_driver *driver) {
    if (bound_driver == driver) bound_driver = NULL;
}

int net_register_adapter(const struct net_adapter *card) {
    bound_adapter = card;
    return 0;
}

void net_unregister_adapter(const struct net_adapter *card) {
    if (bound_adapter == card) bound_adapter = NULL;
}

static uint32_t register32(uint32_t offset) {
    return *(volatile uint32_t *)(device_registers + offset);
}

static void set_register32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(device_registers + offset) = value;
}

static uint16_t register16(uint32_t offset) {
    return *(volatile uint16_t *)(device_registers + offset);
}

static void set_register16(uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(device_registers + offset) = value;
}

static void serve_mdio(void) {
    uint32_t control = register32(REG_MDIO_CTRL);
    if (!(control & MDIO_CTRL_START)) return;
    unsigned reg = (control >> MDIO_CTRL_REG_SHIFT) & 0x1FU;
    if (control & MDIO_CTRL_OP_READ) {
        control = (control & ~0xFFFFU) | phy_registers[reg];
    } else {
        phy_registers[reg] = (uint16_t)control;
        if (reg == 0U) phy_registers[0] &= ~0x8000U;
    }
    control &= ~(MDIO_CTRL_START | MDIO_CTRL_BUSY);
    set_register32(REG_MDIO_CTRL, control);
}

static void serve_reset(void) {
    uint32_t master = register32(REG_MASTER_CTRL);
    if (master & MASTER_CTRL_SOFT_RST)
        set_register32(REG_MASTER_CTRL, master & ~MASTER_CTRL_SOFT_RST);
}

static int transmit_enabled(void) {
    return bus_master && (register32(REG_TXQ_CTRL) & TXQ_CTRL_EN) &&
           (register32(REG_MAC_CTRL) & MAC_CTRL_TX_EN);
}

static int receive_enabled(void) {
    return bus_master && (register32(REG_RXQ_CTRL) & RXQ_CTRL_EN) &&
           (register32(REG_MAC_CTRL) & MAC_CTRL_RX_EN);
}

static void serve_eeprom(void) {
    uint32_t control = register32(REG_TWSI_CTRL);
    if (!(control & TWSI_CTRL_SW_LDSTART)) return;
    if (eeprom_present) {
        set_register32(REG_MAC_STA_ADDR, 0x56789ABCU);
        set_register32(REG_MAC_STA_ADDR + 4U, 0x00001234U);
        eeprom_loads++;
    }
    set_register32(REG_TWSI_CTRL, control & ~TWSI_CTRL_SW_LDSTART);
}

static void serve_transmit(void) {
    if (transmit_paused || !transmit_enabled()) return;
    uint16_t producer = register16(REG_TPD_PRI0_PIDX);
    while (tpd_consumer != producer && sent_count < 64U) {
        uint64_t ring = register32(REG_TPD_PRI0_ADDR_LO);
        struct model_tpd *descriptor =
            (struct model_tpd *)(uintptr_t)(ring + (uint64_t)tpd_consumer * sizeof(*descriptor));
        unsigned length = descriptor->length;
        if (length > FRAME_BYTES) length = FRAME_BYTES;
        memcpy(sent_frames[sent_count], (void *)(uintptr_t)descriptor->address, length);
        sent_lengths[sent_count] = length;
        sent_count++;
        tpd_consumer = (uint16_t)((tpd_consumer + 1U) % TX_SLOTS);
        set_register16(REG_TPD_PRI0_CIDX, tpd_consumer);
    }
}

static void *model_main(void *unused) {
    (void)unused;
    while (model_running) {
        serve_mdio();
        serve_reset();
        serve_eeprom();
        serve_transmit();
        sched_yield();
    }
    return NULL;
}

static int model_receive_slots(const uint8_t *frame, unsigned length,
                               uint32_t extra_word3, uint32_t word0_override,
                               unsigned slots) {
    if (!receive_enabled()) return 0;
    uint16_t producer = (uint16_t)register32(REG_MB_RFD0_PROD_IDX);
    for (unsigned taken = 0; taken < slots; taken++)
        if ((uint16_t)((model_rfd_next + taken) % RX_SLOTS) == producer) return 0;

    uint64_t rfd_ring = register32(REG_RFD0_HEAD_ADDR_LO);
    uint64_t rrd_ring = register32(REG_RRD0_HEAD_ADDR_LO);
    uint64_t buffer = *(uint64_t *)(uintptr_t)(rfd_ring + (uint64_t)model_rfd_next * 8ULL);
    memcpy((void *)(uintptr_t)buffer, frame, length);

    struct model_rrd *status =
        (struct model_rrd *)(uintptr_t)(rrd_ring + (uint64_t)model_rrd_next * sizeof(*status));
    status->word0 = word0_override ? word0_override
                                   : (((uint32_t)model_rfd_next << 20) | (1U << 16));
    status->hash = 0;
    status->vlan = 0;
    status->flag = 0;
    status->word3 = RRS_UPDATED | (length + 4U) | extra_word3;

    model_rfd_next = (uint16_t)((model_rfd_next + slots) % RX_SLOTS);
    model_rrd_next = (uint16_t)((model_rrd_next + 1U) % RX_SLOTS);
    return 1;
}

static int model_receive_word3(const uint8_t *frame, unsigned length,
                               uint32_t extra_word3, uint32_t word0_override) {
    return model_receive_slots(frame, length, extra_word3, word0_override, 1U);
}

static void model_receive(const uint8_t *frame, unsigned length) {
    if (!model_receive_word3(frame, length, 0, 0)) {
        printf("ATL1CTEST model could not take a frame\n");
        failures++;
    }
}

static void deliver(const uint8_t *frame, size_t length) {
    if (received_count >= 64U) return;
    memcpy(received_frames[received_count], frame, length);
    received_lengths[received_count] = (unsigned)length;
    received_count++;
}

static void fill_frame(uint8_t *frame, unsigned length, uint8_t seed) {
    for (unsigned index = 0; index < length; index++)
        frame[index] = (uint8_t)(seed + index);
}

int main(void) {
    arena = mmap((void *)ARENA_BASE, ARENA_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (arena == MAP_FAILED) {
        printf("ATL1CTEST cannot place the dma arena\n");
        return 1;
    }
    memset(arena, 0, ARENA_BYTES);
    arena_used = REGISTER_BYTES;
    device_registers = arena;

    phy_registers[MII_PHYSID1] = 0x004DU;
    phy_registers[MII_BMSR] = BMSR_LINK_UP;
    phy_registers[MII_GIGA_PSSR] = GIGA_PSSR_RESOLVED | GIGA_PSSR_DUPLEX | GIGA_PSSR_1000MBS;
    set_register32(REG_MAC_STA_ADDR, 0x1A2B3C4DU);
    set_register32(REG_MAC_STA_ADDR + 4U, 0x00001069U);
    set_register32(REG_CLK_GATING_CTRL, 0x3FU);
    set_register32(REG_LTSSM_ID_CTRL, LTSSM_ID_EN_WRO);
    set_register32(REG_MASTER_CTRL, MASTER_CTRL_CLK_SEL_DIS);
    set_register32(REG_PM_CTRL, PM_CTRL_ASPM_L0S_EN | PM_CTRL_ASPM_L1_EN |
                   PM_CTRL_MAC_ASPM_CHK | PM_CTRL_SERDES_PD_EX_L1 |
                   (PM_CTRL_L1_ENTRY_TIMER_MASK << PM_CTRL_L1_ENTRY_TIMER_SHIFT));

    model_running = 1;
    pthread_create(&model_thread, NULL, model_main, NULL);

    check("module-init", __this_module.init() == 0);
    check("driver-registered", bound_driver != NULL);
    if (!bound_driver) return 1;

    struct pci_device device;
    memset(&device, 0, sizeof(device));
    device.vendor_id = 0x1969U;
    device.device_id = 0x1063U;
    device.bus = 3;
    device.slot = 0;
    device.function = 0;
    device.irq_line = 11;
    device.bar[0] = (uint32_t)(uintptr_t)arena;

    check("probe", bound_driver->probe(&device) == 0);
    check("adapter-registered", bound_adapter != NULL);
    if (!bound_adapter) return 1;

    static const uint8_t expected_mac[6] = { 0x10, 0x69, 0x1A, 0x2B, 0x3C, 0x4D };
    check("station-address", memcmp(bound_adapter->mac, expected_mac, 6) == 0);
    check("phy-advertised", phy_registers[MII_ADVERTISE] == 0x0DE1U &&
                            phy_registers[MII_CTRL1000] == 0x0300U);
    check("phy-autoneg", (phy_registers[MII_BMCR] & 0x1200U) == 0x1200U &&
                         !(phy_registers[MII_BMCR] & 0x0800U));
    check("mac-running",
          (register32(REG_MAC_CTRL) & (MAC_CTRL_TX_EN | MAC_CTRL_RX_EN)) ==
          (MAC_CTRL_TX_EN | MAC_CTRL_RX_EN));
    check("queues-running", (register32(REG_TXQ_CTRL) & TXQ_CTRL_EN) &&
                            (register32(REG_RXQ_CTRL) & RXQ_CTRL_EN));
    check("speed-1000", ((register32(REG_MAC_CTRL) >> MAC_CTRL_SPEED_SHIFT) & 3U) == 2U &&
                        (register32(REG_MAC_CTRL) & MAC_CTRL_DUPLX));
    check("ring-sizes", register32(REG_RFD_RING_SIZE) == RX_SLOTS &&
                        register32(REG_RRD_RING_SIZE) == RX_SLOTS &&
                        register32(REG_TPD_RING_SIZE) == TX_SLOTS);
    check("buffer-size", register32(REG_RX_BUF_SIZE) >= 1522U &&
                         register32(REG_MTU) >= 1522U);
    check("receive-slots-published", register32(REG_MB_RFD0_PROD_IDX) == RX_SLOTS - 1U);
    check("bus-mastering", bus_master);
    check("pcie-patched", !(register32(REG_LTSSM_ID_CTRL) & LTSSM_ID_EN_WRO) &&
                          (register32(REG_PCIE_PHYMISC) & PCIE_PHYMISC_FORCE_RCV_DET) &&
                          !(register32(REG_MASTER_CTRL) & MASTER_CTRL_CLK_SEL_DIS));
    check("clock-gating-off", register32(REG_CLK_GATING_CTRL) == 0);
    uint32_t power = register32(REG_PM_CTRL);
    check("aspm-off-with-link",
          !(power & (PM_CTRL_ASPM_L0S_EN | PM_CTRL_ASPM_L1_EN | PM_CTRL_CLK_SWH_L1 |
                     PM_CTRL_SERDES_PD_EX_L1)) &&
          (power & (PM_CTRL_SERDES_L1_EN | PM_CTRL_SERDES_PLL_L1_EN |
                    PM_CTRL_SERDES_BUFS_RX_L1_EN)) ==
              (PM_CTRL_SERDES_L1_EN | PM_CTRL_SERDES_PLL_L1_EN |
               PM_CTRL_SERDES_BUFS_RX_L1_EN) &&
          ((power >> PM_CTRL_L1_ENTRY_TIMER_SHIFT) & PM_CTRL_L1_ENTRY_TIMER_MASK) == 0U);

    uint8_t frame[1514];
    for (unsigned round = 0; round < 8U; round++) {
        unsigned length = 60U + round * 180U;
        fill_frame(frame, length, (uint8_t)(0x40U + round));
        check("transmit", bound_adapter->transmit(frame, length) == 0);
    }
    for (unsigned attempt = 0; attempt < 2000U && sent_count < 8U; attempt++) usleep(100);
    check("transmit-arrived", sent_count == 8U);
    int identical = sent_count == 8U;
    for (unsigned round = 0; round < sent_count && identical; round++) {
        unsigned length = 60U + round * 180U;
        fill_frame(frame, length, (uint8_t)(0x40U + round));
        identical = sent_lengths[round] == length &&
                    memcmp(sent_frames[round], frame, length) == 0;
    }
    check("transmit-bytes", identical);

    for (unsigned round = 0; round < 6U; round++) {
        unsigned length = 64U + round * 200U;
        fill_frame(frame, length, (uint8_t)(0x90U + round));
        model_receive(frame, length);
    }
    bound_adapter->poll(deliver);
    check("receive-count", received_count == 6U);
    identical = received_count == 6U;
    for (unsigned round = 0; round < received_count && identical; round++) {
        unsigned length = 64U + round * 200U;
        fill_frame(frame, length, (uint8_t)(0x90U + round));
        identical = received_lengths[round] == length &&
                    memcmp(received_frames[round], frame, length) == 0;
    }
    check("receive-bytes", identical);
    check("receive-slots-returned",
          register32(REG_MB_RFD0_PROD_IDX) == (RX_SLOTS - 1U + 6U) % RX_SLOTS);

    received_count = 0;
    bound_adapter->poll(deliver);
    check("receive-idle", received_count == 0);

    unsigned wrapped = 0;
    int wrap_bytes_match = 1;
    for (unsigned batch = 0; batch < 10U; batch++) {
        received_count = 0;
        for (unsigned index = 0; index < 24U; index++) {
            unsigned length = 60U + (wrapped + index) % 400U;
            fill_frame(frame, length, (uint8_t)(wrapped + index));
            model_receive(frame, length);
        }
        bound_adapter->poll(deliver);
        if (received_count != 24U) wrap_bytes_match = 0;
        for (unsigned index = 0; index < received_count; index++) {
            unsigned length = 60U + (wrapped + index) % 400U;
            fill_frame(frame, length, (uint8_t)(wrapped + index));
            if (received_lengths[index] != length ||
                memcmp(received_frames[index], frame, length) != 0)
                wrap_bytes_match = 0;
        }
        wrapped += 24U;
    }
    check("receive-wraps-the-ring", wrapped == 240U && wrap_bytes_match);
    check("receive-slots-still-published",
          register32(REG_MB_RFD0_PROD_IDX) == (RX_SLOTS - 1U + 6U + 240U) % RX_SLOTS);

    sent_count = 0;
    int tx_wrap_ok = 1;
    for (unsigned index = 0; index < 60U; index++) {
        fill_frame(frame, 128, (uint8_t)(0x20U + index));
        int queued_frame = 0;
        for (unsigned attempt = 0; attempt < 5000U && !queued_frame; attempt++) {
            if (bound_adapter->transmit(frame, 128) == 0) queued_frame = 1;
            else usleep(100);
        }
        if (!queued_frame) tx_wrap_ok = 0;
    }
    for (unsigned attempt = 0; attempt < 5000U && sent_count < 60U; attempt++) usleep(100);
    for (unsigned index = 0; index < sent_count && tx_wrap_ok; index++) {
        fill_frame(frame, 128, (uint8_t)(0x20U + index));
        if (sent_lengths[index] != 128U ||
            memcmp(sent_frames[index], frame, 128) != 0)
            tx_wrap_ok = 0;
    }
    check("transmit-wraps-the-ring", tx_wrap_ok && sent_count == 60U);
    sent_count = 0;

    uint64_t dropped_before = bound_adapter->rx_dropped();
    received_count = 0;
    fill_frame(frame, 200, 0x11);
    model_receive_word3(frame, 200, RRS_ERR_SUM, 0);
    model_receive_word3(frame, 200, RRS_LEN_ERR, 0);
    model_receive_word3(frame, 200, 0x3F00U, 0);
    model_receive_word3(frame, 200, 0, (RX_SLOTS + 8U) << 20 | (1U << 16));
    model_receive_slots(frame, 200, 0,
                        ((uint32_t)model_rfd_next << 20) | (2U << 16), 2U);
    bound_adapter->poll(deliver);
    check("bad-descriptors-dropped", received_count == 0 &&
          bound_adapter->rx_dropped() == dropped_before + 5U);

    received_count = 0;
    fill_frame(frame, 300, 0x77);
    model_receive(frame, 300);
    bound_adapter->poll(deliver);
    check("good-frame-after-bad",
          received_count == 1U && received_lengths[0] == 300U &&
          memcmp(received_frames[0], frame, 300) == 0);

    uint32_t receive_queue = register32(REG_RXQ_CTRL);
    set_register32(REG_RXQ_CTRL, receive_queue & ~RXQ_CTRL_EN);
    received_count = 0;
    fill_frame(frame, 100, 0x55);
    int refused = model_receive_word3(frame, 100, 0, 0) == 0;
    bound_adapter->poll(deliver);
    check("no-dma-with-the-queue-off", refused && received_count == 0);
    set_register32(REG_RXQ_CTRL, receive_queue);

    unsigned already_sent = sent_count;
    transmit_paused = 1;
    usleep(2000);
    unsigned queued = 0;
    for (unsigned round = 0; round < TX_SLOTS + 4U; round++) {
        fill_frame(frame, 100, (uint8_t)round);
        if (bound_adapter->transmit(frame, 100) == 0) queued++;
    }
    check("transmit-backpressure", queued == TX_SLOTS - 1U);
    transmit_paused = 0;
    for (unsigned attempt = 0; attempt < 2000U && sent_count < already_sent + queued;
         attempt++)
        usleep(100);
    check("transmit-drains", sent_count == already_sent + queued);

    size_t mark = driver_log_used;
    phy_registers[MII_BMSR] = 0;
    for (unsigned round = 0; round < 4U; round++) {
        clock_ns += 600000000ULL;
        bound_adapter->poll(deliver);
    }
    check("link-down-said-once", log_count(mark, "link down") == 1U);
    power = register32(REG_PM_CTRL);
    check("aspm-parked-without-link",
          (power & PM_CTRL_CLK_SWH_L1) &&
          !(power & (PM_CTRL_SERDES_L1_EN | PM_CTRL_SERDES_PLL_L1_EN |
                     PM_CTRL_SERDES_BUFS_RX_L1_EN | PM_CTRL_ASPM_L0S_EN)));

    mark = driver_log_used;
    phy_registers[MII_BMSR] = BMSR_LINK_UP;
    phy_registers[MII_GIGA_PSSR] = GIGA_PSSR_RESOLVED | GIGA_PSSR_100MBS;
    for (unsigned round = 0; round < 3U; round++) {
        clock_ns += 600000000ULL;
        bound_adapter->poll(deliver);
    }
    check("link-up-said-once", log_count(mark, "link up") == 1U);
    check("link-renegotiated",
          ((register32(REG_MAC_CTRL) >> MAC_CTRL_SPEED_SHIFT) & 3U) == 1U &&
          !(register32(REG_MAC_CTRL) & MAC_CTRL_DUPLX));

    received_count = 0;
    fill_frame(frame, 250, 0x33);
    model_receive(frame, 250);
    bound_adapter->poll(deliver);
    check("traffic-survives-a-link-flap",
          received_count == 1U && received_lengths[0] == 250U &&
          memcmp(received_frames[0], frame, 250) == 0);

    mark = driver_log_used;
    phy_registers[MII_BMSR] = BMSR_LINK_UP;
    phy_registers[MII_GIGA_PSSR] = 0;
    clock_ns += 600000000ULL;
    bound_adapter->poll(deliver);
    check("link-negotiating", log_count(mark, "negotiating") == 1U);

    const struct net_adapter *gone = bound_adapter;
    bound_driver->remove(&device);
    check("adapter-gone", bound_adapter == NULL);
    check("mac-stopped",
          !(register32(REG_MAC_CTRL) & (MAC_CTRL_TX_EN | MAC_CTRL_RX_EN)));
    fill_frame(frame, 100, 0x66);
    check("transmit-refused-after-remove", gone->transmit(frame, 100) != 0);
    __this_module.exit();
    check("driver-gone", bound_driver == NULL);

    eeprom_present = 1;
    set_register32(REG_MAC_STA_ADDR, 0);
    set_register32(REG_MAC_STA_ADDR + 4U, 0);
    set_register32(REG_TWSI_DEBUG, TWSI_DEBUG_DEV_EXIST);
    phy_registers[MII_BMSR] = BMSR_LINK_UP;
    phy_registers[MII_GIGA_PSSR] =
        GIGA_PSSR_RESOLVED | GIGA_PSSR_DUPLEX | GIGA_PSSR_1000MBS;
    check("module-init-again", __this_module.init() == 0);
    check("probe-with-eeprom-address", bound_driver->probe(&device) == 0);
    static const uint8_t eeprom_mac[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };
    check("station-address-from-eeprom",
          eeprom_loads > 0 && bound_adapter != NULL &&
          memcmp(bound_adapter->mac, eeprom_mac, 6) == 0);
    bound_driver->remove(&device);
    __this_module.exit();

    model_running = 0;
    pthread_join(model_thread, NULL);
    printf("ATL1CTEST %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
