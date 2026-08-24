/*
 * Intel High Definition Audio.
 *
 * The controller every PC built since 2004 has, and the one QEMU emulates. It
 * is really two devices: a DMA engine that walks a scatter list of the ring
 * buffer, and a serial link to one or more codecs that actually convert the
 * samples. Neither knows anything about the other until told, which is what
 * most of this file does.
 *
 * The codec side is a graph -- converters, mixers, selectors, pins -- and its
 * shape differs per machine, so the output path is discovered rather than
 * assumed. Everything below the graph walk is fixed by the specification.
 *
 * Position is read from the DMA position buffer and not from an interrupt:
 * the sound core asks for it whenever userspace syncs, which is often enough
 * for a ring measured in milliseconds and costs nothing when nothing plays.
 */
#include <stddef.h>
#include <stdint.h>

#include "../include/hda.h"
#include "../include/kstring.h"
#include "../include/pci.h"
#include "../include/pmm.h"
#include "../include/sound.h"
#include "../include/time.h"
#include "../include/vmm.h"

extern void kprintf(const char *fmt, ...);

#define PCI_CLASS_MULTIMEDIA 0x04U
#define PCI_SUBCLASS_HDA 0x03U
#define PCI_VENDOR_INTEL 0x8086U
/* Traffic class select. Intel controllers come out of reset on a class the
   chipset may not service; Linux clears it for the same reason. */
#define PCI_TCSEL 0x44U

#define BAR_IO 0x1U
#define BAR_TYPE_MASK 0x6U
#define BAR_TYPE_64BIT 0x4U
#define BAR_ADDRESS_MASK 0xFFFFFFF0U

/* Registers below 0x80 are the controller's; the stream descriptors follow. */
#define HDA_GCAP 0x00U
#define HDA_GCTL 0x08U
#define HDA_WAKEEN 0x0CU
#define HDA_STATESTS 0x0EU
#define HDA_INTCTL 0x20U
#define HDA_INTSTS 0x24U
#define HDA_CORBLBASE 0x40U
#define HDA_CORBUBASE 0x44U
#define HDA_CORBWP 0x48U
#define HDA_CORBRP 0x4AU
#define HDA_CORBCTL 0x4CU
#define HDA_CORBSIZE 0x4EU
#define HDA_RIRBLBASE 0x50U
#define HDA_RIRBUBASE 0x54U
#define HDA_RIRBWP 0x58U
#define HDA_RINTCNT 0x5AU
#define HDA_RIRBCTL 0x5CU
#define HDA_RIRBSTS 0x5DU
#define HDA_RIRBSIZE 0x5EU
#define HDA_DPLBASE 0x70U
#define HDA_DPUBASE 0x74U

#define GCTL_RESET 0x1U
#define CORBCTL_RUN 0x2U
#define CORBRP_RESET 0x8000U
#define RIRBCTL_RUN 0x2U
#define RIRBWP_RESET 0x8000U
#define RIRBSTS_CLEAR 0x5U
#define RIRB_INTERRUPT_COUNT 0xFFU
#define DPLBASE_ENABLE 0x1U

#define GCAP_ISS_SHIFT 8U
#define GCAP_ISS_MASK 0xFU
#define GCAP_OSS_SHIFT 12U
#define GCAP_OSS_MASK 0xFU

/* Stream descriptor, at STREAM_BASE + index * STREAM_STRIDE. */
#define STREAM_BASE 0x80U
#define STREAM_STRIDE 0x20U
#define SD_CTL 0x00U
#define SD_STS 0x03U
#define SD_LPIB 0x04U
#define SD_CBL 0x08U
#define SD_LVI 0x0CU
#define SD_FIFOS 0x10U
#define SD_FMT 0x12U
#define SD_BDPL 0x18U
#define SD_BDPU 0x1CU

#define SDCTL_RESET 0x1U
#define SDCTL_RUN 0x2U
#define SDCTL_STREAM_SHIFT 20U
#define SDSTS_CLEAR 0x1CU

/* Codec verbs. Anything above 0xF carries an eight-bit payload; the four-bit
   verbs carry sixteen, which is why the encoder splits on the value. */
#define VERB_GET_PARAMETER 0xF00U
#define VERB_GET_CONNECT_LIST 0xF02U
#define VERB_SET_CONNECT_SEL 0x701U
#define VERB_SET_POWER_STATE 0x705U
#define VERB_SET_STREAM_CHANNEL 0x706U
#define VERB_SET_PIN_CONTROL 0x707U
#define VERB_SET_EAPD 0x70CU
#define VERB_GET_CONFIG_DEFAULT 0xF1CU
#define VERB_SET_CONVERTER_FORMAT 0x2U
#define VERB_SET_AMP_GAIN_MUTE 0x3U

#define PARAM_NODE_COUNT 0x04U
#define PARAM_FUNCTION_TYPE 0x05U
#define PARAM_AUDIO_WIDGET_CAP 0x09U
#define PARAM_PIN_CAP 0x0CU
#define PARAM_IN_AMP_CAP 0x0DU
#define PARAM_CONNLIST_LEN 0x0EU
#define PARAM_OUT_AMP_CAP 0x12U

