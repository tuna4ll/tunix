#include <stddef.h>
#include <stdint.h>

#include "../include/cpu.h"
#include "../include/dma.h"
#include "../include/kstring.h"
#include "../include/module.h"
#include "../include/pci.h"
#include "../include/time.h"
#include "../include/vmm.h"
#include "../include/net/net.h"

extern void kprintf(const char *fmt, ...);

#define ATL1C_VENDOR 0x1969U
#define ATL1C_AR8131 0x1063U
#define ATL1C_AR8132 0x1062U

#define ATL1C_REGISTER_BYTES 0x2000ULL

#define REG_TWSI_CTRL 0x218U
#define TWSI_CTRL_SW_LDSTART 0x800U
#define REG_TWSI_DEBUG 0x1108U
#define TWSI_DEBUG_DEV_EXIST 0x20000000U
#define REG_OTP_CTRL 0x12F0U
#define OTP_CTRL_CLK_EN 0x0002U

#define REG_MASTER_CTRL 0x1400U
#define MASTER_CTRL_SOFT_RST (1U << 0)
#define MASTER_CTRL_OOB_DIS (1U << 6)
#define MASTER_CTRL_SA_TIMER_EN (1U << 7)
#define MASTER_CTRL_TX_ITIMER_EN (1U << 10)
#define MASTER_CTRL_RX_ITIMER_EN (1U << 11)
#define MASTER_CTRL_CLK_SEL_DIS (1U << 12)
#define MASTER_CTRL_INT_RDCLR (1U << 14)
#define MASTER_CTRL_OTP_SEL (1U << 31)

#define REG_GPHY_CTRL 0x140CU
#define GPHY_CTRL_EXT_RESET (1U << 0)
#define GPHY_CTRL_GATE_25M_EN (1U << 5)
#define GPHY_CTRL_PHY_IDDQ (1U << 7)
#define GPHY_CTRL_HIB_EN (1U << 10)
#define GPHY_CTRL_HIB_PULSE (1U << 11)
#define GPHY_CTRL_SEL_ANA_RST (1U << 12)
#define GPHY_CTRL_PWDOWN_HW (1U << 14)

#define REG_IDLE_STATUS 0x1410U
#define IDLE_STATUS_MASK 0x0FU

#define REG_MDIO_CTRL 0x1414U
#define MDIO_CTRL_BUSY (1U << 27)
#define MDIO_CTRL_START (1U << 23)
#define MDIO_CTRL_SPRES_PRMBL (1U << 22)
#define MDIO_CTRL_OP_READ (1U << 21)
#define MDIO_CTRL_REG_SHIFT 16
#define MDIO_CTRL_DATA_MASK 0xFFFFU

#define REG_MAC_CTRL 0x1480U
#define MAC_CTRL_TX_EN (1U << 0)
#define MAC_CTRL_RX_EN (1U << 1)
#define MAC_CTRL_TX_FLOW (1U << 2)
#define MAC_CTRL_RX_FLOW (1U << 3)
#define MAC_CTRL_DUPLX (1U << 5)
#define MAC_CTRL_ADD_CRC (1U << 6)
#define MAC_CTRL_PAD (1U << 7)
#define MAC_CTRL_PRMLEN_SHIFT 10
#define MAC_CTRL_PROMIS_EN (1U << 15)
#define MAC_CTRL_SPEED_SHIFT 20
#define MAC_CTRL_BC_EN (1U << 26)
#define MAC_CTRL_HASH_ALG_CRC32 (1U << 29)
#define MAC_CTRL_SPEED_MODE_SW (1U << 30)
#define MAC_CTRL_SINGLE_PAUSE_EN (1U << 28)
#define MAC_SPEED_10_100 1U
#define MAC_SPEED_1000 2U

#define REG_MAC_STA_ADDR 0x1488U
#define REG_RX_HASH_TABLE 0x1490U
#define REG_MTU 0x149CU
#define REG_WOL_CTRL 0x14A0U