#define FUNCTION_TYPE_AUDIO 0x01U

#define WIDGET_TYPE_SHIFT 20U
#define WIDGET_TYPE_MASK 0xFU
#define WIDGET_OUTPUT 0x0U
#define WIDGET_MIXER 0x2U
#define WIDGET_SELECTOR 0x3U
#define WIDGET_PIN 0x4U

#define AWCAP_IN_AMP (1U << 1)
#define AWCAP_OUT_AMP (1U << 2)
#define AWCAP_AMP_OVERRIDE (1U << 3)
#define AWCAP_POWER (1U << 10)

#define PINCAP_OUTPUT (1U << 4)
#define PINCAP_HEADPHONE (1U << 3)
#define PINCAP_EAPD (1U << 16)

#define PIN_CONTROL_OUT 0x40U
#define PIN_CONTROL_HP 0x80U
#define EAPD_ENABLE 0x2U

/* Amp payload: which amp, which channels, which input index, then the gain. */
#define AMP_SET_OUTPUT 0x8000U
#define AMP_SET_INPUT 0x4000U
#define AMP_SET_LEFT 0x2000U
#define AMP_SET_RIGHT 0x1000U
#define AMP_INDEX_SHIFT 8U
#define AMP_MUTE 0x80U
#define AMP_GAIN_MASK 0x7FU
#define AMPCAP_OFFSET_MASK 0x7FU
#define AMPCAP_STEPS_SHIFT 8U
#define AMPCAP_STEPS_MASK 0x7FU

/* Config default: what the pin is wired to, and whether it is wired at all. */
#define CONFIG_DEVICE_SHIFT 20U
#define CONFIG_DEVICE_MASK 0xFU
#define CONFIG_DEVICE_LINE_OUT 0x0U
#define CONFIG_DEVICE_SPEAKER 0x1U
#define CONFIG_DEVICE_HEADPHONE 0x2U
#define CONFIG_CONNECTIVITY_SHIFT 30U
#define CONFIG_CONNECTIVITY_NONE 0x1U

#define CORB_ENTRIES 256U
#define RIRB_ENTRIES 256U
#define SIZE_256_ENTRIES 0x2U

#define MAX_WIDGETS 64U
#define MAX_CONNECTIONS 16U
#define MAX_PATH 8U
/* One page of BDL is 256 entries, and one entry maps one page of ring. */
#define MAX_BDL_ENTRIES 256U

#define MMIO_PAGE_BYTES 4096ULL
#define HDA_REGISTER_BYTES 0x4000ULL
/* The slot this driver owns in the shared device window; see vmm.h. */
#define HDA_MMIO_VIRTUAL_BASE (DEVICE_MMIO_VIRTUAL_BASE + 0x00500000ULL)

#define RESET_TIMEOUT_NS (500ULL * 1000ULL * 1000ULL)
#define VERB_TIMEOUT_NS (100ULL * 1000ULL * 1000ULL)
#define CODEC_SETTLE_NS (1000ULL * 1000ULL)

struct hda_widget {
    uint8_t nid;
    uint8_t type;
    uint8_t connection_count;
    uint8_t connections[MAX_CONNECTIONS];
    uint32_t caps;
    uint32_t pin_caps;
    uint32_t config;
    uint32_t out_amp_cap;
    uint32_t in_amp_cap;
};

struct hda_controller {
    int present;
    uint64_t base;
    uint32_t output_stream;    /* descriptor index, not the stream tag */
    uint32_t stream_tag;

    uint32_t *corb;
    uint64_t corb_physical;
    uint64_t *rirb;
    uint64_t rirb_physical;
    uint16_t corb_wp;
    uint16_t rirb_rp;

    uint32_t *position_buffer;
    uint64_t position_physical;
    uint64_t *bdl;
    uint64_t bdl_physical;

    uint8_t codec;
    uint8_t widget_count;
    struct hda_widget widgets[MAX_WIDGETS];

    /* The output path, from the pin back to the converter. */
    uint8_t path[MAX_PATH];
    uint8_t path_length;
    uint8_t dac_nid;
    uint8_t pin_nid;
    uint32_t volume_steps;

    /* The last configuration, kept because a prepare resets the descriptor
       registers and has to put them back. */
    uint32_t buffer_bytes;
    uint16_t stream_format;
    uint16_t bdl_entries;
};

static struct hda_controller hda;

static inline uint8_t read8(uint32_t offset) {
    return *(volatile uint8_t *)(hda.base + offset);
}

static inline void write8(uint32_t offset, uint8_t value) {
    *(volatile uint8_t *)(hda.base + offset) = value;
}

static inline uint16_t read16(uint32_t offset) {
    return *(volatile uint16_t *)(hda.base + offset);
}

static inline void write16(uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(hda.base + offset) = value;
}