#define REG_LOAD_PTR 0x1534U
#define REG_RX_BASE_ADDR_HI 0x1540U
#define REG_TX_BASE_ADDR_HI 0x1544U
#define REG_RFD0_HEAD_ADDR_LO 0x1550U
#define REG_RFD_RING_SIZE 0x1560U
#define REG_RX_BUF_SIZE 0x1564U
#define REG_RRD0_HEAD_ADDR_LO 0x1568U
#define REG_RRD_RING_SIZE 0x1578U
#define REG_TPD_PRI0_ADDR_LO 0x1580U
#define REG_TPD_RING_SIZE 0x1584U

#define REG_TXQ_CTRL 0x1590U
#define TXQ_NUM_TPD_BURST_DEF 5U
#define TXQ_CTRL_IP_OPTION_EN (1U << 4)
#define TXQ_CTRL_EN (1U << 5)
#define TXQ_CTRL_ENH_MODE (1U << 6)
#define TXQ_CTRL_LS_8023_EN (1U << 7)
#define TXQ_TXF_BURST_SHIFT 16
#define TXQ_TXF_BURST_L1C 0x200U

#define REG_RXQ_CTRL 0x15A0U
#define RXQ_RFD_BURST_SHIFT 20
#define RXQ_RFD_BURST_DEF 8U
#define RXQ_CTRL_EN (1U << 31)

#define REG_DMA_CTRL 0x15C0U
#define DMA_CTRL_RORDER_MODE_OUT 4U
#define DMA_CTRL_RREQ_BLEN_SHIFT 4
#define DMA_CTRL_RREQ_PRI_DATA (1U << 10)
#define DMA_CTRL_RDLY_CNT_SHIFT 11
#define DMA_CTRL_RDLY_CNT_DEF 15U
#define DMA_CTRL_WDLY_CNT_SHIFT 16
#define DMA_CTRL_WDLY_CNT_DEF 4U

#define REG_MB_RFD0_PROD_IDX 0x15E0U
#define REG_TPD_PRI0_PIDX 0x15F2U
#define REG_TPD_PRI0_CIDX 0x15F6U

#define REG_ISR 0x1600U
#define REG_IMR 0x1604U

#define MII_BMCR 0x00U
#define MII_BMSR 0x01U
#define MII_PHYSID1 0x02U
#define MII_PHYSID2 0x03U
#define MII_ADVERTISE 0x04U
#define MII_CTRL1000 0x09U
#define ADVERTISE_ALL 0x01E1U
#define ADVERTISE_PAUSE 0x0C00U
#define CTRL1000_FULL 0x0200U
#define CTRL1000_HALF 0x0100U
#define BMCR_RESTART_ANEG 0x0200U
#define BMCR_POWER_DOWN 0x0800U
#define BMCR_ANEG_EN 0x1000U
#define BMCR_RESET 0x8000U
#define BMSR_LINK_UP 0x0004U
#define MII_GIGA_PSSR 0x11U
#define GIGA_PSSR_RESOLVED 0x0800U
#define GIGA_PSSR_DUPLEX 0x2000U
#define GIGA_PSSR_SPEED 0xC000U
#define GIGA_PSSR_100MBS 0x4000U
#define GIGA_PSSR_1000MBS 0x8000U

#define RRS_RFD_INDEX_SHIFT 20
#define RRS_RFD_INDEX_MASK 0x0FFFU
#define RRS_RFD_COUNT_SHIFT 16
#define RRS_RFD_COUNT_MASK 0x000FU
#define RRS_PKT_SIZE_MASK 0x3FFFU
#define RRS_ERR_SUM (1U << 20)
#define RRS_LEN_ERR (1U << 30)
#define RRS_UPDATED (1U << 31)

#define TPD_EOP (1U << 31)

#define RX_SLOTS 64U
#define TX_SLOTS 16U
#define FRAME_BYTES 1536U
#define FRAME_MIN 14U
#define FRAME_MAX 1514U
#define RX_BUFFER_BYTES 1536U

struct atl1c_tpd {
    uint16_t length;
    uint16_t vlan;
    uint32_t word1;
    uint64_t address;
};

struct atl1c_rrd {
    uint32_t word0;
    uint32_t hash;
    uint16_t vlan;
    uint16_t flag;
    uint32_t word3;
};

typedef void (*atl1c_receive_fn)(const uint8_t *frame, size_t length);