static inline uint32_t read32(uint32_t offset) {
    return *(volatile uint32_t *)(hda.base + offset);
}

static inline void write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(hda.base + offset) = value;
}

static void delay_ns(uint64_t nanoseconds) {
    uint64_t deadline = time_uptime_ns() + nanoseconds;
    while (time_uptime_ns() < deadline) __asm__ volatile("pause");
}

/* One zeroed page of DMA memory. The allocator only hands out pages inside the
   direct map, so the kernel pointer is a subtraction away. */
static void *dma_page(uint64_t *physical_out) {
    void *physical = pmm_alloc_page();
    if (!physical) return NULL;
    void *virtual_address = vmm_phys_to_virt((uint64_t)physical);
    if (!virtual_address) return NULL;
    memset(virtual_address, 0, (size_t)PMM_PAGE_SIZE);
    *physical_out = (uint64_t)physical;
    return virtual_address;
}

static uint64_t map_registers(uint64_t physical) {
    if (physical & (MMIO_PAGE_BYTES - 1)) return 0;
    uint64_t cr3 = vmm_kernel_cr3();
    for (uint64_t offset = 0; offset < HDA_REGISTER_BYTES; offset += MMIO_PAGE_BYTES) {
        int status = vmm_map_page_in(cr3, HDA_MMIO_VIRTUAL_BASE + offset,
                                     physical + offset,
                                     PAGE_WRITE | PAGE_DEVICE | PAGE_UNCACHED | PAGE_NX);
        if (status != 0) {
            kprintf("HDA: map offset %x -> %d\n", (unsigned)offset, status);
            return 0;
        }
    }
    return HDA_MMIO_VIRTUAL_BASE;
}

/*
 * Bring the controller out of whatever state the firmware left it in. The
 * reset bit is inverted: zero means held in reset, and the codecs only start
 * announcing themselves once it has been one for a while.
 */