static uint64_t registers;
static struct pci_device card;
static uint8_t mac_address[6];
static int available;
static int link_speed;
static int link_duplex;
static int link_up;
static int link_state;
static uint64_t link_deadline;
static int debug;
static int hibernate;

MODULE_PARAMETER(debug, MODULE_PARAM_INT);
MODULE_PARAMETER(hibernate, MODULE_PARAM_INT);

static uint64_t *rfd_ring;
static uint64_t rfd_physical;
static struct atl1c_rrd *rrd_ring;
static uint64_t rrd_physical;
static struct atl1c_tpd *tpd_ring;
static uint64_t tpd_physical;
static uint8_t *rx_buffers;
static uint64_t rx_buffers_physical;
static uint8_t *tx_buffers;
static uint64_t tx_buffers_physical;

static uint16_t rfd_next;
static uint16_t rrd_next;
static uint16_t tpd_next;
static uint64_t rx_count;
static uint64_t tx_count;
static uint64_t drop_count;

static const struct net_adapter atl1c_adapter;

static uint32_t read32(uint32_t offset) {
    return *(volatile uint32_t *)(registers + offset);
}

static void write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(registers + offset) = value;
}

static uint16_t read16(uint32_t offset) {
    return *(volatile uint16_t *)(registers + offset);
}

static void write16(uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(registers + offset) = value;
}

static void flush_writes(void) {
    (void)read32(REG_MASTER_CTRL);
}

static void delay_ns(uint64_t nanoseconds) {
    uint64_t deadline = time_uptime_ns() + nanoseconds;
    while (time_uptime_ns() < deadline) cpu_relax();
}

static int wait_until_idle(void) {
    for (unsigned attempt = 0; attempt < 100U; attempt++) {
        if (!(read32(REG_IDLE_STATUS) & IDLE_STATUS_MASK)) return 0;
        delay_ns(1000000ULL);
    }
    return -1;
}

static int mdio_wait(void) {
    for (unsigned attempt = 0; attempt < 200U; attempt++) {
        uint32_t control = read32(REG_MDIO_CTRL);
        if (!(control & (MDIO_CTRL_BUSY | MDIO_CTRL_START))) return 0;
        delay_ns(10000ULL);
    }
    return -1;
}

static int phy_read(unsigned reg, uint16_t *value) {
    write32(REG_MDIO_CTRL, MDIO_CTRL_SPRES_PRMBL | MDIO_CTRL_START |
            MDIO_CTRL_OP_READ | ((reg & 0x1FU) << MDIO_CTRL_REG_SHIFT));
    flush_writes();
    if (mdio_wait() != 0) return -1;
    *value = (uint16_t)(read32(REG_MDIO_CTRL) & MDIO_CTRL_DATA_MASK);
    return 0;
}

static int phy_write(unsigned reg, uint16_t value) {
    write32(REG_MDIO_CTRL, MDIO_CTRL_SPRES_PRMBL | MDIO_CTRL_START |
            ((reg & 0x1FU) << MDIO_CTRL_REG_SHIFT) | value);
    flush_writes();
    return mdio_wait();
}

static int mac_address_valid(const uint8_t *address) {
    int zero = 1;
    for (unsigned index = 0; index < 6U; index++)
        if (address[index]) zero = 0;
    return !zero && !(address[0] & 1U);
}

static void read_station_address(uint8_t *out) {
    uint32_t low = read32(REG_MAC_STA_ADDR);
    uint32_t high = read32(REG_MAC_STA_ADDR + 4U);
    out[0] = (uint8_t)(high >> 8);
    out[1] = (uint8_t)high;
    out[2] = (uint8_t)(low >> 24);
    out[3] = (uint8_t)(low >> 16);
    out[4] = (uint8_t)(low >> 8);
    out[5] = (uint8_t)low;
}

static void write_station_address(const uint8_t *address) {
    write32(REG_MAC_STA_ADDR, ((uint32_t)address[2] << 24) |
            ((uint32_t)address[3] << 16) | ((uint32_t)address[4] << 8) | address[5]);
    write32(REG_MAC_STA_ADDR + 4U,
            ((uint32_t)address[0] << 8) | address[1]);
}

static int load_mac_address(void) {
    read_station_address(mac_address);
    if (mac_address_valid(mac_address)) return 0;

    uint32_t otp = read32(REG_OTP_CTRL);
    int eeprom = (read32(REG_TWSI_DEBUG) & TWSI_DEBUG_DEV_EXIST) ||
                 (read32(REG_MASTER_CTRL) & MASTER_CTRL_OTP_SEL);
    if (!eeprom) return -1;

    if (!(otp & OTP_CTRL_CLK_EN)) {
        write32(REG_OTP_CTRL, otp | OTP_CTRL_CLK_EN);
        flush_writes();
        delay_ns(1000000ULL);
    }
    write32(REG_TWSI_CTRL, read32(REG_TWSI_CTRL) | TWSI_CTRL_SW_LDSTART);
    for (unsigned attempt = 0; attempt < 100U; attempt++) {
        delay_ns(10000000ULL);
        if (!(read32(REG_TWSI_CTRL) & TWSI_CTRL_SW_LDSTART)) break;
    }
    if (!(otp & OTP_CTRL_CLK_EN)) {
        write32(REG_OTP_CTRL, otp);
        delay_ns(1000000ULL);
    }

    read_station_address(mac_address);
    return mac_address_valid(mac_address) ? 0 : -1;
}

static void stop_mac(void) {
    write32(REG_RXQ_CTRL, read32(REG_RXQ_CTRL) & ~RXQ_CTRL_EN);
    write32(REG_TXQ_CTRL, read32(REG_TXQ_CTRL) & ~TXQ_CTRL_EN);
    delay_ns(1000000ULL);
    write32(REG_MAC_CTRL, read32(REG_MAC_CTRL) & ~(MAC_CTRL_TX_EN | MAC_CTRL_RX_EN));
    (void)wait_until_idle();
}

static int reset_mac(void) {
    write32(REG_IMR, 0);
    write32(REG_ISR, 0xFFFFFFFFU);
    stop_mac();

    uint32_t master = read32(REG_MASTER_CTRL) | MASTER_CTRL_OOB_DIS;
    write32(REG_MASTER_CTRL, master | MASTER_CTRL_SOFT_RST);
    flush_writes();
    delay_ns(10000000ULL);
    if (wait_until_idle() != 0) return -1;
    write32(REG_MASTER_CTRL, master);
    write32(REG_MAC_CTRL, read32(REG_MAC_CTRL) | MAC_CTRL_SPEED_MODE_SW);
    return 0;
}

static void reset_phy(void) {
    uint32_t control = read32(REG_GPHY_CTRL);
    control &= ~(GPHY_CTRL_EXT_RESET | GPHY_CTRL_PHY_IDDQ | GPHY_CTRL_GATE_25M_EN |
                 GPHY_CTRL_PWDOWN_HW);
    control |= GPHY_CTRL_SEL_ANA_RST;
    if (hibernate) control |= GPHY_CTRL_HIB_EN | GPHY_CTRL_HIB_PULSE;
    else control &= ~(GPHY_CTRL_HIB_EN | GPHY_CTRL_HIB_PULSE);
    write32(REG_GPHY_CTRL, control);
    flush_writes();
    delay_ns(10000ULL);
    write32(REG_GPHY_CTRL, control | GPHY_CTRL_EXT_RESET);
    flush_writes();
    delay_ns(1000000ULL);
}

#define LINK_UP 0
#define LINK_NO_PHY (-1)
#define LINK_NO_CABLE (-2)
#define LINK_NEGOTIATING (-3)

static int read_link(int *speed, int *duplex) {
    uint16_t status = 0;
    (void)phy_read(MII_BMSR, &status);
    if (phy_read(MII_BMSR, &status) != 0) return LINK_NO_PHY;
    if (!(status & BMSR_LINK_UP)) return LINK_NO_CABLE;

    uint16_t detail = 0;
    if (phy_read(MII_GIGA_PSSR, &detail) != 0) return LINK_NO_PHY;
    if (!(detail & GIGA_PSSR_RESOLVED)) return LINK_NEGOTIATING;
    *duplex = (detail & GIGA_PSSR_DUPLEX) ? 1 : 0;
    switch (detail & GIGA_PSSR_SPEED) {
    case GIGA_PSSR_1000MBS: *speed = 1000; break;
    case GIGA_PSSR_100MBS: *speed = 100; break;
    default: *speed = 10; break;
    }
    return LINK_UP;
}