static int reset_controller(void) {
    write32(HDA_GCTL, read32(HDA_GCTL) & ~GCTL_RESET);
    uint64_t deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (read32(HDA_GCTL) & GCTL_RESET) {
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
    delay_ns(CODEC_SETTLE_NS);

    write32(HDA_GCTL, read32(HDA_GCTL) | GCTL_RESET);
    deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (!(read32(HDA_GCTL) & GCTL_RESET)) {
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
    /* The specification's codec discovery window, after which STATESTS is
       meaningful. Reading it earlier finds no codecs on real hardware. */
    delay_ns(CODEC_SETTLE_NS);
    return 0;
}

static int start_ring_buffers(void) {
    write8(HDA_CORBCTL, 0);
    write8(HDA_RIRBCTL, 0);

    hda.corb = dma_page(&hda.corb_physical);
    if (!hda.corb) return -1;
    hda.rirb = dma_page(&hda.rirb_physical);
    if (!hda.rirb) return -1;

    write8(HDA_CORBSIZE, SIZE_256_ENTRIES);
    write32(HDA_CORBLBASE, (uint32_t)hda.corb_physical);
    write32(HDA_CORBUBASE, (uint32_t)(hda.corb_physical >> 32));

    /* The read pointer resets by a write-then-clear handshake, and the
       controller answers each half in its own time. */
    write16(HDA_CORBRP, CORBRP_RESET);
    uint64_t deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (!(read16(HDA_CORBRP) & CORBRP_RESET)) {
        if (time_uptime_ns() >= deadline) break;
        __asm__ volatile("pause");
    }
    write16(HDA_CORBRP, 0);
    deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (read16(HDA_CORBRP) & CORBRP_RESET) {
        if (time_uptime_ns() >= deadline) return -1;
        __asm__ volatile("pause");
    }
    write16(HDA_CORBWP, 0);
    hda.corb_wp = 0;

    write8(HDA_RIRBSIZE, SIZE_256_ENTRIES);
    write32(HDA_RIRBLBASE, (uint32_t)hda.rirb_physical);
    write32(HDA_RIRBUBASE, (uint32_t)(hda.rirb_physical >> 32));
    write16(HDA_RIRBWP, RIRBWP_RESET);
    /*
     * Not 1, which is what an interrupt-driven driver asks for.
     *
     * The response-interrupt count is also the point at which the controller
     * stops taking commands until the status bit it raised is acknowledged. At
     * 1 that is every single verb, and one missed acknowledgement wedges the
     * command ring for good -- which is exactly what happened here: the second
     * verb of codec enumeration never left the CORB. A high count means the
     * interlock is reached rarely, and the acknowledgement after every response
     * below clears it when it is.
     */
    write16(HDA_RINTCNT, RIRB_INTERRUPT_COUNT);
    hda.rirb_rp = 0;

    write8(HDA_CORBCTL, CORBCTL_RUN);
    write8(HDA_RIRBCTL, RIRBCTL_RUN);
    return 0;
}

/* Send one verb and wait for its response. Returns ~0 on timeout, which no
   valid parameter read produces for the fields this driver uses. */
static uint32_t codec_command(uint8_t nid, uint32_t verb, uint32_t payload) {
    uint32_t value = ((uint32_t)hda.codec << 28) | ((uint32_t)nid << 20);
    if (verb > 0xFU) value |= (verb << 8) | (payload & 0xFFU);
    else value |= (verb << 16) | (payload & 0xFFFFU);

    hda.corb_wp = (uint16_t)((hda.corb_wp + 1U) % CORB_ENTRIES);
    hda.corb[hda.corb_wp] = value;
    __asm__ volatile("mfence" : : : "memory");
    write16(HDA_CORBWP, hda.corb_wp);

    uint64_t deadline = time_uptime_ns() + VERB_TIMEOUT_NS;
    for (;;) {
        uint16_t write_pointer = (uint16_t)(read16(HDA_RIRBWP) & 0xFFU);
        if (write_pointer != hda.rirb_rp) break;
        if (time_uptime_ns() >= deadline) {
            kprintf("HDA: verb %x got no response\n", value);
            return 0xFFFFFFFFU;
        }
        __asm__ volatile("pause");
    }
    hda.rirb_rp = (uint16_t)((hda.rirb_rp + 1U) % RIRB_ENTRIES);
    __asm__ volatile("mfence" : : : "memory");
    uint64_t response = hda.rirb[hda.rirb_rp];
    /* Written unconditionally rather than read-modify-write: both bits are
       write-one-to-clear, and it is the clearing that lets the ring run on. */
    write8(HDA_RIRBSTS, RIRBSTS_CLEAR);
    return (uint32_t)response;
}

static uint32_t get_parameter(uint8_t nid, uint32_t parameter) {
    return codec_command(nid, VERB_GET_PARAMETER, parameter);
}

static struct hda_widget *widget_for(uint8_t nid) {
    for (unsigned index = 0; index < hda.widget_count; index++)
        if (hda.widgets[index].nid == nid) return &hda.widgets[index];
    return NULL;
}

/* The connection list is four entries per response in short form and two in
   long form; ranges are rare on output paths and treated as their endpoint. */
static void read_connections(struct hda_widget *widget) {
    uint32_t length_parameter = get_parameter(widget->nid, PARAM_CONNLIST_LEN);
    unsigned count = length_parameter & 0x7FU;
    int long_form = (length_parameter & 0x80U) != 0;
    if (count > MAX_CONNECTIONS) count = MAX_CONNECTIONS;

    unsigned per_response = long_form ? 2U : 4U;
    for (unsigned index = 0; index < count; index += per_response) {
        uint32_t response = codec_command(widget->nid, VERB_GET_CONNECT_LIST, index);
        for (unsigned slot = 0; slot < per_response && index + slot < count; slot++) {
            uint32_t entry = long_form ? ((response >> (slot * 16U)) & 0xFFFFU)
                                       : ((response >> (slot * 8U)) & 0xFFU);
            widget->connections[index + slot] = (uint8_t)(entry & 0x7FU);
        }
    }
    widget->connection_count = (uint8_t)count;
}

/* Walk the audio function group and record every widget in it. */
static int enumerate_widgets(uint8_t function_group) {
    uint32_t nodes = get_parameter(function_group, PARAM_NODE_COUNT);
    uint8_t start = (uint8_t)((nodes >> 16) & 0xFFU);
    unsigned count = nodes & 0xFFU;
    if (!count) return -1;
    if (count > MAX_WIDGETS) count = MAX_WIDGETS;

    uint32_t group_out_amp = get_parameter(function_group, PARAM_OUT_AMP_CAP);
    uint32_t group_in_amp = get_parameter(function_group, PARAM_IN_AMP_CAP);

    hda.widget_count = 0;
    for (unsigned index = 0; index < count; index++) {
        struct hda_widget *widget = &hda.widgets[hda.widget_count];
        memset(widget, 0, sizeof(*widget));
        widget->nid = (uint8_t)(start + index);
        widget->caps = get_parameter(widget->nid, PARAM_AUDIO_WIDGET_CAP);
        if (widget->caps == 0xFFFFFFFFU) continue;
        widget->type = (uint8_t)((widget->caps >> WIDGET_TYPE_SHIFT) & WIDGET_TYPE_MASK);

        /* Amp capabilities live on the function group unless the widget says
           it overrides them, so a widget-local read can legitimately be zero. */
        if (widget->caps & AWCAP_AMP_OVERRIDE) {
            widget->out_amp_cap = get_parameter(widget->nid, PARAM_OUT_AMP_CAP);
            widget->in_amp_cap = get_parameter(widget->nid, PARAM_IN_AMP_CAP);
        } else {
            widget->out_amp_cap = group_out_amp;
            widget->in_amp_cap = group_in_amp;
        }

        if (widget->type == WIDGET_PIN) {
            widget->pin_caps = get_parameter(widget->nid, PARAM_PIN_CAP);
            widget->config = codec_command(widget->nid, VERB_GET_CONFIG_DEFAULT, 0);
        }
        read_connections(widget);
        hda.widget_count++;
    }
    return hda.widget_count ? 0 : -1;
}

/*
 * Find a route from an output pin back to a converter.
 *
 * Breadth first over the connection lists, which are directed the way the
 * signal flows into a widget, so following them walks upstream. The parent
 * table then gives the path back in playback order.
 */
static int find_path(uint8_t pin_nid) {
    uint8_t queue[MAX_WIDGETS];
    uint8_t parent[MAX_WIDGETS];
    uint8_t parent_index[MAX_WIDGETS];
    int seen[MAX_WIDGETS];
    memset(seen, 0, sizeof(seen));
    memset(parent, 0, sizeof(parent));
    memset(parent_index, 0, sizeof(parent_index));

    struct hda_widget *pin = widget_for(pin_nid);
    if (!pin) return -1;

    unsigned head = 0;
    unsigned tail = 0;
    queue[tail++] = pin_nid;
    seen[pin - hda.widgets] = 1;

    uint8_t found = 0;
    while (head < tail && !found) {
        uint8_t nid = queue[head++];
        struct hda_widget *widget = widget_for(nid);
        if (!widget) continue;
        for (unsigned slot = 0; slot < widget->connection_count; slot++) {
            uint8_t next = widget->connections[slot];
            struct hda_widget *target = widget_for(next);
            if (!target) continue;
            unsigned target_index = (unsigned)(target - hda.widgets);
            if (seen[target_index]) continue;
            seen[target_index] = 1;
            parent[target_index] = nid;
            parent_index[target_index] = (uint8_t)slot;
            if (target->type == WIDGET_OUTPUT) {
                found = next;
                break;
            }
            if (target->type == WIDGET_MIXER || target->type == WIDGET_SELECTOR) {
                if (tail < MAX_WIDGETS) queue[tail++] = next;
            }
        }
    }
    if (!found) return -1;

    /* Unwind to the pin, then reverse: the path is used from the pin down. */
    uint8_t reverse[MAX_PATH];
    unsigned length = 0;
    uint8_t nid = found;
    while (length < MAX_PATH) {
        reverse[length++] = nid;
        if (nid == pin_nid) break;
        struct hda_widget *widget = widget_for(nid);
        if (!widget) return -1;
        nid = parent[widget - hda.widgets];
    }
    if (length >= MAX_PATH || reverse[length - 1] != pin_nid) return -1;

    hda.path_length = (uint8_t)length;
    for (unsigned index = 0; index < length; index++)
        hda.path[index] = reverse[length - 1 - index];
    hda.pin_nid = pin_nid;
    hda.dac_nid = found;

    /* Point every selector on the path at the hop that was taken. */
    for (unsigned index = 0; index + 1 < length; index++) {
        struct hda_widget *widget = widget_for(hda.path[index]);
        struct hda_widget *next = widget_for(hda.path[index + 1]);
        if (!widget || !next) continue;
        if (widget->connection_count > 1)
            (void)codec_command(widget->nid, VERB_SET_CONNECT_SEL,
                                parent_index[next - hda.widgets]);
    }
    return 0;
}

static uint32_t amp_offset(uint32_t capability) {
    return capability & AMPCAP_OFFSET_MASK;
}

static uint32_t amp_steps(uint32_t capability) {
    return (capability >> AMPCAP_STEPS_SHIFT) & AMPCAP_STEPS_MASK;
}

static void set_output_amp(struct hda_widget *widget, uint32_t gain, int muted) {
    if (!(widget->caps & AWCAP_OUT_AMP)) return;
    uint32_t payload = AMP_SET_OUTPUT | AMP_SET_LEFT | AMP_SET_RIGHT |
                       (gain & AMP_GAIN_MASK) | (muted ? AMP_MUTE : 0U);
    (void)codec_command(widget->nid, VERB_SET_AMP_GAIN_MUTE, payload);
}

static void unmute_input_amp(struct hda_widget *widget, unsigned index) {
    if (!(widget->caps & AWCAP_IN_AMP)) return;
    uint32_t payload = AMP_SET_INPUT | AMP_SET_LEFT | AMP_SET_RIGHT |
                       ((index & 0xFU) << AMP_INDEX_SHIFT) |
                       (amp_offset(widget->in_amp_cap) & AMP_GAIN_MASK);
    (void)codec_command(widget->nid, VERB_SET_AMP_GAIN_MUTE, payload);
}

/* Power everything on the path, open its amps, and enable the jack. */
static void enable_path(void) {
    for (unsigned index = 0; index < hda.path_length; index++) {
        struct hda_widget *widget = widget_for(hda.path[index]);
        if (!widget) continue;
        if (widget->caps & AWCAP_POWER)
            (void)codec_command(widget->nid, VERB_SET_POWER_STATE, 0);
        set_output_amp(widget, amp_offset(widget->out_amp_cap), 0);
        /* Index zero is the selected input once the selector has been set. */
        unmute_input_amp(widget, 0);
    }

    struct hda_widget *pin = widget_for(hda.pin_nid);
    if (!pin) return;
    uint32_t control = PIN_CONTROL_OUT;
    if (pin->pin_caps & PINCAP_HEADPHONE) control |= PIN_CONTROL_HP;
    (void)codec_command(pin->nid, VERB_SET_PIN_CONTROL, control);
    if (pin->pin_caps & PINCAP_EAPD)
        (void)codec_command(pin->nid, VERB_SET_EAPD, EAPD_ENABLE);
}

/* Prefer a pin that is wired to something and meant for listening. */
static int pin_is_output(const struct hda_widget *widget, int strict) {
    if (widget->type != WIDGET_PIN) return 0;
    if (!(widget->pin_caps & PINCAP_OUTPUT)) return 0;
    if (!strict) return 1;
    if (((widget->config >> CONFIG_CONNECTIVITY_SHIFT) & 0x3U) ==
        CONFIG_CONNECTIVITY_NONE) return 0;
    uint32_t device = (widget->config >> CONFIG_DEVICE_SHIFT) & CONFIG_DEVICE_MASK;
    return device == CONFIG_DEVICE_LINE_OUT || device == CONFIG_DEVICE_SPEAKER ||
           device == CONFIG_DEVICE_HEADPHONE;
}

static int build_output_path(void) {
    for (int strict = 1; strict >= 0; strict--) {
        for (unsigned index = 0; index < hda.widget_count; index++) {
            if (!pin_is_output(&hda.widgets[index], strict)) continue;
            if (find_path(hda.widgets[index].nid) == 0) return 0;
        }
    }
    return -1;
}

/* The first audio function group of the first codec that answers. */
static int probe_codecs(void) {
    uint16_t present = read16(HDA_STATESTS);
    for (uint8_t address = 0; address < 15U; address++) {
        if (!(present & (1U << address))) continue;
        hda.codec = address;
        uint32_t nodes = get_parameter(0, PARAM_NODE_COUNT);
        if (nodes == 0xFFFFFFFFU) continue;
        uint8_t start = (uint8_t)((nodes >> 16) & 0xFFU);
        unsigned count = nodes & 0xFFU;
        for (unsigned index = 0; index < count; index++) {
            uint8_t nid = (uint8_t)(start + index);
            uint32_t type = get_parameter(nid, PARAM_FUNCTION_TYPE);
            if ((type & 0xFFU) != FUNCTION_TYPE_AUDIO) continue;
            (void)codec_command(nid, VERB_SET_POWER_STATE, 0);
            delay_ns(CODEC_SETTLE_NS);
            if (enumerate_widgets(nid) != 0) continue;
            if (build_output_path() != 0) continue;
            enable_path();
            return 0;
        }
    }
    return -1;
}

/* Sample rates the link can carry, with the base/multiplier/divisor triple the
   format register spells them as. */
struct rate_encoding {
    uint32_t rate;
    uint16_t bits;
};

#define RATE_BASE_44100 (1U << 14)
#define RATE_MULT_SHIFT 11U
#define RATE_DIV_SHIFT 8U

static const struct rate_encoding rate_table[] = {
    { 8000, (5U << RATE_DIV_SHIFT) },
    { 11025, RATE_BASE_44100 | (3U << RATE_DIV_SHIFT) },
    { 16000, (2U << RATE_DIV_SHIFT) },
    { 22050, RATE_BASE_44100 | (1U << RATE_DIV_SHIFT) },
    { 32000, (1U << RATE_MULT_SHIFT) | (2U << RATE_DIV_SHIFT) },
    { 44100, RATE_BASE_44100 },
    { 48000, 0 },
    { 88200, RATE_BASE_44100 | (1U << RATE_MULT_SHIFT) },
    { 96000, (1U << RATE_MULT_SHIFT) },
    { 176400, RATE_BASE_44100 | (3U << RATE_MULT_SHIFT) },
    { 192000, (3U << RATE_MULT_SHIFT) }
};

static const uint32_t supported_rates[] = {
    8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000
};

static int encode_format(const struct snd_stream_format *format, uint16_t *out) {
    uint16_t value = 0;
    unsigned index = 0;
    for (; index < sizeof(rate_table) / sizeof(rate_table[0]); index++)
        if (rate_table[index].rate == format->rate) break;
    if (index == sizeof(rate_table) / sizeof(rate_table[0])) return -1;
    value |= rate_table[index].bits;

    if (format->format == SND_FORMAT_S16_LE) value |= 1U << 4;
    else if (format->format == SND_FORMAT_S32_LE) value |= 4U << 4;
    else return -1;

    if (!format->channels || format->channels > 16U) return -1;
    value |= (uint16_t)((format->channels - 1U) & 0xFU);
    *out = value;
    return 0;
}

static uint32_t stream_register(uint32_t offset) {
    return STREAM_BASE + hda.output_stream * STREAM_STRIDE + offset;
}

static void stop_stream(void) {
    uint32_t control = read32(stream_register(SD_CTL));
    write32(stream_register(SD_CTL), control & ~SDCTL_RUN);
    uint64_t deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (read32(stream_register(SD_CTL)) & SDCTL_RUN) {
        if (time_uptime_ns() >= deadline) break;
        __asm__ volatile("pause");
    }
    write8(stream_register(SD_STS), SDSTS_CLEAR);
}

/* The engine has to be reset before its descriptors may be rewritten, and the
   reset bit is another write-then-clear handshake. */
static void reset_stream(void) {
    stop_stream();
    write32(stream_register(SD_CTL), SDCTL_RESET);
    uint64_t deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (!(read32(stream_register(SD_CTL)) & SDCTL_RESET)) {
        if (time_uptime_ns() >= deadline) break;
        __asm__ volatile("pause");
    }
    write32(stream_register(SD_CTL), 0);
    deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (read32(stream_register(SD_CTL)) & SDCTL_RESET) {
        if (time_uptime_ns() >= deadline) break;
        __asm__ volatile("pause");
    }
}

/* Reset the engine and write the stored descriptors back into it. The reset is
   what puts the position counters at zero, so this is also the prepare path. */
static int program_stream(void) {
    if (!hda.bdl_entries) return -1;
    reset_stream();

    write32(stream_register(SD_CBL), hda.buffer_bytes);
    write16(stream_register(SD_LVI), (uint16_t)(hda.bdl_entries - 1U));
    write16(stream_register(SD_FMT), hda.stream_format);
    write32(stream_register(SD_BDPL), (uint32_t)hda.bdl_physical);
    write32(stream_register(SD_BDPU), (uint32_t)(hda.bdl_physical >> 32));
    write32(stream_register(SD_CTL), hda.stream_tag << SDCTL_STREAM_SHIFT);
    hda.position_buffer[hda.output_stream * 2U] = 0;

    /* The converter has to be told the same format and which stream tag on the
       link carries it; a mismatch here plays silence with everything running. */
    (void)codec_command(hda.dac_nid, VERB_SET_CONVERTER_FORMAT, hda.stream_format);
    (void)codec_command(hda.dac_nid, VERB_SET_STREAM_CHANNEL, hda.stream_tag << 4);
    return 0;
}

static int hda_configure(const struct snd_stream_format *format,
                         const uint64_t *pages, unsigned page_count,
                         uint32_t buffer_bytes) {
    if (!hda.present || !pages || !page_count || !buffer_bytes) return -1;
    if (buffer_bytes > page_count * (uint32_t)PMM_PAGE_SIZE) return -1;
    uint16_t encoded = 0;
    if (encode_format(format, &encoded) != 0) return -1;

    /* The scatter list must have at least two entries, so a ring smaller than
       a page is described in halves rather than one entry. Every chunk is a
       power of two no larger than a page, which keeps it inside one. */
    uint32_t chunk = (uint32_t)PMM_PAGE_SIZE;
    while (chunk > 128U && buffer_bytes <= chunk) chunk /= 2U;

    unsigned entries = 0;
    for (uint32_t offset = 0; offset < buffer_bytes; offset += chunk) {
        if (entries >= MAX_BDL_ENTRIES) return -1;
        uint32_t length = buffer_bytes - offset < chunk ? buffer_bytes - offset : chunk;
        hda.bdl[entries * 2U] = pages[offset / (uint32_t)PMM_PAGE_SIZE] +
                                (offset % (uint32_t)PMM_PAGE_SIZE);
        hda.bdl[entries * 2U + 1U] = length;
        entries++;
    }
    if (entries < 2U) return -1;

    hda.buffer_bytes = buffer_bytes;
    hda.stream_format = encoded;
    hda.bdl_entries = (uint16_t)entries;
    return program_stream();
}

static int hda_prepare(void) {
    if (!hda.present) return -1;
    return program_stream();
}

static int hda_trigger(int running) {
    if (!hda.present) return -1;
    if (!running) {
        stop_stream();
        return 0;
    }
    write8(stream_register(SD_STS), SDSTS_CLEAR);
    uint32_t control = read32(stream_register(SD_CTL));
    write32(stream_register(SD_CTL), control | SDCTL_RUN);
    return 0;
}

static uint32_t hda_position(void) {
    if (!hda.present || !hda.buffer_bytes) return 0;
    /* The position buffer is DMA-coherent memory and cheaper to trust than
       LPIB, which lags on several chipsets; LPIB covers a controller that
       never writes it. */
    uint32_t position = hda.position_buffer[hda.output_stream * 2U];
    if (!position) position = read32(stream_register(SD_LPIB));
    return position % hda.buffer_bytes;
}

static int hda_set_volume(uint32_t left, uint32_t right, int muted) {
    struct hda_widget *dac = widget_for(hda.dac_nid);
    if (!dac) return -1;
    uint32_t payload = AMP_SET_OUTPUT | AMP_SET_LEFT | (left & AMP_GAIN_MASK) |
                       (muted ? AMP_MUTE : 0U);
    (void)codec_command(dac->nid, VERB_SET_AMP_GAIN_MUTE, payload);
    payload = AMP_SET_OUTPUT | AMP_SET_RIGHT | (right & AMP_GAIN_MASK) |
              (muted ? AMP_MUTE : 0U);
    (void)codec_command(dac->nid, VERB_SET_AMP_GAIN_MUTE, payload);
    return 0;
}

static struct snd_backend hda_backend = {
    .driver = "HDA-Intel",
    .name = "HDA Intel",
    .mixer_name = "Realtek HDA",
    .components = "HDA:hda-generic",
    .hardware = {
        .rate_min = 8000,
        .rate_max = 192000,
        .rates = supported_rates,
        .rate_count = sizeof(supported_rates) / sizeof(supported_rates[0]),
        .channels_min = 2,
        .channels_max = 2,
        .formats = (1U << SND_FORMAT_S16_LE) | (1U << SND_FORMAT_S32_LE),
        /* Both bounds are the controller's: a cyclic buffer length must be a
           multiple of 128 bytes, and the scatter list holds one page each. */
        .period_bytes_min = 128,
        .period_bytes_max = 128 * 1024,
        .periods_min = 2,
        .periods_max = 32,
        .buffer_bytes_max = MAX_BDL_ENTRIES * 4096U,
        .fifo_size = 0
    },
    .configure = hda_configure,
    .prepare = hda_prepare,
    .trigger = hda_trigger,
    .position = hda_position,
    .volume_max = 0,
    .set_volume = hda_set_volume
};

int hda_init(void) {
    struct pci_device device;
    memset(&hda, 0, sizeof(hda));
    if (pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_HDA, &device) != 0)
        return -1;
    if (device.bar[0] & BAR_IO) return -1;

    uint64_t physical = device.bar[0] & BAR_ADDRESS_MASK;
    if ((device.bar[0] & BAR_TYPE_MASK) == BAR_TYPE_64BIT)
        physical |= (uint64_t)device.bar[1] << 32;
    if (!physical) {
        kprintf("HDA: controller has no register window\n");
        return -1;
    }

    pci_enable_bus_mastering(&device);
    if (device.vendor_id == PCI_VENDOR_INTEL) {
        uint32_t tcsel = pci_config_read32(device.bus, device.slot, device.function,
                                           PCI_TCSEL);
        pci_config_write32(device.bus, device.slot, device.function, PCI_TCSEL,
                           tcsel & ~0x7U);
    }

    hda.base = map_registers(physical);
    if (!hda.base) {
        kprintf("HDA: cannot map registers at %x\n", (unsigned)physical);
        return -1;
    }

    /* Nothing here uses interrupts; masking them keeps a controller the
       firmware left armed from raising a line no handler owns. */
    write32(HDA_INTCTL, 0);
    if (reset_controller() != 0) {
        kprintf("HDA: controller will not reset\n");
        return -1;
    }
    write16(HDA_WAKEEN, 0);
    write32(HDA_INTCTL, 0);
    write32(HDA_INTSTS, read32(HDA_INTSTS));

    uint16_t capabilities = read16(HDA_GCAP);
    uint32_t input_streams = (capabilities >> GCAP_ISS_SHIFT) & GCAP_ISS_MASK;
    uint32_t output_streams = (capabilities >> GCAP_OSS_SHIFT) & GCAP_OSS_MASK;
    if (!output_streams) {
        kprintf("HDA: controller has no output streams\n");
        return -1;
    }
    /* Output descriptors follow the input ones in the register file, so the
       first output stream is at the input count. */
    hda.output_stream = input_streams;
    hda.stream_tag = 1;

    if (start_ring_buffers() != 0) {
        kprintf("HDA: command ring setup failed\n");
        return -1;
    }

    hda.bdl = dma_page(&hda.bdl_physical);
    hda.position_buffer = dma_page(&hda.position_physical);
    if (!hda.bdl || !hda.position_buffer) {
        kprintf("HDA: out of DMA memory\n");
        return -1;
    }
    write32(HDA_DPLBASE, (uint32_t)hda.position_physical | DPLBASE_ENABLE);
    write32(HDA_DPUBASE, (uint32_t)(hda.position_physical >> 32));

    if (probe_codecs() != 0) {
        kprintf("HDA: no usable output path on any codec\n");
        return -1;
    }

    struct hda_widget *dac = widget_for(hda.dac_nid);
    hda.volume_steps = dac ? amp_steps(dac->out_amp_cap) : 0;
    hda_backend.volume_max = hda.volume_steps;
    if (!hda.volume_steps) hda_backend.set_volume = NULL;

    hda.present = 1;
    kprintf("HDA: codec %u, dac nid %u, pin nid %u, stream %u\n",
            (unsigned)hda.codec, (unsigned)hda.dac_nid, (unsigned)hda.pin_nid,
            (unsigned)hda.output_stream);
    return snd_register_card(&hda_backend);
}