static const char *link_reason(int state) {
    switch (state) {
    case LINK_NO_PHY: return "the phy stopped answering";
    case LINK_NO_CABLE: return "no cable";
    case LINK_NEGOTIATING: return "negotiating";
    default: return "down";
    }
}

static void start_mac(void);

static void update_link(void) {
    int speed = 0;
    int duplex = 0;
    int state = read_link(&speed, &duplex);
    int up = state == LINK_UP;
    if (debug) {
        uint16_t status = 0;
        uint16_t detail = 0;
        (void)phy_read(MII_BMSR, &status);
        (void)phy_read(MII_GIGA_PSSR, &detail);
        kprintf("ATL1C: poll bmsr %x pssr %x state %d rx %u tx %u dropped %u\n",
                status, detail, state, (unsigned)rx_count, (unsigned)tx_count,
                (unsigned)drop_count);
    }
    if (up == link_up && (!up || (speed == link_speed && duplex == link_duplex))) {
        link_state = state;
        return;
    }

    link_up = up;
    if (up) {
        link_speed = speed;
        link_duplex = duplex;
        start_mac();
        kprintf("ATL1C: link up, %u Mbit %s duplex\n", (unsigned)link_speed,
                link_duplex ? "full" : "half");
    } else if (state != link_state) {
        kprintf("ATL1C: link down, %s\n", link_reason(state));
    }
    link_state = state;
}

static unsigned read_request_block(void) {
    uint8_t capability = pci_find_capability(&card, 0x10U);
    if (!capability) return 3U;
    uint32_t control = pci_config_read32(card.bus, card.slot, card.function,
                                         (uint8_t)(capability + 8U));
    unsigned maximum = (control >> 12) & 7U;
    return maximum < 3U ? maximum : 3U;
}

static void configure_rings(void) {
    write32(REG_TX_BASE_ADDR_HI, (uint32_t)(tpd_physical >> 32));
    write32(REG_TPD_PRI0_ADDR_LO, (uint32_t)tpd_physical);
    write32(REG_TPD_RING_SIZE, TX_SLOTS);

    write32(REG_RX_BASE_ADDR_HI, (uint32_t)(rfd_physical >> 32));
    write32(REG_RFD0_HEAD_ADDR_LO, (uint32_t)rfd_physical);
    write32(REG_RFD_RING_SIZE, RX_SLOTS);
    write32(REG_RX_BUF_SIZE, RX_BUFFER_BYTES);
    write32(REG_RRD0_HEAD_ADDR_LO, (uint32_t)rrd_physical);
    write32(REG_RRD_RING_SIZE, RX_SLOTS);
    write32(REG_LOAD_PTR, 1);
}

static void configure_mac(void) {
    uint32_t master = read32(REG_MASTER_CTRL);
    master &= ~(MASTER_CTRL_TX_ITIMER_EN | MASTER_CTRL_RX_ITIMER_EN |
                MASTER_CTRL_INT_RDCLR | MASTER_CTRL_SA_TIMER_EN |
                MASTER_CTRL_CLK_SEL_DIS);
    write32(REG_ISR, 0xFFFFFFFFU);
    write32(REG_WOL_CTRL, 0);
    write32(REG_MASTER_CTRL, master);

    configure_rings();

    write32(REG_MTU, FRAME_MAX + 8U);
    write32(REG_TXQ_CTRL, TXQ_NUM_TPD_BURST_DEF | TXQ_CTRL_ENH_MODE |
            TXQ_CTRL_LS_8023_EN | TXQ_CTRL_IP_OPTION_EN |
            (TXQ_TXF_BURST_L1C << TXQ_TXF_BURST_SHIFT));
    write32(REG_RXQ_CTRL, RXQ_RFD_BURST_DEF << RXQ_RFD_BURST_SHIFT);
    write32(REG_DMA_CTRL, DMA_CTRL_RORDER_MODE_OUT | DMA_CTRL_RREQ_PRI_DATA |
            (read_request_block() << DMA_CTRL_RREQ_BLEN_SHIFT) |
            (DMA_CTRL_RDLY_CNT_DEF << DMA_CTRL_RDLY_CNT_SHIFT) |
            (DMA_CTRL_WDLY_CNT_DEF << DMA_CTRL_WDLY_CNT_SHIFT));
    write32(REG_RX_HASH_TABLE, 0xFFFFFFFFU);
    write32(REG_RX_HASH_TABLE + 4U, 0xFFFFFFFFU);
    write32(REG_IMR, 0);
}

static void start_mac(void) {
    uint32_t mac = read32(REG_MAC_CTRL);
    mac |= MAC_CTRL_TX_EN | MAC_CTRL_RX_EN | MAC_CTRL_TX_FLOW | MAC_CTRL_RX_FLOW |
           MAC_CTRL_ADD_CRC | MAC_CTRL_PAD | MAC_CTRL_BC_EN |
           MAC_CTRL_SINGLE_PAUSE_EN | MAC_CTRL_HASH_ALG_CRC32 |
           MAC_CTRL_SPEED_MODE_SW;
    mac &= ~(3U << MAC_CTRL_SPEED_SHIFT);
    mac |= (link_speed == 1000 ? MAC_SPEED_1000 : MAC_SPEED_10_100)
           << MAC_CTRL_SPEED_SHIFT;
    if (link_duplex) mac |= MAC_CTRL_DUPLX;
    else mac &= ~MAC_CTRL_DUPLX;
    mac &= ~(0xFU << MAC_CTRL_PRMLEN_SHIFT);
    mac |= 7U << MAC_CTRL_PRMLEN_SHIFT;

    write32(REG_TXQ_CTRL, read32(REG_TXQ_CTRL) | TXQ_CTRL_EN);
    write32(REG_RXQ_CTRL, read32(REG_RXQ_CTRL) | RXQ_CTRL_EN);
    write32(REG_MAC_CTRL, mac);
    flush_writes();
}

static int allocate_rings(void) {
    rfd_ring = dma_alloc_below(RX_SLOTS * sizeof(uint64_t), 4096, DMA_LIMIT_32BIT,
                               &rfd_physical);
    rrd_ring = dma_alloc_below(RX_SLOTS * sizeof(struct atl1c_rrd), 4096,
                               DMA_LIMIT_32BIT, &rrd_physical);
    tpd_ring = dma_alloc_below(TX_SLOTS * sizeof(struct atl1c_tpd), 4096,
                               DMA_LIMIT_32BIT, &tpd_physical);
    rx_buffers = dma_alloc_below(RX_SLOTS * FRAME_BYTES, 4096, DMA_LIMIT_32BIT,
                                 &rx_buffers_physical);
    tx_buffers = dma_alloc_below(TX_SLOTS * FRAME_BYTES, 4096, DMA_LIMIT_32BIT,
                                 &tx_buffers_physical);
    if (!rfd_ring || !rrd_ring || !tpd_ring || !rx_buffers || !tx_buffers) return -1;

    for (unsigned index = 0; index < RX_SLOTS; index++)
        rfd_ring[index] = rx_buffers_physical + (uint64_t)index * FRAME_BYTES;
    memset(rrd_ring, 0, RX_SLOTS * sizeof(struct atl1c_rrd));
    memset(tpd_ring, 0, TX_SLOTS * sizeof(struct atl1c_tpd));
    return 0;
}

static void release_rings(void) {
    dma_free(rfd_ring, RX_SLOTS * sizeof(uint64_t));
    dma_free(rrd_ring, RX_SLOTS * sizeof(struct atl1c_rrd));
    dma_free(tpd_ring, TX_SLOTS * sizeof(struct atl1c_tpd));
    dma_free(rx_buffers, RX_SLOTS * FRAME_BYTES);
    dma_free(tx_buffers, TX_SLOTS * FRAME_BYTES);
    rfd_ring = NULL;
    rrd_ring = NULL;
    tpd_ring = NULL;
    rx_buffers = NULL;
    tx_buffers = NULL;
}

static int atl1c_transmit(const void *frame, size_t length) {
    if (!available || !frame || length < FRAME_MIN || length > FRAME_MAX) return -1;

    uint16_t consumer = read16(REG_TPD_PRI0_CIDX);
    uint16_t next = (uint16_t)((tpd_next + 1U) % TX_SLOTS);
    if (next == consumer) return -1;

    memcpy(tx_buffers + (size_t)tpd_next * FRAME_BYTES, frame, length);
    struct atl1c_tpd *descriptor = &tpd_ring[tpd_next];
    descriptor->length = (uint16_t)length;
    descriptor->vlan = 0;
    descriptor->word1 = TPD_EOP;
    descriptor->address = tx_buffers_physical + (uint64_t)tpd_next * FRAME_BYTES;

    cpu_memory_barrier();
    tpd_next = next;
    write16(REG_TPD_PRI0_PIDX, tpd_next);
    flush_writes();
    tx_count++;
    return 0;
}

static void atl1c_poll(atl1c_receive_fn receive) {
    if (!available || !receive) return;

    uint64_t now = time_uptime_ns();
    if (now >= link_deadline) {
        link_deadline = now + 500000000ULL;
        update_link();
    }

    unsigned served = 0;
    unsigned returned = 0;
    while (served < 32U) {
        struct atl1c_rrd *status = &rrd_ring[rrd_next];
        if (!(status->word3 & RRS_UPDATED)) break;

        unsigned count = (status->word0 >> RRS_RFD_COUNT_SHIFT) & RRS_RFD_COUNT_MASK;
        unsigned index = (status->word0 >> RRS_RFD_INDEX_SHIFT) & RRS_RFD_INDEX_MASK;
        unsigned length = status->word3 & RRS_PKT_SIZE_MASK;

        if (count == 1U && index < RX_SLOTS && !(status->word3 & (RRS_ERR_SUM | RRS_LEN_ERR)) &&
            length > 4U && length <= FRAME_BYTES) {
            receive(rx_buffers + (size_t)index * FRAME_BYTES, length - 4U);
            rx_count++;
        } else {
            drop_count++;
        }

        status->word3 &= ~RRS_UPDATED;
        rrd_next = (uint16_t)((rrd_next + 1U) % RX_SLOTS);
        if (count < 1U) count = 1U;
        returned += count;
        served++;
    }

    if (!returned) return;
    rfd_next = (uint16_t)((rfd_next + returned) % RX_SLOTS);
    cpu_memory_barrier();
    write32(REG_MB_RFD0_PROD_IDX, rfd_next);
    flush_writes();
}

static uint64_t atl1c_rx_dropped(void) { return drop_count; }

static int atl1c_probe(const struct pci_device *found) {
    if (available) return -1;
    card = *found;

    uint64_t physical = pci_bar_address(&card, 0);
    if (!physical) {
        kprintf("ATL1C: no register window\n");
        return -1;
    }
    pci_enable_bus_mastering(&card);
    if (debug)
        kprintf("ATL1C: %x:%x at %x:%x.%x, registers at %x, irq %u\n",
                card.vendor_id, card.device_id, card.bus, card.slot, card.function,
                (unsigned)physical, card.irq_line);

    registers = vmm_map_device(physical, ATL1C_REGISTER_BYTES);
    if (!registers) {
        kprintf("ATL1C: cannot map registers at %x\n", (unsigned)physical);
        return -1;
    }

    if (load_mac_address() != 0) {
        kprintf("ATL1C: no station address in the hardware\n");
        return -1;
    }
    if (reset_mac() != 0) {
        kprintf("ATL1C: the controller will not go idle, status %x\n",
                read32(REG_IDLE_STATUS));
        return -1;
    }
    if (debug)
        kprintf("ATL1C: reset done, master %x idle %x\n", read32(REG_MASTER_CTRL),
                read32(REG_IDLE_STATUS));

    reset_phy();
    uint16_t identity = 0;
    if (phy_read(MII_PHYSID1, &identity) != 0 || identity == 0xFFFFU) {
        kprintf("ATL1C: the phy does not answer\n");
        return -1;
    }
    if (phy_write(MII_BMCR, BMCR_RESET) != 0) {
        kprintf("ATL1C: the phy will not reset\n");
        return -1;
    }
    for (unsigned attempt = 0; attempt < 100U; attempt++) {
        uint16_t control = 0;
        delay_ns(1000000ULL);
        if (phy_read(MII_BMCR, &control) == 0 && !(control & BMCR_RESET)) break;
    }
    if (phy_write(MII_ADVERTISE, ADVERTISE_ALL | ADVERTISE_PAUSE) != 0 ||
        phy_write(MII_CTRL1000, CTRL1000_FULL | CTRL1000_HALF) != 0 ||
        phy_write(MII_BMCR, BMCR_ANEG_EN | BMCR_RESTART_ANEG) != 0) {
        kprintf("ATL1C: the phy will not take a setting\n");
        return -1;
    }

    if (allocate_rings() != 0) {
        kprintf("ATL1C: out of DMA memory\n");
        release_rings();
        return -1;
    }
    if (debug)
        kprintf("ATL1C: rings rfd %x rrd %x tpd %x buffers %x\n",
                (unsigned)rfd_physical, (unsigned)rrd_physical,
                (unsigned)tpd_physical, (unsigned)rx_buffers_physical);

    rfd_next = RX_SLOTS - 1U;
    rrd_next = 0;
    tpd_next = 0;
    rx_count = tx_count = drop_count = 0;

    write_station_address(mac_address);
    configure_mac();
    link_speed = 100;
    link_duplex = 1;
    start_mac();
    write32(REG_MB_RFD0_PROD_IDX, rfd_next);
    flush_writes();

    available = 1;
    link_up = 0;
    link_state = LINK_UP;
    link_deadline = 0;
    uint16_t status = 0;
    uint16_t detail = 0;
    (void)phy_read(MII_BMSR, &status);
    (void)phy_read(MII_GIGA_PSSR, &detail);
    kprintf("ATL1C: %x:%x:%x:%x:%x:%x at %x:%x.%x, phy %x bmsr %x pssr %x\n",
            mac_address[0], mac_address[1], mac_address[2], mac_address[3],
            mac_address[4], mac_address[5], card.bus, card.slot, card.function,
            identity, status, detail);
    update_link();
    if (net_register_adapter(&atl1c_adapter) != 0) {
        kprintf("ATL1C: the stack already has an adapter\n");
        available = 0;
        stop_mac();
        release_rings();
        return -1;
    }
    return 0;
}

static void atl1c_remove(const struct pci_device *device) {
    (void)device;
    if (!available) return;
    available = 0;
    net_unregister_adapter(&atl1c_adapter);
    write32(REG_IMR, 0);
    stop_mac();
    release_rings();
}

static const struct net_adapter atl1c_adapter = {
    .name = "atl1c",
    .mac = mac_address,
    .transmit = atl1c_transmit,
    .poll = atl1c_poll,
    .rx_dropped = atl1c_rx_dropped,
    .enable_interrupts = NULL,
    .interrupt_vector = NULL,
};

static const struct pci_device_id atl1c_ids[] = {
    { ATL1C_VENDOR, ATL1C_AR8131, PCI_ANY_ID, PCI_ANY_ID },
    { ATL1C_VENDOR, ATL1C_AR8132, PCI_ANY_ID, PCI_ANY_ID },
};

static struct pci_driver atl1c_driver = {
    .name = "atl1c",
    .ids = atl1c_ids,
    .id_count = sizeof(atl1c_ids) / sizeof(atl1c_ids[0]),
    .probe = atl1c_probe,
    .remove = atl1c_remove,
};

static int atl1c_load(void) {
    return pci_register_driver(&atl1c_driver);
}

static void atl1c_unload(void) {
    pci_unregister_driver(&atl1c_driver);
}

MODULE_MAIN(atl1c_load, atl1c_unload);
MODULE_PCI_ALIAS("1969", "1063");
MODULE_PCI_ALIAS("1969", "1062");
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("Atheros AR8131/AR8132 gigabit Ethernet");
MODULE_AUTHOR("Tunix");
