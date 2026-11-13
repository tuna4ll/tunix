#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/heap.h>
#include <tunix/irq.h>
#include <tunix/kprintf.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/module.h>
#include <tunix/pci.h>
#include <tunix/pmm.h>
#include <tunix/sound.h>
#include <tunix/time.h>
#include <tunix/vmm.h>

#define PCI_CLASS_MULTIMEDIA 0x04U
#define PCI_SUBCLASS_HDA     0x03U
#define PCI_VENDOR_INTEL     0x8086U
#define PCI_VENDOR_NVIDIA    0x10DEU
#define PCI_VENDOR_AMD       0x1002U
#define PCI_TCSEL            0x44U
#define PCI_CAP_EXPRESS      0x10U
#define PCI_CAP_MSI          0x05U
#define PCIE_DEVCTL_NOSNOOP  (1U << 11)

#define BAR_IO           0x1U
#define BAR_TYPE_MASK    0x6U
#define BAR_TYPE_64BIT   0x4U
#define BAR_ADDRESS_MASK 0xFFFFFFF0U

#define HDA_GCAP      0x00U
#define HDA_GCTL      0x08U
#define HDA_WAKEEN    0x0CU
#define HDA_STATESTS  0x0EU
#define HDA_INTCTL    0x20U
#define HDA_INTSTS    0x24U
#define HDA_CORBLBASE 0x40U
#define HDA_CORBUBASE 0x44U
#define HDA_CORBWP    0x48U
#define HDA_CORBRP    0x4AU
#define HDA_CORBCTL   0x4CU
#define HDA_CORBSIZE  0x4EU
#define HDA_RIRBLBASE 0x50U
#define HDA_RIRBUBASE 0x54U
#define HDA_RIRBWP    0x58U
#define HDA_RINTCNT   0x5AU
#define HDA_RIRBCTL   0x5CU
#define HDA_RIRBSTS   0x5DU
#define HDA_RIRBSIZE  0x5EU
#define HDA_DPLBASE   0x70U
#define HDA_DPUBASE   0x74U

#define GCTL_RESET     0x1U
#define GCTL_UNSOL     0x100U
#define CORBCTL_RUN    0x2U
#define CORBRP_RESET   0x8000U
#define RIRBCTL_RUN    0x2U
#define RIRBCTL_INT    0x1U
#define RIRBWP_RESET   0x8000U
#define RIRBSTS_CLEAR  0x5U
#define DPLBASE_ENABLE 0x1U
#define INTCTL_GIE     0x80000000U

#define GCAP_ISS_SHIFT  8U
#define GCAP_OSS_SHIFT  12U
#define GCAP_COUNT_MASK 0xFU

#define STREAM_BASE   0x80U
#define STREAM_STRIDE 0x20U
#define SD_CTL        0x00U
#define SD_STS        0x03U
#define SD_LPIB       0x04U
#define SD_CBL        0x08U
#define SD_LVI        0x0CU
#define SD_FMT        0x12U
#define SD_BDPL       0x18U
#define SD_BDPU       0x1CU

#define SDCTL_RESET        0x1U
#define SDCTL_RUN          0x2U
#define SDCTL_IOCE         0x4U
#define SDCTL_STREAM_SHIFT 20U
#define SDSTS_BCIS         0x4U
#define SDSTS_CLEAR        0x1CU

#define VERB_GET_PARAMETER        0xF00U
#define VERB_GET_CONNECT_LIST     0xF02U
#define VERB_GET_CONNECT_SEL      0xF01U
#define VERB_GET_PIN_CONTROL      0xF07U
#define VERB_GET_EAPD             0xF0CU
#define VERB_GET_PIN_SENSE        0xF09U
#define VERB_GET_CONFIG_DEFAULT   0xF1CU
#define VERB_GET_SUBSYSTEM_ID     0xF20U
#define VERB_SET_CONNECT_SEL      0x701U
#define VERB_SET_POWER_STATE      0x705U
#define VERB_SET_STREAM_CHANNEL   0x706U
#define VERB_SET_PIN_CONTROL      0x707U
#define VERB_SET_PIN_SENSE        0x709U
#define VERB_SET_EAPD             0x70CU
#define VERB_SET_CONVERTER_FORMAT 0x2U
#define VERB_SET_AMP_GAIN_MUTE    0x3U

#define PARAM_VENDOR_ID        0x00U
#define PARAM_REVISION_ID      0x02U
#define PARAM_NODE_COUNT       0x04U
#define PARAM_FUNCTION_TYPE    0x05U
#define PARAM_AUDIO_WIDGET_CAP 0x09U
#define PARAM_PCM              0x0AU
#define PARAM_STREAM_FORMATS   0x0BU
#define PARAM_PIN_CAP          0x0CU
#define PARAM_IN_AMP_CAP       0x0DU
#define PARAM_CONNLIST_LEN     0x0EU
#define PARAM_OUT_AMP_CAP      0x12U

#define FUNCTION_TYPE_AUDIO 0x01U

#define WIDGET_OUTPUT   0x0U
#define WIDGET_INPUT    0x1U
#define WIDGET_MIXER    0x2U
#define WIDGET_SELECTOR 0x3U
#define WIDGET_PIN      0x4U

#define AWCAP_IN_AMP       (1U << 1)
#define AWCAP_OUT_AMP      (1U << 2)
#define AWCAP_AMP_OVERRIDE (1U << 3)
#define AWCAP_FORMAT_OVRD  (1U << 4)
#define AWCAP_CONN_LIST    (1U << 8)
#define AWCAP_DIGITAL      (1U << 9)
#define AWCAP_POWER        (1U << 10)

#define PINCAP_TRIGGER  (1U << 1)
#define PINCAP_PRESENCE (1U << 2)
#define PINCAP_HP_DRIVE (1U << 3)
#define PINCAP_OUTPUT   (1U << 4)
#define PINCAP_INPUT    (1U << 5)
#define PINCAP_HDMI     (1U << 7)
#define PINCAP_EAPD     (1U << 16)
#define PINCAP_DP       (1U << 24)
#define PINCAP_VREF_50  (1U << 9)
#define PINCAP_VREF_80  (1U << 12)
#define PINCAP_VREF_100 (1U << 13)

#define PIN_IN       0x20U
#define PIN_OUT      0x40U
#define PIN_HP       0x80U
#define PIN_VREF_50  0x1U
#define PIN_VREF_80  0x4U
#define PIN_VREF_100 0x5U
#define EAPD_ENABLE  0x2U

#define AMP_SET_OUTPUT  0x8000U
#define AMP_SET_INPUT   0x4000U
#define AMP_SET_LEFT    0x2000U
#define AMP_SET_RIGHT   0x1000U
#define AMP_INDEX_SHIFT 8U
#define AMP_MUTE        0x80U

#define CONFIG_PORT_SHIFT     30U
#define CONFIG_PORT_JACK      0x0U
#define CONFIG_PORT_NONE      0x1U
#define CONFIG_PORT_FIXED     0x2U
#define CONFIG_LOCATION_SHIFT 24U
#define CONFIG_DEVICE_SHIFT   20U
#define CONFIG_MISC_NO_DETECT (1U << 8)
#define CONFIG_ASSOC_SHIFT    4U

#define DEVICE_LINE_OUT  0x0U
#define DEVICE_SPEAKER   0x1U
#define DEVICE_HEADPHONE 0x2U
#define DEVICE_LINE_IN   0x8U
#define DEVICE_AUX       0x9U
#define DEVICE_MIC       0xAU

#define CORB_ENTRIES 256U
#define RIRB_ENTRIES 256U

#define MAX_NODES       128U
#define MAX_CONNECTIONS 32U
#define MAX_PATH        10U
#define MAX_OUTPUTS     8U
#define MAX_INPUTS      6U
#define MAX_BDL_ENTRIES 256U
#define MAX_CONTROLS    40U
#define MAX_CONTROLLERS 4U

#define HDA_REGISTER_BYTES 0x4000ULL
#define RESET_TIMEOUT_NS   (500ULL * 1000ULL * 1000ULL)
#define VERB_TIMEOUT_NS    (50ULL * 1000ULL * 1000ULL)
#define CODEC_SETTLE_NS    (1000ULL * 1000ULL)

enum role {
    ROLE_LINE_OUT,
    ROLE_SPEAKER,
    ROLE_HEADPHONE,
    ROLE_MIC,
    ROLE_INTERNAL_MIC,
    ROLE_LINE_IN,
};

enum control_kind {
    CTL_MASTER_VOLUME,
    CTL_MASTER_SWITCH,
    CTL_OUTPUT_SWITCH,
    CTL_AUTO_MUTE,
    CTL_CAPTURE_VOLUME,
    CTL_CAPTURE_SWITCH,
    CTL_INPUT_SOURCE,
    CTL_BOOST,
    CTL_JACK,
};

struct widget {
    uint8_t nid;
    uint8_t type;
    uint8_t connection_count;
    uint8_t connections[MAX_CONNECTIONS];
    uint32_t caps;
    uint32_t pin_caps;
    uint32_t config;
    uint32_t out_amp_cap;
    uint32_t in_amp_cap;
    uint32_t pcm_caps;
};

struct step {
    uint8_t nid;
    uint8_t index;
};

struct output_path {
    uint8_t pin;
    uint8_t dac;
    uint8_t role;
    uint8_t length;
    struct step steps[MAX_PATH];
    uint8_t volume_nid;
    uint8_t mute_nid;
    uint8_t mute_input;
    uint8_t mute_index;
    uint8_t detect;
    uint8_t present;
    uint8_t enabled;
    unsigned jack_control;
    char label[24];
};

struct input_path {
    uint8_t pin;
    uint8_t adc;
    uint8_t role;
    uint8_t length;
    struct step steps[MAX_PATH];
    uint8_t boost;
    uint8_t detect;
    uint8_t present;
    long boost_value;
    unsigned jack_control;
    char label[24];
};

struct control_info {
    uint8_t kind;
    uint8_t target;
};

struct hda_stream {
    uint32_t index;
    uint32_t tag;
    uint64_t *bdl;
    uint64_t bdl_physical;
    uint16_t bdl_entries;
    uint32_t buffer_bytes;
    uint16_t format;
    int configured;
};

struct hda_controller;

typedef uint32_t (*hda_transport_fn)(struct hda_controller *hda, uint32_t verb);

struct hda_controller {
    int present;
    hda_transport_fn transport;
    struct pci_device pci;
    uint64_t base;
    unsigned vector;
    struct lock codec_lock;

    uint32_t *corb;
    uint64_t corb_physical;
    uint64_t *rirb;
    uint64_t rirb_physical;
    uint16_t corb_wp;
    uint16_t rirb_rp;
    uint16_t corb_entries;
    uint32_t *position_buffer;
    uint64_t position_physical;

    uint8_t codec;
    uint8_t afg;
    uint32_t vendor_id;
    uint32_t subsystem_id;
    uint32_t revision_id;
    uint32_t afg_pcm;
    unsigned widget_count;
    struct widget widgets[MAX_NODES];

    struct output_path outputs[MAX_OUTPUTS];
    unsigned output_count;
    struct input_path inputs[MAX_INPUTS];
    unsigned input_count;
    uint8_t adc;
    uint8_t capture_volume_nid;
    uint8_t capture_volume_input;
    uint8_t capture_volume_index;

    long master[2];
    long master_on[2];
    uint32_t master_cap;
    long auto_mute;
    long capture[2];
    long capture_on[2];
    uint32_t capture_cap;
    long input_source;
    int auto_mic;
    int has_auto_mute;

    struct hda_stream streams[2];
    struct snd_pcm_ops ops[2];
    struct snd_control controls[MAX_CONTROLS];
    struct control_info control_info[MAX_CONTROLS];
    unsigned control_count;
    const char *source_names[MAX_INPUTS];
    char names[MAX_CONTROLS][44];
    char mixer_name[48];
    char longname[80];
    char components[64];
    struct snd_backend backend;
};

static struct hda_controller *controllers[MAX_CONTROLLERS];
static const char *const auto_mute_items[] = {"Disabled", "Enabled"};

static inline uint8_t read8(struct hda_controller *hda, uint32_t offset) {
    return *(volatile uint8_t *)(hda->base + offset);
}

static inline void write8(struct hda_controller *hda, uint32_t offset, uint8_t value) {
    *(volatile uint8_t *)(hda->base + offset) = value;
}

static inline uint16_t read16(struct hda_controller *hda, uint32_t offset) {
    return *(volatile uint16_t *)(hda->base + offset);
}

static inline void write16(struct hda_controller *hda, uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(hda->base + offset) = value;
}

static inline uint32_t read32(struct hda_controller *hda, uint32_t offset) {
    return *(volatile uint32_t *)(hda->base + offset);
}

static inline void write32(struct hda_controller *hda, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(hda->base + offset) = value;
}

static void delay_ns(uint64_t nanoseconds) {
    uint64_t deadline = time_uptime_ns() + nanoseconds;
    while (time_uptime_ns() < deadline) cpu_relax();
}

static void *dma_page(uint64_t *physical_out) {
    void *physical = pmm_alloc_page();
    if (!physical) return NULL;
    void *virtual_address = vmm_phys_to_virt((uint64_t)physical);
    if (!virtual_address) return NULL;
    memset(virtual_address, 0, (size_t)PMM_PAGE_SIZE);
    *physical_out = (uint64_t)physical;
    return virtual_address;
}

static int wait_bits16(struct hda_controller *hda, uint32_t offset, uint16_t mask, uint16_t want) {
    uint64_t deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while ((read16(hda, offset) & mask) != want) {
        if (time_uptime_ns() >= deadline) return -1;
        cpu_relax();
    }
    return 0;
}

static int reset_controller(struct hda_controller *hda) {
    write32(hda, HDA_GCTL, read32(hda, HDA_GCTL) & ~GCTL_RESET);
    uint64_t deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (read32(hda, HDA_GCTL) & GCTL_RESET) {
        if (time_uptime_ns() >= deadline) return -1;
        cpu_relax();
    }
    delay_ns(CODEC_SETTLE_NS);
    write32(hda, HDA_GCTL, read32(hda, HDA_GCTL) | GCTL_RESET);
    deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (!(read32(hda, HDA_GCTL) & GCTL_RESET)) {
        if (time_uptime_ns() >= deadline) return -1;
        cpu_relax();
    }
    delay_ns(CODEC_SETTLE_NS);
    return 0;
}

static uint8_t ring_size_code(uint8_t capability, uint16_t *entries) {
    if (capability & 0x40U) {
        *entries = 256;
        return 0x2U;
    }
    if (capability & 0x20U) {
        *entries = 16;
        return 0x1U;
    }
    *entries = 2;
    return 0x0U;
}

static int start_command_rings(struct hda_controller *hda) {
    write8(hda, HDA_CORBCTL, 0);
    write8(hda, HDA_RIRBCTL, 0);
    hda->corb = dma_page(&hda->corb_physical);
    hda->rirb = dma_page(&hda->rirb_physical);
    if (!hda->corb || !hda->rirb) return -1;

    uint16_t corb_entries = 0;
    uint16_t rirb_entries = 0;
    write8(hda, HDA_CORBSIZE, ring_size_code(read8(hda, HDA_CORBSIZE), &corb_entries));
    write32(hda, HDA_CORBLBASE, (uint32_t)hda->corb_physical);
    write32(hda, HDA_CORBUBASE, (uint32_t)(hda->corb_physical >> 32));
    write16(hda, HDA_CORBRP, CORBRP_RESET);
    (void)wait_bits16(hda, HDA_CORBRP, CORBRP_RESET, CORBRP_RESET);
    write16(hda, HDA_CORBRP, 0);
    if (wait_bits16(hda, HDA_CORBRP, CORBRP_RESET, 0) != 0) return -1;
    write16(hda, HDA_CORBWP, 0);
    hda->corb_wp = 0;
    hda->corb_entries = corb_entries;

    write8(hda, HDA_RIRBSIZE, ring_size_code(read8(hda, HDA_RIRBSIZE), &rirb_entries));
    write32(hda, HDA_RIRBLBASE, (uint32_t)hda->rirb_physical);
    write32(hda, HDA_RIRBUBASE, (uint32_t)(hda->rirb_physical >> 32));
    write16(hda, HDA_RIRBWP, RIRBWP_RESET);
    write16(hda, HDA_RINTCNT, 1);
    hda->rirb_rp = 0;

    write8(hda, HDA_CORBCTL, CORBCTL_RUN);
    write8(hda, HDA_RIRBCTL, RIRBCTL_RUN | RIRBCTL_INT);
    return 0;
}

static uint32_t ring_transport(struct hda_controller *hda, uint32_t value) {
    uint16_t mask = (uint16_t)(hda->corb_entries - 1U);
    hda->corb_wp = (uint16_t)((hda->corb_wp + 1U) & mask);
    hda->corb[hda->corb_wp] = value;
    cpu_memory_barrier();
    write16(hda, HDA_CORBWP, hda->corb_wp);

    uint64_t deadline = time_uptime_ns() + VERB_TIMEOUT_NS;
    for (;;) {
        uint16_t write_pointer = (uint16_t)(read16(hda, HDA_RIRBWP) & 0xFFU);
        while (write_pointer != hda->rirb_rp) {
            hda->rirb_rp = (uint16_t)((hda->rirb_rp + 1U) & 0xFFU);
            cpu_memory_barrier();
            uint64_t response = hda->rirb[hda->rirb_rp];
            if (response & (1ULL << 36)) continue;
            write8(hda, HDA_RIRBSTS, RIRBSTS_CLEAR);
            return (uint32_t)response;
        }
        if (time_uptime_ns() >= deadline) {
            kprintf("HDA: verb %x got no response\n", value);
            write8(hda, HDA_RIRBSTS, RIRBSTS_CLEAR);
            return 0xFFFFFFFFU;
        }
        cpu_relax();
    }
}

static uint32_t codec_command(struct hda_controller *hda, uint8_t nid, uint32_t verb,
                              uint32_t payload) {
    uint32_t value = ((uint32_t)hda->codec << 28) | ((uint32_t)nid << 20);
    if (verb > 0xFU) value |= (verb << 8) | (payload & 0xFFU);
    else value |= (verb << 16) | (payload & 0xFFFFU);
    lock_acquire(&hda->codec_lock);
    uint32_t response = hda->transport(hda, value);
    lock_release(&hda->codec_lock);
    return response;
}

static uint32_t parameter(struct hda_controller *hda, uint8_t nid, uint32_t which) {
    return codec_command(hda, nid, VERB_GET_PARAMETER, which);
}

static struct widget *widget_for(struct hda_controller *hda, uint8_t nid) {
    for (unsigned index = 0; index < hda->widget_count; index++)
        if (hda->widgets[index].nid == nid) return &hda->widgets[index];
    return NULL;
}

static void read_connections(struct hda_controller *hda, struct widget *widget) {
    widget->connection_count = 0;
    if (!(widget->caps & AWCAP_CONN_LIST)) return;
    uint32_t length_parameter = parameter(hda, widget->nid, PARAM_CONNLIST_LEN);
    unsigned count = length_parameter & 0x7FU;
    int long_form = (length_parameter & 0x80U) != 0;
    unsigned per_response = long_form ? 2U : 4U;
    unsigned stored = 0;
    uint32_t previous = 0;
    for (unsigned index = 0; index < count; index += per_response) {
        uint32_t response = codec_command(hda, widget->nid, VERB_GET_CONNECT_LIST, index);
        for (unsigned slot = 0; slot < per_response && index + slot < count; slot++) {
            uint32_t entry = long_form ? ((response >> (slot * 16U)) & 0xFFFFU)
                                       : ((response >> (slot * 8U)) & 0xFFU);
            uint32_t range_bit = long_form ? 0x8000U : 0x80U;
            uint32_t nid = entry & (range_bit - 1U);
            if ((entry & range_bit) && previous && nid > previous) {
                for (uint32_t fill = previous + 1U; fill <= nid && stored < MAX_CONNECTIONS; fill++)
                    widget->connections[stored++] = (uint8_t)fill;
            } else if (stored < MAX_CONNECTIONS) {
                widget->connections[stored++] = (uint8_t)nid;
            }
            previous = nid;
        }
    }
    widget->connection_count = (uint8_t)stored;
}

static int enumerate_widgets(struct hda_controller *hda) {
    uint32_t nodes = parameter(hda, hda->afg, PARAM_NODE_COUNT);
    unsigned start = (nodes >> 16) & 0xFFU;
    unsigned count = nodes & 0xFFU;
    if (!count || nodes == 0xFFFFFFFFU) return -1;
    uint32_t group_out_amp = parameter(hda, hda->afg, PARAM_OUT_AMP_CAP);
    uint32_t group_in_amp = parameter(hda, hda->afg, PARAM_IN_AMP_CAP);
    hda->afg_pcm = parameter(hda, hda->afg, PARAM_PCM);

    hda->widget_count = 0;
    for (unsigned index = 0; index < count && hda->widget_count < MAX_NODES; index++) {
        unsigned nid = start + index;
        if (nid >= MAX_NODES) break;
        struct widget *widget = &hda->widgets[hda->widget_count];
        memset(widget, 0, sizeof(*widget));
        widget->nid = (uint8_t)nid;
        widget->caps = parameter(hda, widget->nid, PARAM_AUDIO_WIDGET_CAP);
        if (widget->caps == 0xFFFFFFFFU) continue;
        widget->type = (uint8_t)((widget->caps >> 20) & 0xFU);
        if (widget->caps & AWCAP_AMP_OVERRIDE) {
            widget->out_amp_cap = parameter(hda, widget->nid, PARAM_OUT_AMP_CAP);
            widget->in_amp_cap = parameter(hda, widget->nid, PARAM_IN_AMP_CAP);
        } else {
            widget->out_amp_cap = group_out_amp;
            widget->in_amp_cap = group_in_amp;
        }
        if (widget->type == WIDGET_OUTPUT || widget->type == WIDGET_INPUT)
            widget->pcm_caps = (widget->caps & AWCAP_FORMAT_OVRD)
                ? parameter(hda, widget->nid, PARAM_PCM)
                : hda->afg_pcm;
        if (widget->type == WIDGET_PIN) {
            widget->pin_caps = parameter(hda, widget->nid, PARAM_PIN_CAP);
            widget->config = codec_command(hda, widget->nid, VERB_GET_CONFIG_DEFAULT, 0);
        }
        read_connections(hda, widget);
        if (widget->caps & AWCAP_POWER)
            (void)codec_command(hda, widget->nid, VERB_SET_POWER_STATE, 0);
        hda->widget_count++;
    }
    return hda->widget_count ? 0 : -1;
}

static uint32_t amp_offset(uint32_t cap) { return cap & 0x7FU; }
static uint32_t amp_steps(uint32_t cap) { return (cap >> 8) & 0x7FU; }
static uint32_t amp_step_cdb(uint32_t cap) { return (((cap >> 16) & 0x7FU) + 1U) * 25U; }
static int amp_mute_capable(uint32_t cap) { return (cap & 0x80000000U) != 0; }

static void set_amp(struct hda_controller *hda, uint8_t nid, int input, unsigned index, int left,
                    int right, uint32_t gain, int mute) {
    uint32_t payload = (input ? AMP_SET_INPUT : AMP_SET_OUTPUT) | (left ? AMP_SET_LEFT : 0U) |
        (right ? AMP_SET_RIGHT : 0U) | ((index & 0xFU) << AMP_INDEX_SHIFT) | (gain & 0x7FU) |
        (mute ? AMP_MUTE : 0U);
    (void)codec_command(hda, nid, VERB_SET_AMP_GAIN_MUTE, payload);
}

static int pin_digital(const struct widget *pin) {
    return (pin->caps & AWCAP_DIGITAL) || (pin->pin_caps & (PINCAP_HDMI | PINCAP_DP));
}

static uint32_t pin_port(const struct widget *pin) { return pin->config >> CONFIG_PORT_SHIFT; }
static uint32_t pin_device(const struct widget *pin) {
    return (pin->config >> CONFIG_DEVICE_SHIFT) & 0xFU;
}

static int pin_detectable(const struct widget *pin) {
    return (pin->pin_caps & PINCAP_PRESENCE) && !(pin->config & CONFIG_MISC_NO_DETECT) &&
        pin_port(pin) != CONFIG_PORT_FIXED;
}

static int find_route(struct hda_controller *hda, uint8_t from, uint8_t target, int target_type,
                      const uint8_t *avoid, unsigned avoid_count, struct step *steps,
                      uint8_t *length) {
    uint8_t queue[MAX_NODES];
    uint8_t parent[MAX_NODES];
    uint8_t parent_slot[MAX_NODES];
    uint8_t seen[MAX_NODES];
    memset(seen, 0, sizeof(seen));
    unsigned head = 0;
    unsigned tail = 0;
    queue[tail++] = from;
    seen[from] = 1;
    uint8_t found = 0;
    while (head < tail && !found) {
        struct widget *widget = widget_for(hda, queue[head++]);
        if (!widget) continue;
        for (unsigned slot = 0; slot < widget->connection_count && !found; slot++) {
            uint8_t next = widget->connections[slot];
            if (next >= MAX_NODES || seen[next]) continue;
            struct widget *candidate = widget_for(hda, next);
            if (!candidate) continue;
            int wanted = target ? next == target : (int)candidate->type == target_type;
            for (unsigned index = 0; wanted && index < avoid_count; index++)
                if (avoid[index] == next) wanted = 0;
            if (wanted) {
                seen[next] = 1;
                parent[next] = widget->nid;
                parent_slot[next] = (uint8_t)slot;
                found = next;
                break;
            }
            if (candidate->type != WIDGET_MIXER && candidate->type != WIDGET_SELECTOR) continue;
            seen[next] = 1;
            parent[next] = widget->nid;
            parent_slot[next] = (uint8_t)slot;
            if (tail < MAX_NODES) queue[tail++] = next;
        }
    }
    if (!found) return -1;
    uint8_t chain[MAX_PATH];
    uint8_t slots[MAX_PATH];
    unsigned count = 0;
    uint8_t nid = found;
    for (;;) {
        if (count >= MAX_PATH) return -1;
        chain[count] = nid;
        slots[count] = nid == from ? 0 : parent_slot[nid];
        count++;
        if (nid == from) break;
        nid = parent[nid];
    }
    for (unsigned index = 0; index < count; index++) {
        steps[index].nid = chain[index];
        steps[index].index = index ? slots[index - 1U] : 0;
    }
    *length = (uint8_t)count;
    return 0;
}

static void program_route(struct hda_controller *hda, const struct step *steps, uint8_t length) {
    for (unsigned index = 1; index < length; index++) {
        struct widget *widget = widget_for(hda, steps[index].nid);
        if (!widget) continue;
        uint8_t slot = steps[index].index;
        if (widget->type == WIDGET_MIXER) {
            if (widget->caps & AWCAP_IN_AMP)
                for (unsigned input = 0; input < widget->connection_count; input++)
                    set_amp(hda, widget->nid, 1, input, 1, 1, amp_offset(widget->in_amp_cap),
                            input != slot);
        } else {
            if (widget->connection_count > 1)
                (void)codec_command(hda, widget->nid, VERB_SET_CONNECT_SEL, slot);
            if ((widget->caps & AWCAP_IN_AMP) && widget->type != WIDGET_INPUT &&
                widget->type != WIDGET_PIN)
                set_amp(hda, widget->nid, 1, slot, 1, 1, amp_offset(widget->in_amp_cap), 0);
        }
        if ((widget->caps & AWCAP_OUT_AMP) &&
            (widget->type == WIDGET_MIXER || widget->type == WIDGET_SELECTOR))
            set_amp(hda, widget->nid, 0, 0, 1, 1, amp_offset(widget->out_amp_cap), 0);
    }
}

static const char *role_label(int role) {
    switch (role) {
    case ROLE_LINE_OUT:     return "Line Out";
    case ROLE_SPEAKER:      return "Speaker";
    case ROLE_HEADPHONE:    return "Headphone";
    case ROLE_MIC:          return "Mic";
    case ROLE_INTERNAL_MIC: return "Internal Mic";
    default:                return "Line";
    }
}

static const char *location_prefix(uint32_t config) {
    uint32_t location = (config >> CONFIG_LOCATION_SHIFT) & 0x3FU;
    if ((location & 0x30U) == 0x10U) return "Internal ";
    if ((location & 0x30U) == 0x20U) return "Dock ";
    switch (location & 0x0FU) {
    case 0x1: return "Rear ";
    case 0x2: return "Front ";
    case 0x3: return "Left ";
    case 0x4: return "Right ";
    case 0x5: return "Top ";
    case 0x6: return "Bottom ";
    default:  return "";
    }
}

static void make_label(char *out, size_t size, const char *prefix, const char *base) {
    size_t at = 0;
    for (size_t index = 0; prefix[index] && at + 1U < size; index++) out[at++] = prefix[index];
    for (size_t index = 0; base[index] && at + 1U < size; index++) out[at++] = base[index];
    out[at] = '\0';
}

static void label_paths(struct hda_controller *hda) {
    for (unsigned index = 0; index < hda->output_count; index++) {
        struct output_path *path = &hda->outputs[index];
        unsigned same = 0;
        for (unsigned other = 0; other < hda->output_count; other++)
            if (hda->outputs[other].role == path->role) same++;
        struct widget *pin = widget_for(hda, path->pin);
        const char *prefix = same > 1 && pin ? location_prefix(pin->config) : "";
        make_label(path->label, sizeof(path->label), prefix, role_label(path->role));
    }
    for (unsigned index = 0; index < hda->input_count; index++) {
        struct input_path *path = &hda->inputs[index];
        unsigned same = 0;
        for (unsigned other = 0; other < hda->input_count; other++)
            if (hda->inputs[other].role == path->role) same++;
        struct widget *pin = widget_for(hda, path->pin);
        const char *prefix =
            same > 1 && pin && path->role != ROLE_INTERNAL_MIC ? location_prefix(pin->config) : "";
        make_label(path->label, sizeof(path->label), prefix, role_label(path->role));
    }
}

static unsigned pin_order(const struct widget *pin) {
    unsigned association = (pin->config >> CONFIG_ASSOC_SHIFT) & 0xFU;
    if (!association) association = 16;
    return association * 16U + (pin->config & 0xFU);
}

static void choose_output_volume(struct hda_controller *hda, struct output_path *path) {
    path->volume_nid = 0;
    path->mute_nid = 0;
    for (unsigned index = 0; index < path->length; index++) {
        struct widget *widget = widget_for(hda, path->steps[index].nid);
        if (!widget || widget->type == WIDGET_PIN) continue;
        if ((widget->caps & AWCAP_OUT_AMP) && amp_steps(widget->out_amp_cap)) {
            path->volume_nid = widget->nid;
            break;
        }
    }
    struct widget *pin = widget_for(hda, path->pin);
    if (pin && (pin->caps & AWCAP_OUT_AMP) && amp_mute_capable(pin->out_amp_cap)) {
        path->mute_nid = pin->nid;
        return;
    }
    for (unsigned index = 1; index < path->length; index++) {
        struct widget *widget = widget_for(hda, path->steps[index].nid);
        if (widget && widget->type == WIDGET_MIXER && (widget->caps & AWCAP_IN_AMP) &&
            amp_mute_capable(widget->in_amp_cap)) {
            path->mute_nid = widget->nid;
            path->mute_input = 1;
            path->mute_index = path->steps[index].index;
            return;
        }
    }
}

static void collect_outputs(struct hda_controller *hda) {
    uint8_t order[MAX_NODES];
    unsigned candidates = 0;
    for (unsigned index = 0; index < hda->widget_count; index++) {
        struct widget *pin = &hda->widgets[index];
        if (pin->type != WIDGET_PIN || pin_digital(pin) || !(pin->pin_caps & PINCAP_OUTPUT))
            continue;
        if (pin_port(pin) == CONFIG_PORT_NONE) continue;
        uint32_t device = pin_device(pin);
        if (device != DEVICE_LINE_OUT && device != DEVICE_SPEAKER && device != DEVICE_HEADPHONE)
            continue;
        order[candidates++] = (uint8_t)index;
    }
    for (unsigned a = 1; a < candidates; a++)
        for (unsigned b = a;
             b > 0 && pin_order(&hda->widgets[order[b]]) < pin_order(&hda->widgets[order[b - 1U]]);
             b--) {
            uint8_t swap = order[b];
            order[b] = order[b - 1U];
            order[b - 1U] = swap;
        }
    uint8_t used[MAX_OUTPUTS];
    unsigned used_count = 0;
    hda->output_count = 0;
    for (unsigned at = 0; at < candidates && hda->output_count < MAX_OUTPUTS; at++) {
        struct widget *pin = &hda->widgets[order[at]];
        struct output_path *path = &hda->outputs[hda->output_count];
        memset(path, 0, sizeof(*path));
        path->pin = pin->nid;
        uint32_t device = pin_device(pin);
        path->role = device == DEVICE_SPEAKER    ? ROLE_SPEAKER
            : device == DEVICE_HEADPHONE         ? ROLE_HEADPHONE
            : pin_port(pin) == CONFIG_PORT_FIXED ? ROLE_SPEAKER
                                                 : ROLE_LINE_OUT;
        struct step steps[MAX_PATH];
        uint8_t length = 0;
        if (find_route(hda, pin->nid, 0, WIDGET_OUTPUT, used, used_count, steps, &length) != 0 &&
            find_route(hda, pin->nid, 0, WIDGET_OUTPUT, NULL, 0, steps, &length) != 0)
            continue;
        struct widget *dac = widget_for(hda, steps[0].nid);
        if (!dac || (dac->caps & AWCAP_DIGITAL)) continue;
        memcpy(path->steps, steps, sizeof(steps));
        path->length = length;
        path->dac = steps[0].nid;
        path->detect = (uint8_t)pin_detectable(pin);
        path->present = path->detect ? 0 : 1;
        path->enabled = 1;
        choose_output_volume(hda, path);
        int seen = 0;
        for (unsigned index = 0; index < used_count; index++)
            if (used[index] == path->dac) seen = 1;
        if (!seen && used_count < MAX_OUTPUTS) used[used_count++] = path->dac;
        hda->output_count++;
    }
}

static void collect_inputs(struct hda_controller *hda) {
    hda->input_count = 0;
    hda->adc = 0;
    for (unsigned a = 0; a < hda->widget_count && !hda->input_count; a++) {
        struct widget *adc = &hda->widgets[a];
        if (adc->type != WIDGET_INPUT || (adc->caps & AWCAP_DIGITAL) || !adc->connection_count)
            continue;
        for (unsigned index = 0; index < hda->widget_count && hda->input_count < MAX_INPUTS;
             index++) {
            struct widget *pin = &hda->widgets[index];
            if (pin->type != WIDGET_PIN || pin_digital(pin) || !(pin->pin_caps & PINCAP_INPUT))
                continue;
            if (pin_port(pin) == CONFIG_PORT_NONE) continue;
            uint32_t device = pin_device(pin);
            if (device != DEVICE_MIC && device != DEVICE_LINE_IN && device != DEVICE_AUX) continue;
            struct input_path *path = &hda->inputs[hda->input_count];
            memset(path, 0, sizeof(*path));
            if (find_route(hda, adc->nid, pin->nid, -1, NULL, 0, path->steps, &path->length) != 0)
                continue;
            path->pin = pin->nid;
            path->adc = adc->nid;
            path->role = device != DEVICE_MIC        ? ROLE_LINE_IN
                : pin_port(pin) == CONFIG_PORT_FIXED ? ROLE_INTERNAL_MIC
                                                     : ROLE_MIC;
            path->detect = (uint8_t)pin_detectable(pin);
            path->present = path->detect ? 0 : 1;
            if ((pin->caps & AWCAP_IN_AMP) && amp_steps(pin->in_amp_cap)) path->boost = pin->nid;
            hda->input_count++;
        }
        if (hda->input_count) hda->adc = adc->nid;
    }
    if (!hda->input_count) return;
    struct input_path *first = &hda->inputs[0];
    struct widget *adc = widget_for(hda, hda->adc);
    uint8_t last_index = first->steps[first->length - 1U].index;
    if (adc && (adc->caps & AWCAP_IN_AMP) && amp_steps(adc->in_amp_cap)) {
        hda->capture_volume_nid = adc->nid;
        hda->capture_volume_input = 1;
        hda->capture_volume_index = adc->connection_count > 1 ? last_index : 0;
        hda->capture_cap = adc->in_amp_cap;
        return;
    }
    for (unsigned index = first->length - 1U; index > 0; index--) {
        struct widget *widget = widget_for(hda, first->steps[index].nid);
        if (!widget) continue;
        if ((widget->caps & AWCAP_OUT_AMP) && amp_steps(widget->out_amp_cap)) {
            hda->capture_volume_nid = widget->nid;
            hda->capture_volume_input = 0;
            hda->capture_cap = widget->out_amp_cap;
            return;
        }
    }
    if (adc && (adc->caps & AWCAP_IN_AMP) && amp_mute_capable(adc->in_amp_cap)) {
        hda->capture_volume_nid = adc->nid;
        hda->capture_volume_input = 1;
        hda->capture_volume_index = adc->connection_count > 1 ? last_index : 0;
        hda->capture_cap = adc->in_amp_cap;
    }
}

static int headphone_present(struct hda_controller *hda) {
    for (unsigned index = 0; index < hda->output_count; index++)
        if (hda->outputs[index].role == ROLE_HEADPHONE && hda->outputs[index].detect &&
            hda->outputs[index].present)
            return 1;
    return 0;
}

static uint32_t scaled_gain(long value, uint32_t master_cap, uint32_t cap) {
    long db = (value - (long)amp_offset(master_cap)) * (long)amp_step_cdb(master_cap);
    long step = (long)amp_step_cdb(cap);
    long gain =
        (long)amp_offset(cap) + (db >= 0 ? (db + step / 2) / step : -((-db + step / 2) / step));
    if (gain < 0) gain = 0;
    if (gain > (long)amp_steps(cap)) gain = (long)amp_steps(cap);
    return (uint32_t)gain;
}

static void apply_outputs(struct hda_controller *hda) {
    int muted_by_hp = hda->has_auto_mute && hda->auto_mute && headphone_present(hda);
    for (unsigned index = 0; index < hda->output_count; index++) {
        struct output_path *path = &hda->outputs[index];
        struct widget *pin = widget_for(hda, path->pin);
        if (!pin) continue;
        int on = path->enabled && !(muted_by_hp && path->role != ROLE_HEADPHONE);
        int any_mute = 0;
        if (path->volume_nid) {
            struct widget *amp = widget_for(hda, path->volume_nid);
            uint32_t cap = amp->out_amp_cap;
            int can_mute = amp_mute_capable(cap);
            for (unsigned channel = 0; channel < 2U; channel++) {
                uint32_t gain = scaled_gain(hda->master[channel], hda->master_cap, cap);
                int mute = !hda->master_on[channel];
                if (mute && !can_mute) gain = 0;
                set_amp(hda, amp->nid, 0, 0, channel == 0, channel == 1, gain, mute);
            }
            any_mute = can_mute;
        }
        if (path->mute_nid) {
            struct widget *amp = widget_for(hda, path->mute_nid);
            uint32_t cap = path->mute_input ? amp->in_amp_cap : amp->out_amp_cap;
            for (unsigned channel = 0; channel < 2U; channel++)
                set_amp(hda, amp->nid, path->mute_input, path->mute_index, channel == 0,
                        channel == 1, amp_offset(cap), !hda->master_on[channel]);
            any_mute = 1;
        }
        if (!any_mute && !hda->master_on[0] && !hda->master_on[1]) on = 0;
        uint32_t control = 0;
        if (on) {
            control = PIN_OUT;
            if (path->role == ROLE_HEADPHONE && (pin->pin_caps & PINCAP_HP_DRIVE))
                control |= PIN_HP;
        }
        (void)codec_command(hda, pin->nid, VERB_SET_PIN_CONTROL, control);
        if (pin->pin_caps & PINCAP_EAPD)
            (void)codec_command(hda, pin->nid, VERB_SET_EAPD, EAPD_ENABLE);
    }
}

static unsigned mic_vref(const struct widget *pin) {
    if (pin->pin_caps & PINCAP_VREF_80) return PIN_VREF_80;
    if (pin->pin_caps & PINCAP_VREF_50) return PIN_VREF_50;
    if (pin->pin_caps & PINCAP_VREF_100) return PIN_VREF_100;
    return 0;
}

static void apply_inputs(struct hda_controller *hda) {
    if (!hda->input_count) return;
    if (hda->auto_mic) {
        long source = hda->input_source;
        for (unsigned index = 0; index < hda->input_count; index++) {
            if (hda->inputs[index].role == ROLE_MIC && hda->inputs[index].present) source = index;
            else if (hda->inputs[index].role == ROLE_INTERNAL_MIC && !headphone_present(hda) &&
                     source == (long)index)
                source = index;
        }
        int external = 0;
        for (unsigned index = 0; index < hda->input_count; index++)
            if (hda->inputs[index].role == ROLE_MIC && hda->inputs[index].present) {
                source = index;
                external = 1;
            }
        if (!external)
            for (unsigned index = 0; index < hda->input_count; index++)
                if (hda->inputs[index].role == ROLE_INTERNAL_MIC) source = index;
        hda->input_source = source;
    }
    for (unsigned index = 0; index < hda->input_count; index++) {
        struct input_path *path = &hda->inputs[index];
        struct widget *pin = widget_for(hda, path->pin);
        if (!pin) continue;
        uint32_t control = PIN_IN;
        if (path->role != ROLE_LINE_IN) control |= mic_vref(pin);
        (void)codec_command(hda, pin->nid, VERB_SET_PIN_CONTROL, control);
        if (path->boost) set_amp(hda, path->boost, 1, 0, 1, 1, (uint32_t)path->boost_value, 0);
    }
    struct input_path *selected = &hda->inputs[hda->input_source];
    program_route(hda, selected->steps, selected->length);
    if (hda->capture_volume_nid) {
        uint8_t index = hda->capture_volume_index;
        struct widget *adc = widget_for(hda, hda->adc);
        if (hda->capture_volume_input && adc && adc->connection_count > 1)
            index = selected->steps[selected->length - 1U].index;
        for (unsigned channel = 0; channel < 2U; channel++) {
            uint32_t gain = (uint32_t)hda->capture[channel];
            int mute = !hda->capture_on[channel];
            if (mute && !amp_mute_capable(hda->capture_cap)) gain = 0;
            set_amp(hda, hda->capture_volume_nid, hda->capture_volume_input, index, channel == 0,
                    channel == 1, gain, mute);
        }
    }
}

static int read_presence(struct hda_controller *hda, uint8_t nid) {
    struct widget *pin = widget_for(hda, nid);
    if (!pin) return 0;
    if (pin->pin_caps & PINCAP_TRIGGER) (void)codec_command(hda, nid, VERB_SET_PIN_SENSE, 0);
    uint32_t sense = codec_command(hda, nid, VERB_GET_PIN_SENSE, 0);
    if (sense == 0xFFFFFFFFU) return 0;
    return (sense & 0x80000000U) != 0;
}

static void add_control(struct hda_controller *hda, const char *name, unsigned name_index,
                        uint32_t iface, uint32_t type, uint32_t count, uint32_t access, int32_t min,
                        int32_t max, uint32_t kind, uint32_t target) {
    if (hda->control_count >= MAX_CONTROLS) return;
    unsigned index = hda->control_count++;
    char *label = hda->names[index];
    size_t at = 0;
    for (; name[at] && at + 1U < sizeof(hda->names[index]); at++) label[at] = name[at];
    label[at] = '\0';
    struct snd_control *control = &hda->controls[index];
    memset(control, 0, sizeof(*control));
    control->name = label;
    control->iface = iface;
    control->index = name_index;
    control->type = type;
    control->count = count;
    control->access = access;
    control->min = min;
    control->max = max;
    control->step = 1;
    hda->control_info[index].kind = (uint8_t)kind;
    hda->control_info[index].target = (uint8_t)target;
}

static void set_db(struct hda_controller *hda, uint32_t cap) {
    struct snd_control *control = &hda->controls[hda->control_count - 1U];
    control->db_min = -(int32_t)(amp_offset(cap) * amp_step_cdb(cap));
    control->db_step = (int32_t)amp_step_cdb(cap);
    control->db_mute = 0;
}

static void compose(char *out, size_t size, const char *first, const char *second) {
    size_t at = 0;
    for (size_t index = 0; first[index] && at + 1U < size; index++) out[at++] = first[index];
    for (size_t index = 0; second[index] && at + 1U < size; index++) out[at++] = second[index];
    out[at] = '\0';
}

static unsigned label_index(struct hda_controller *hda, const char *label, int output,
                            unsigned upto) {
    unsigned same = 0;
    for (unsigned index = 0; index < upto; index++) {
        const char *other = output ? hda->outputs[index].label : hda->inputs[index].label;
        if (!strcmp(other, label)) same++;
    }
    return same;
}

static void build_controls(struct hda_controller *hda) {
    char name[44];
    hda->control_count = 0;
    uint32_t rw = SND_CONTROL_READ | SND_CONTROL_WRITE;
    if (hda->master_cap) {
        add_control(hda, "Master Playback Volume", 0, SND_IFACE_MIXER, SND_CONTROL_INTEGER, 2, rw,
                    0, (int32_t)amp_steps(hda->master_cap), CTL_MASTER_VOLUME, 0);
        set_db(hda, hda->master_cap);
    }
    add_control(hda, "Master Playback Switch", 0, SND_IFACE_MIXER, SND_CONTROL_BOOLEAN, 2, rw, 0, 1,
                CTL_MASTER_SWITCH, 0);
    for (unsigned index = 0; index < hda->output_count; index++) {
        const char *label = hda->outputs[index].label;
        compose(name, sizeof(name), label, " Playback Switch");
        add_control(hda, name, label_index(hda, label, 1, index), SND_IFACE_MIXER,
                    SND_CONTROL_BOOLEAN, 2, rw, 0, 1, CTL_OUTPUT_SWITCH, index);
    }
    if (hda->has_auto_mute) {
        add_control(hda, "Auto-Mute Mode", 0, SND_IFACE_MIXER, SND_CONTROL_ENUMERATED, 1, rw, 0, 1,
                    CTL_AUTO_MUTE, 0);
        hda->controls[hda->control_count - 1U].items = auto_mute_items;
        hda->controls[hda->control_count - 1U].item_count = 2;
    }
    if (hda->input_count && hda->capture_volume_nid) {
        if (amp_steps(hda->capture_cap)) {
            add_control(hda, "Capture Volume", 0, SND_IFACE_MIXER, SND_CONTROL_INTEGER, 2, rw, 0,
                        (int32_t)amp_steps(hda->capture_cap), CTL_CAPTURE_VOLUME, 0);
            set_db(hda, hda->capture_cap);
        }
        add_control(hda, "Capture Switch", 0, SND_IFACE_MIXER, SND_CONTROL_BOOLEAN, 2, rw, 0, 1,
                    CTL_CAPTURE_SWITCH, 0);
    }
    if (hda->input_count > 1 && !hda->auto_mic) {
        for (unsigned index = 0; index < hda->input_count; index++)
            hda->source_names[index] = hda->inputs[index].label;
        add_control(hda, "Input Source", 0, SND_IFACE_MIXER, SND_CONTROL_ENUMERATED, 1, rw, 0,
                    (int32_t)hda->input_count - 1, CTL_INPUT_SOURCE, 0);
        hda->controls[hda->control_count - 1U].items = hda->source_names;
        hda->controls[hda->control_count - 1U].item_count = hda->input_count;
    }
    for (unsigned index = 0; index < hda->input_count; index++) {
        if (!hda->inputs[index].boost) continue;
        struct widget *pin = widget_for(hda, hda->inputs[index].boost);
        const char *label = hda->inputs[index].label;
        compose(name, sizeof(name), label, " Boost Volume");
        add_control(hda, name, label_index(hda, label, 0, index), SND_IFACE_MIXER,
                    SND_CONTROL_INTEGER, 2, rw, 0, (int32_t)amp_steps(pin->in_amp_cap), CTL_BOOST,
                    index);
        set_db(hda, pin->in_amp_cap);
        hda->controls[hda->control_count - 1U].db_min = 0;
    }
    uint32_t jack_access = SND_CONTROL_READ | SND_CONTROL_VOLATILE;
    for (unsigned index = 0; index < hda->output_count; index++) {
        struct output_path *path = &hda->outputs[index];
        const char *label = path->label;
        compose(name, sizeof(name), label, path->detect ? " Jack" : " Phantom Jack");
        path->jack_control = hda->control_count;
        add_control(hda, name, label_index(hda, label, 1, index), SND_IFACE_CARD,
                    SND_CONTROL_BOOLEAN, 1, jack_access, 0, 1, CTL_JACK, index);
    }
    for (unsigned index = 0; index < hda->input_count; index++) {
        struct input_path *path = &hda->inputs[index];
        const char *label = path->label;
        compose(name, sizeof(name), label, path->detect ? " Jack" : " Phantom Jack");
        path->jack_control = hda->control_count;
        add_control(hda, name, label_index(hda, label, 0, index), SND_IFACE_CARD,
                    SND_CONTROL_BOOLEAN, 1, jack_access, 0, 1, CTL_JACK, 0x80U | index);
    }
}

static int hda_control_get(void *context, unsigned control, long *values) {
    struct hda_controller *hda = context;
    if (control >= hda->control_count) return -1;
    unsigned target = hda->control_info[control].target;
    switch (hda->control_info[control].kind) {
    case CTL_MASTER_VOLUME:
        values[0] = hda->master[0];
        values[1] = hda->master[1];
        break;
    case CTL_MASTER_SWITCH:
        values[0] = hda->master_on[0];
        values[1] = hda->master_on[1];
        break;
    case CTL_OUTPUT_SWITCH: values[0] = values[1] = hda->outputs[target].enabled; break;
    case CTL_AUTO_MUTE:     values[0] = hda->auto_mute; break;
    case CTL_CAPTURE_VOLUME:
        values[0] = hda->capture[0];
        values[1] = hda->capture[1];
        break;
    case CTL_CAPTURE_SWITCH:
        values[0] = hda->capture_on[0];
        values[1] = hda->capture_on[1];
        break;
    case CTL_INPUT_SOURCE: values[0] = hda->input_source; break;
    case CTL_BOOST:        values[0] = values[1] = hda->inputs[target].boost_value; break;
    case CTL_JACK:
        values[0] =
            target & 0x80U ? hda->inputs[target & 0x7FU].present : hda->outputs[target].present;
        break;
    default: return -1;
    }
    return 0;
}

static int store_pair(long *pair, const long *values) {
    int changed = pair[0] != values[0] || pair[1] != values[1];
    pair[0] = values[0];
    pair[1] = values[1];
    return changed;
}

static int hda_control_put(void *context, unsigned control, const long *values) {
    struct hda_controller *hda = context;
    if (control >= hda->control_count) return -1;
    unsigned target = hda->control_info[control].target;
    int changed = 0;
    switch (hda->control_info[control].kind) {
    case CTL_MASTER_VOLUME:
        changed = store_pair(hda->master, values);
        if (changed) apply_outputs(hda);
        break;
    case CTL_MASTER_SWITCH:
        changed = store_pair(hda->master_on, values);
        if (changed) apply_outputs(hda);
        break;
    case CTL_OUTPUT_SWITCH: {
        uint8_t on = (values[0] || values[1]) ? 1U : 0U;
        changed = hda->outputs[target].enabled != on;
        hda->outputs[target].enabled = on;
        if (changed) apply_outputs(hda);
        break;
    }
    case CTL_AUTO_MUTE:
        changed = hda->auto_mute != values[0];
        hda->auto_mute = values[0];
        if (changed) apply_outputs(hda);
        break;
    case CTL_CAPTURE_VOLUME:
        changed = store_pair(hda->capture, values);
        if (changed) apply_inputs(hda);
        break;
    case CTL_CAPTURE_SWITCH:
        changed = store_pair(hda->capture_on, values);
        if (changed) apply_inputs(hda);
        break;
    case CTL_INPUT_SOURCE:
        if (values[0] < 0 || values[0] >= (long)hda->input_count) return -22;
        changed = hda->input_source != values[0];
        hda->input_source = values[0];
        if (changed) apply_inputs(hda);
        break;
    case CTL_BOOST: {
        long value = values[0] > values[1] ? values[0] : values[1];
        changed = hda->inputs[target].boost_value != value;
        hda->inputs[target].boost_value = value;
        if (changed) apply_inputs(hda);
        break;
    }
    default: return -1;
    }
    return changed;
}

static void hda_poll(void *context) {
    struct hda_controller *hda = context;
    if (!hda->present) return;
    unsigned changed_controls[MAX_OUTPUTS + MAX_INPUTS];
    unsigned changes = 0;
    for (unsigned index = 0; index < hda->output_count; index++) {
        struct output_path *path = &hda->outputs[index];
        if (!path->detect) continue;
        uint8_t present = (uint8_t)read_presence(hda, path->pin);
        if (present == path->present) continue;
        path->present = present;
        changed_controls[changes++] = path->jack_control;
    }
    int outputs_changed = changes != 0;
    for (unsigned index = 0; index < hda->input_count; index++) {
        struct input_path *path = &hda->inputs[index];
        if (!path->detect) continue;
        uint8_t present = (uint8_t)read_presence(hda, path->pin);
        if (present == path->present) continue;
        path->present = present;
        changed_controls[changes++] = path->jack_control;
    }
    if (!changes) return;
    if (outputs_changed) apply_outputs(hda);
    if (hda->auto_mic) apply_inputs(hda);
    for (unsigned index = 0; index < changes; index++)
        snd_ctl_notify(&hda->backend, changed_controls[index], SND_EVENT_VALUE);
}

struct rate_encoding {
    uint32_t rate;
    uint16_t bits;
};

static const struct rate_encoding rate_table[] = {
    {8000, 0x0500},  {11025, 0x4300},  {16000, 0x0200}, {22050, 0x4100},
    {32000, 0x0A00}, {44100, 0x4000},  {48000, 0x0000}, {88200, 0x4800},
    {96000, 0x0800}, {176400, 0x5800}, {192000, 0x1800}};

static const uint32_t pcm_cap_rates[] = {8000,  11025, 16000, 22050,  32000, 44100,
                                         48000, 88200, 96000, 176400, 192000};

static int encode_format(const struct snd_stream_format *format, uint16_t *out) {
    unsigned index = 0;
    for (; index < sizeof(rate_table) / sizeof(rate_table[0]); index++)
        if (rate_table[index].rate == format->rate) break;
    if (index == sizeof(rate_table) / sizeof(rate_table[0])) return -1;
    uint16_t value = rate_table[index].bits;
    if (format->format == SND_FORMAT_S16_LE) value |= 1U << 4;
    else if (format->format == SND_FORMAT_S32_LE)
        value |= (format->msbits >= 32U ? 4U : format->msbits >= 24U ? 3U : 2U) << 4;
    else return -1;
    if (!format->channels || format->channels > 16U) return -1;
    value |= (uint16_t)((format->channels - 1U) & 0xFU);
    *out = value;
    return 0;
}

static uint32_t stream_register(const struct hda_stream *stream, uint32_t offset) {
    return STREAM_BASE + stream->index * STREAM_STRIDE + offset;
}

static void stop_stream(struct hda_controller *hda, struct hda_stream *stream) {
    uint32_t control = read32(hda, stream_register(stream, SD_CTL));
    write32(hda, stream_register(stream, SD_CTL), control & ~(SDCTL_RUN | SDCTL_IOCE));
    uint64_t deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (read32(hda, stream_register(stream, SD_CTL)) & SDCTL_RUN) {
        if (time_uptime_ns() >= deadline) break;
        cpu_relax();
    }
    write8(hda, stream_register(stream, SD_STS), SDSTS_CLEAR);
}

static void reset_stream(struct hda_controller *hda, struct hda_stream *stream) {
    stop_stream(hda, stream);
    write32(hda, stream_register(stream, SD_CTL), SDCTL_RESET);
    uint64_t deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (!(read32(hda, stream_register(stream, SD_CTL)) & SDCTL_RESET)) {
        if (time_uptime_ns() >= deadline) break;
        cpu_relax();
    }
    write32(hda, stream_register(stream, SD_CTL), 0);
    deadline = time_uptime_ns() + RESET_TIMEOUT_NS;
    while (read32(hda, stream_register(stream, SD_CTL)) & SDCTL_RESET) {
        if (time_uptime_ns() >= deadline) break;
        cpu_relax();
    }
}

static void set_converters(struct hda_controller *hda, int direction, uint16_t format,
                           uint32_t tag) {
    if (direction == SND_CAPTURE) {
        (void)codec_command(hda, hda->adc, VERB_SET_CONVERTER_FORMAT, format);
        (void)codec_command(hda, hda->adc, VERB_SET_STREAM_CHANNEL, tag << 4);
        return;
    }
    for (unsigned index = 0; index < hda->output_count; index++) {
        uint8_t dac = hda->outputs[index].dac;
        int earlier = 0;
        for (unsigned before = 0; before < index; before++)
            if (hda->outputs[before].dac == dac) earlier = 1;
        if (earlier) continue;
        (void)codec_command(hda, dac, VERB_SET_CONVERTER_FORMAT, format);
        (void)codec_command(hda, dac, VERB_SET_STREAM_CHANNEL, tag << 4);
    }
}

static int program_stream(struct hda_controller *hda, int direction) {
    struct hda_stream *stream = &hda->streams[direction];
    if (!stream->configured) return -1;
    reset_stream(hda, stream);
    write32(hda, stream_register(stream, SD_CBL), stream->buffer_bytes);
    write16(hda, stream_register(stream, SD_LVI), (uint16_t)(stream->bdl_entries - 1U));
    write16(hda, stream_register(stream, SD_FMT), stream->format);
    write32(hda, stream_register(stream, SD_BDPL), (uint32_t)stream->bdl_physical);
    write32(hda, stream_register(stream, SD_BDPU), (uint32_t)(stream->bdl_physical >> 32));
    write32(hda, stream_register(stream, SD_CTL), stream->tag << SDCTL_STREAM_SHIFT);
    hda->position_buffer[stream->index * 2U] = 0;
    set_converters(hda, direction, stream->format, stream->tag);
    return 0;
}

static int stream_configure(struct hda_controller *hda, int direction,
                            const struct snd_stream_format *format, const uint64_t *pages,
                            unsigned page_count, uint32_t buffer_bytes, uint32_t period_bytes) {
    struct hda_stream *stream = &hda->streams[direction];
    if (!hda->present || !pages || !page_count || !buffer_bytes || !period_bytes) return -1;
    if (buffer_bytes > page_count * (uint32_t)PMM_PAGE_SIZE) return -1;
    uint16_t encoded = 0;
    if (encode_format(format, &encoded) != 0) return -1;
    unsigned entries = 0;
    uint32_t offset = 0;
    while (offset < buffer_bytes) {
        if (entries >= MAX_BDL_ENTRIES) return -1;
        uint32_t within = offset % (uint32_t)PMM_PAGE_SIZE;
        uint32_t length = (uint32_t)PMM_PAGE_SIZE - within;
        uint32_t period_end = (offset / period_bytes + 1U) * period_bytes;
        if (length > buffer_bytes - offset) length = buffer_bytes - offset;
        if (length > period_end - offset) length = period_end - offset;
        int ioc = (offset + length) % period_bytes == 0 || offset + length == buffer_bytes;
        stream->bdl[entries * 2U] = pages[offset / (uint32_t)PMM_PAGE_SIZE] + within;
        stream->bdl[entries * 2U + 1U] = (uint64_t)length | ((uint64_t)(ioc ? 1U : 0U) << 32);
        entries++;
        offset += length;
    }
    if (entries < 2U) return -1;
    stop_stream(hda, stream);
    stream->buffer_bytes = buffer_bytes;
    stream->format = encoded;
    stream->bdl_entries = (uint16_t)entries;
    stream->configured = 1;
    return program_stream(hda, direction);
}

static int stream_trigger(struct hda_controller *hda, int direction, int command) {
    struct hda_stream *stream = &hda->streams[direction];
    if (!hda->present || !stream->configured) return -1;
    if (command == SND_TRIGGER_STOP || command == SND_TRIGGER_PAUSE) {
        stop_stream(hda, stream);
        return 0;
    }
    write8(hda, stream_register(stream, SD_STS), SDSTS_CLEAR);
    uint32_t control = read32(hda, stream_register(stream, SD_CTL)) | SDCTL_RUN;
    if (hda->vector) control |= SDCTL_IOCE;
    write32(hda, stream_register(stream, SD_CTL), control);
    return 0;
}

static uint32_t stream_position(struct hda_controller *hda, int direction) {
    struct hda_stream *stream = &hda->streams[direction];
    if (!hda->present || !stream->buffer_bytes) return 0;
    uint32_t position = hda->position_buffer[stream->index * 2U];
    if (!position) position = read32(hda, stream_register(stream, SD_LPIB));
    return position % stream->buffer_bytes;
}

static void stream_release(struct hda_controller *hda, int direction) {
    struct hda_stream *stream = &hda->streams[direction];
    if (!hda->present) return;
    stop_stream(hda, stream);
    stream->configured = 0;
}

static int playback_configure(void *context, const struct snd_stream_format *format,
                              const uint64_t *pages, unsigned count, uint32_t buffer_bytes,
                              uint32_t period_bytes) {
    return stream_configure(context, SND_PLAYBACK, format, pages, count, buffer_bytes,
                            period_bytes);
}

static int capture_configure(void *context, const struct snd_stream_format *format,
                             const uint64_t *pages, unsigned count, uint32_t buffer_bytes,
                             uint32_t period_bytes) {
    return stream_configure(context, SND_CAPTURE, format, pages, count, buffer_bytes, period_bytes);
}

static int playback_prepare(void *context) { return program_stream(context, SND_PLAYBACK); }
static int capture_prepare(void *context) { return program_stream(context, SND_CAPTURE); }

static int playback_trigger(void *context, int command) {
    return stream_trigger(context, SND_PLAYBACK, command);
}

static int capture_trigger(void *context, int command) {
    return stream_trigger(context, SND_CAPTURE, command);
}

static uint32_t playback_position(void *context) { return stream_position(context, SND_PLAYBACK); }
static uint32_t capture_position(void *context) { return stream_position(context, SND_CAPTURE); }
static void playback_release(void *context) { stream_release(context, SND_PLAYBACK); }
static void capture_release(void *context) { stream_release(context, SND_CAPTURE); }

static void fill_hardware(struct snd_hardware *hw, uint32_t caps) {
    memset(hw, 0, sizeof(*hw));
    for (unsigned bit = 0; bit < sizeof(pcm_cap_rates) / sizeof(pcm_cap_rates[0]); bit++)
        if (caps & (1U << bit)) hw->rates[hw->rate_count++] = pcm_cap_rates[bit];
    if (!hw->rate_count) {
        hw->rates[0] = 44100;
        hw->rates[1] = 48000;
        hw->rate_count = 2;
    }
    if (caps & (1U << 17) || !(caps & (7U << 18))) hw->formats |= 1U << SND_FORMAT_S16_LE;
    if (caps & (7U << 18)) {
        hw->formats |= 1U << SND_FORMAT_S32_LE;
        hw->msbits_s32 = caps & (1U << 20) ? 32U : caps & (1U << 19) ? 24U : 20U;
    }
    hw->channels_min = 2;
    hw->channels_max = 2;
    hw->period_bytes_min = 128;
    hw->period_bytes_max = 256U * 1024U;
    hw->periods_min = 2;
    hw->periods_max = 32;
    hw->buffer_bytes_max = 512U * 1024U;
}

static void hda_interrupt(void *context) {
    struct hda_controller *hda = context;
    if (!hda->present) return;
    uint32_t status = read32(hda, HDA_INTSTS);
    if (!status || status == 0xFFFFFFFFU) return;
    for (int direction = 0; direction < 2; direction++) {
        struct hda_stream *stream = &hda->streams[direction];
        if (!stream->bdl || !(status & (1U << stream->index))) continue;
        uint8_t stream_status = read8(hda, stream_register(stream, SD_STS));
        write8(hda, stream_register(stream, SD_STS), stream_status & SDSTS_CLEAR);
        if (stream_status & SDSTS_BCIS) snd_pcm_period_elapsed(&hda->backend, direction);
    }
}

static const char *vendor_name(uint32_t vendor) {
    switch (vendor >> 16) {
    case 0x10EC: return "Realtek";
    case 0x1106: return "VIA";
    case 0x111D: return "IDT";
    case 0x14F1: return "Conexant";
    case 0x8384: return "SigmaTel";
    case 0x10DE: return "Nvidia";
    case 0x8086: return "Intel";
    case 0x1002: return "ATI";
    case 0x1022: return "AMD";
    case 0x11D4: return "Analog Devices";
    case 0x1013: return "Cirrus Logic";
    case 0x1AF4: return "QEMU";
    case 0x434D: return "C-Media";
    case 0x17E8: return "Chrontel";
    default:     return "Generic";
    }
}

static void put_hex(char *out, size_t *at, size_t size, uint32_t value, unsigned digits) {
    static const char alphabet[] = "0123456789abcdef";
    for (unsigned index = digits; index-- > 0;)
        if (*at + 1U < size) out[(*at)++] = alphabet[(value >> (index * 4U)) & 0xFU];
}

static void put_text(char *out, size_t *at, size_t size, const char *text) {
    for (size_t index = 0; text[index]; index++)
        if (*at + 1U < size) out[(*at)++] = text[index];
}

static void describe_codec(struct hda_controller *hda) {
    size_t at = 0;
    const char *vendor = vendor_name(hda->vendor_id);
    put_text(hda->mixer_name, &at, sizeof(hda->mixer_name), vendor);
    if ((hda->vendor_id >> 16) == 0x10EC) {
        put_text(hda->mixer_name, &at, sizeof(hda->mixer_name), " ALC");
        put_hex(hda->mixer_name, &at, sizeof(hda->mixer_name), hda->vendor_id & 0xFFFFU,
                (hda->vendor_id & 0xF000U) ? 4U : 3U);
    } else {
        put_text(hda->mixer_name, &at, sizeof(hda->mixer_name), " ID ");
        put_hex(hda->mixer_name, &at, sizeof(hda->mixer_name), hda->vendor_id & 0xFFFFU, 4);
    }
    hda->mixer_name[at] = '\0';
    at = 0;
    put_text(hda->components, &at, sizeof(hda->components), "HDA:");
    put_hex(hda->components, &at, sizeof(hda->components), hda->vendor_id, 8);
    put_text(hda->components, &at, sizeof(hda->components), ",");
    put_hex(hda->components, &at, sizeof(hda->components), hda->subsystem_id, 8);
    put_text(hda->components, &at, sizeof(hda->components), ",");
    put_hex(hda->components, &at, sizeof(hda->components), hda->revision_id, 8);
    hda->components[at] = '\0';
}

static int probe_codec(struct hda_controller *hda, uint8_t address) {
    hda->codec = address;
    uint32_t nodes = parameter(hda, 0, PARAM_NODE_COUNT);
    if (nodes == 0xFFFFFFFFU) return -1;
    unsigned start = (nodes >> 16) & 0xFFU;
    unsigned count = nodes & 0xFFU;
    for (unsigned index = 0; index < count; index++) {
        uint8_t nid = (uint8_t)(start + index);
        uint32_t type = parameter(hda, nid, PARAM_FUNCTION_TYPE);
        if ((type & 0xFFU) != FUNCTION_TYPE_AUDIO) continue;
        hda->afg = nid;
        (void)codec_command(hda, nid, VERB_SET_POWER_STATE, 0);
        delay_ns(10ULL * CODEC_SETTLE_NS);
        if (enumerate_widgets(hda) != 0) continue;
        collect_outputs(hda);
        if (!hda->output_count) continue;
        collect_inputs(hda);
        label_paths(hda);
        hda->vendor_id = parameter(hda, 0, PARAM_VENDOR_ID);
        hda->revision_id = parameter(hda, 0, PARAM_REVISION_ID);
        hda->subsystem_id = codec_command(hda, nid, VERB_GET_SUBSYSTEM_ID, 0);
        return 0;
    }
    return -1;
}

static void setup_mixer(struct hda_controller *hda) {
    hda->master_cap = 0;
    for (unsigned index = 0; index < hda->output_count && !hda->master_cap; index++) {
        struct output_path *path = &hda->outputs[index];
        if (!path->volume_nid) continue;
        hda->master_cap = widget_for(hda, path->volume_nid)->out_amp_cap;
    }
    long start = hda->master_cap ? (long)amp_offset(hda->master_cap) : 0;
    if (hda->master_cap && start > (long)amp_steps(hda->master_cap))
        start = (long)amp_steps(hda->master_cap);
    hda->master[0] = hda->master[1] = start;
    hda->master_on[0] = hda->master_on[1] = 1;

    int hp_detect = 0;
    int others = 0;
    int mics = 0;
    int internal_mics = 0;
    for (unsigned index = 0; index < hda->output_count; index++) {
        if (hda->outputs[index].role == ROLE_HEADPHONE && hda->outputs[index].detect) hp_detect = 1;
        else if (hda->outputs[index].role != ROLE_HEADPHONE) others = 1;
    }
    for (unsigned index = 0; index < hda->input_count; index++) {
        if (hda->inputs[index].role == ROLE_MIC && hda->inputs[index].detect) mics++;
        if (hda->inputs[index].role == ROLE_INTERNAL_MIC) internal_mics++;
    }
    hda->has_auto_mute = hp_detect && others;
    hda->auto_mute = 1;
    hda->auto_mic = mics == 1 && internal_mics == 1;

    long capture = hda->capture_cap ? (long)amp_offset(hda->capture_cap) : 0;
    if (hda->capture_cap && capture > (long)amp_steps(hda->capture_cap))
        capture = (long)amp_steps(hda->capture_cap);
    hda->capture[0] = hda->capture[1] = capture;
    hda->capture_on[0] = hda->capture_on[1] = 1;
    hda->input_source = 0;

    for (unsigned index = 0; index < hda->widget_count; index++) {
        struct widget *pin = &hda->widgets[index];
        if (pin->type != WIDGET_PIN || pin_digital(pin)) continue;
        int used = 0;
        for (unsigned out = 0; out < hda->output_count; out++)
            if (hda->outputs[out].pin == pin->nid) used = 1;
        for (unsigned in = 0; in < hda->input_count; in++)
            if (hda->inputs[in].pin == pin->nid) used = 1;
        if (!used) (void)codec_command(hda, pin->nid, VERB_SET_PIN_CONTROL, 0);
    }
    for (unsigned index = 0; index < hda->output_count; index++) {
        struct output_path *path = &hda->outputs[index];
        program_route(hda, path->steps, path->length);
        if (path->detect) path->present = (uint8_t)read_presence(hda, path->pin);
    }
    for (unsigned index = 0; index < hda->input_count; index++) {
        struct input_path *path = &hda->inputs[index];
        if (path->detect) path->present = (uint8_t)read_presence(hda, path->pin);
    }
    apply_outputs(hda);
    apply_inputs(hda);
}

struct dump_text {
    char *out;
    size_t capacity;
    size_t length;
};

static void dump_add(struct dump_text *text, const char *value) {
    for (size_t index = 0; value[index]; index++) {
        if (text->length + 1U < text->capacity) text->out[text->length] = value[index];
        text->length++;
    }
}

static void dump_hex(struct dump_text *text, uint32_t value, unsigned digits) {
    char buffer[12];
    size_t at = 0;
    put_text(buffer, &at, sizeof(buffer), "0x");
    put_hex(buffer, &at, sizeof(buffer), value, digits);
    buffer[at] = '\0';
    dump_add(text, buffer);
}

static void dump_number(struct dump_text *text, uint32_t value) {
    char digits[12];
    unsigned at = sizeof(digits) - 1U;
    digits[at] = '\0';
    do {
        digits[--at] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && at);
    dump_add(text, &digits[at]);
}

static const char *widget_type_name(uint8_t type) {
    switch (type) {
    case WIDGET_OUTPUT:   return "Audio Output";
    case WIDGET_INPUT:    return "Audio Input";
    case WIDGET_MIXER:    return "Audio Mixer";
    case WIDGET_SELECTOR: return "Audio Selector";
    case WIDGET_PIN:      return "Pin Complex";
    case 0x5:             return "Power Widget";
    case 0x6:             return "Volume Knob Widget";
    case 0x7:             return "Beep Generator Widget";
    default:              return "Vendor Defined Widget";
    }
}

static void dump_amp(struct dump_text *text, const char *label, uint32_t cap) {
    dump_add(text, "  ");
    dump_add(text, label);
    dump_add(text, " caps: ofs=");
    dump_hex(text, amp_offset(cap), 2);
    dump_add(text, ", nsteps=");
    dump_hex(text, amp_steps(cap), 2);
    dump_add(text, ", stepsize=");
    dump_hex(text, (cap >> 16) & 0x7FU, 2);
    dump_add(text, ", mute=");
    dump_number(text, amp_mute_capable(cap) ? 1U : 0U);
    dump_add(text, "\n");
}

static void dump_path(struct dump_text *text, const char *label, const struct step *steps,
                      uint8_t length) {
    dump_add(text, "  ");
    dump_add(text, label);
    dump_add(text, ":");
    for (unsigned index = 0; index < length; index++) {
        dump_add(text, index ? " -> " : " ");
        dump_hex(text, steps[index].nid, 2);
        if (index) {
            dump_add(text, "[");
            dump_number(text, steps[index].index);
            dump_add(text, "]");
        }
    }
    dump_add(text, "\n");
}

static size_t hda_proc_codec(void *context, char *out, size_t capacity) {
    struct hda_controller *hda = context;
    struct dump_text text = {out, capacity, 0};
    if (!hda->present) return 0;
    dump_add(&text, "Codec: ");
    dump_add(&text, hda->mixer_name);
    dump_add(&text, "\nAddress: ");
    dump_number(&text, hda->codec);
    dump_add(&text, "\nVendor Id: ");
    dump_hex(&text, hda->vendor_id, 8);
    dump_add(&text, "\nSubsystem Id: ");
    dump_hex(&text, hda->subsystem_id, 8);
    dump_add(&text, "\nRevision Id: ");
    dump_hex(&text, hda->revision_id, 6);
    dump_add(&text, "\nAFG: ");
    dump_hex(&text, hda->afg, 2);
    dump_add(&text, ", PCM ");
    dump_hex(&text, hda->afg_pcm, 8);
    dump_add(&text, "\n");
    for (unsigned index = 0; index < hda->widget_count; index++) {
        struct widget *widget = &hda->widgets[index];
        dump_add(&text, "Node ");
        dump_hex(&text, widget->nid, 2);
        dump_add(&text, " [");
        dump_add(&text, widget_type_name(widget->type));
        dump_add(&text, "] wcaps ");
        dump_hex(&text, widget->caps, 8);
        dump_add(&text, (widget->caps & 1U) ? ": Stereo" : ": Mono");
        if (widget->caps & AWCAP_DIGITAL) dump_add(&text, " Digital");
        dump_add(&text, "\n");
        if (widget->caps & AWCAP_IN_AMP) dump_amp(&text, "Amp-In", widget->in_amp_cap);
        if (widget->caps & AWCAP_OUT_AMP) dump_amp(&text, "Amp-Out", widget->out_amp_cap);
        if (widget->type == WIDGET_OUTPUT || widget->type == WIDGET_INPUT) {
            dump_add(&text, "  PCM: ");
            dump_hex(&text, widget->pcm_caps, 8);
            dump_add(&text, "\n");
        }
        if (widget->type == WIDGET_PIN) {
            dump_add(&text, "  Pincap ");
            dump_hex(&text, widget->pin_caps, 8);
            dump_add(&text, ", Pin Default ");
            dump_hex(&text, widget->config, 8);
            dump_add(&text, ", Pin-ctls ");
            dump_hex(&text, codec_command(hda, widget->nid, VERB_GET_PIN_CONTROL, 0) & 0xFFU, 2);
            if (widget->pin_caps & PINCAP_EAPD) {
                dump_add(&text, ", EAPD ");
                dump_hex(&text, codec_command(hda, widget->nid, VERB_GET_EAPD, 0) & 0xFFU, 2);
            }
            if (pin_detectable(widget))
                dump_add(&text, read_presence(hda, widget->nid) ? ", plugged" : ", unplugged");
            dump_add(&text, "\n");
        }
        if (widget->connection_count) {
            dump_add(&text, "  Connection: ");
            dump_number(&text, widget->connection_count);
            dump_add(&text, "\n    ");
            uint32_t selected = widget->connection_count > 1 && widget->type != WIDGET_MIXER
                ? codec_command(hda, widget->nid, VERB_GET_CONNECT_SEL, 0) & 0xFFU
                : 0xFFU;
            for (unsigned slot = 0; slot < widget->connection_count; slot++) {
                dump_add(&text, slot ? " " : "");
                dump_hex(&text, widget->connections[slot], 2);
                if (slot == selected) dump_add(&text, "*");
            }
            dump_add(&text, "\n");
        }
    }
    dump_add(&text, "Paths:\n");
    for (unsigned index = 0; index < hda->output_count; index++)
        dump_path(&text, hda->outputs[index].label, hda->outputs[index].steps,
                  hda->outputs[index].length);
    for (unsigned index = 0; index < hda->input_count; index++)
        dump_path(&text, hda->inputs[index].label, hda->inputs[index].steps,
                  hda->inputs[index].length);
    if (text.capacity)
        text.out[text.length < text.capacity ? text.length : text.capacity - 1U] = '\0';
    return text.length < text.capacity ? text.length : text.capacity - 1U;
}

static void enable_interrupts(struct hda_controller *hda) {
    if (!pci_find_capability(&hda->pci, PCI_CAP_MSI)) return;
    unsigned vector = irq_request("snd_hda", "PCI-MSI", hda_interrupt, hda);
    if (!vector) return;
    if (pci_msi_bind(&hda->pci, vector) != 0) {
        irq_release(vector);
        return;
    }
    hda->vector = vector;
    uint32_t mask = INTCTL_GIE;
    for (int direction = 0; direction < 2; direction++)
        if (hda->streams[direction].bdl) mask |= 1U << hda->streams[direction].index;
    write32(hda, HDA_INTCTL, mask);
}

static void enable_snoop(struct hda_controller *hda) {
    struct pci_device *device = &hda->pci;
    if (device->vendor_id == PCI_VENDOR_INTEL) {
        uint32_t tcsel = pci_config_read32(device->bus, device->slot, device->function, PCI_TCSEL);
        pci_config_write32(device->bus, device->slot, device->function, PCI_TCSEL, tcsel & ~0x7U);
    }
    uint8_t express = pci_find_capability(device, PCI_CAP_EXPRESS);
    if (!express) return;
    uint32_t control = pci_config_read32(device->bus, device->slot, device->function, express + 8U);
    if (control & PCIE_DEVCTL_NOSNOOP)
        pci_config_write32(device->bus, device->slot, device->function, express + 8U,
                           control & ~PCIE_DEVCTL_NOSNOOP & 0xFFFFU);
}

static const char *controller_name(const struct pci_device *device) {
    switch (device->vendor_id) {
    case PCI_VENDOR_INTEL:  return "HDA Intel PCH";
    case PCI_VENDOR_NVIDIA: return "HDA NVidia";
    case PCI_VENDOR_AMD:    return "HD-Audio Generic";
    default:                return "HD-Audio Generic";
    }
}

static void controller_free(struct hda_controller *hda) {
    if (hda->vector) {
        write32(hda, HDA_INTCTL, 0);
        irq_release(hda->vector);
        hda->vector = 0;
    }
    kfree(hda);
}

static int hda_probe(const struct pci_device *found) {
    unsigned slot = MAX_CONTROLLERS;
    for (unsigned index = 0; index < MAX_CONTROLLERS; index++)
        if (!controllers[index]) {
            slot = index;
            break;
        }
    if (slot == MAX_CONTROLLERS) return -1;
    if (found->bar[0] & BAR_IO) return -1;
    uint64_t physical = found->bar[0] & BAR_ADDRESS_MASK;
    if ((found->bar[0] & BAR_TYPE_MASK) == BAR_TYPE_64BIT)
        physical |= (uint64_t)found->bar[1] << 32;
    if (!physical) {
        kprintf("HDA: controller has no register window\n");
        return -1;
    }
    struct hda_controller *hda = kmalloc(sizeof(*hda));
    if (!hda) return -1;
    memset(hda, 0, sizeof(*hda));
    hda->pci = *found;
    hda->transport = ring_transport;
    lock_init(&hda->codec_lock, "hda codec", 42);

    pci_enable_bus_mastering(&hda->pci);
    enable_snoop(hda);
    hda->base = vmm_map_device(physical, HDA_REGISTER_BYTES);
    if (!hda->base) {
        kprintf("HDA: cannot map registers at %x\n", (unsigned)physical);
        kfree(hda);
        return -1;
    }
    write32(hda, HDA_INTCTL, 0);
    if (reset_controller(hda) != 0) {
        kprintf("HDA: controller will not reset\n");
        kfree(hda);
        return -1;
    }
    write16(hda, HDA_WAKEEN, 0);
    write32(hda, HDA_INTSTS, read32(hda, HDA_INTSTS));
    uint16_t capabilities = read16(hda, HDA_GCAP);
    uint32_t input_streams = (capabilities >> GCAP_ISS_SHIFT) & GCAP_COUNT_MASK;
    uint32_t output_streams = (capabilities >> GCAP_OSS_SHIFT) & GCAP_COUNT_MASK;
    if (!output_streams) {
        kprintf("HDA: controller has no output streams\n");
        kfree(hda);
        return -1;
    }
    if (start_command_rings(hda) != 0) {
        kprintf("HDA: command ring setup failed\n");
        kfree(hda);
        return -1;
    }
    hda->position_buffer = dma_page(&hda->position_physical);
    if (!hda->position_buffer) {
        kfree(hda);
        return -1;
    }
    write32(hda, HDA_DPLBASE, (uint32_t)hda->position_physical | DPLBASE_ENABLE);
    write32(hda, HDA_DPUBASE, (uint32_t)(hda->position_physical >> 32));
    delay_ns(CODEC_SETTLE_NS);

    uint16_t present = read16(hda, HDA_STATESTS);
    int found_codec = 0;
    for (uint8_t address = 0; address < 15U && !found_codec; address++)
        if ((present & (1U << address)) && probe_codec(hda, address) == 0) found_codec = 1;
    if (!found_codec) {
        kprintf("HDA: %x:%x has no analog codec, left alone\n", (unsigned)found->vendor_id,
                (unsigned)found->device_id);
        write8(hda, HDA_CORBCTL, 0);
        write8(hda, HDA_RIRBCTL, 0);
        write32(hda, HDA_DPLBASE, 0);
        kfree(hda);
        return -1;
    }

    hda->streams[SND_PLAYBACK].index = input_streams;
    hda->streams[SND_PLAYBACK].tag = 1;
    hda->streams[SND_PLAYBACK].bdl = dma_page(&hda->streams[SND_PLAYBACK].bdl_physical);
    if (input_streams && hda->input_count && hda->adc) {
        hda->streams[SND_CAPTURE].index = 0;
        hda->streams[SND_CAPTURE].tag = 1;
        hda->streams[SND_CAPTURE].bdl = dma_page(&hda->streams[SND_CAPTURE].bdl_physical);
    }
    if (!hda->streams[SND_PLAYBACK].bdl) {
        kfree(hda);
        return -1;
    }

    uint32_t playback_caps = 0xFFFFFFFFU;
    for (unsigned index = 0; index < hda->output_count; index++)
        playback_caps &= widget_for(hda, hda->outputs[index].dac)->pcm_caps;
    fill_hardware(&hda->ops[SND_PLAYBACK].hardware, playback_caps);
    hda->ops[SND_PLAYBACK].configure = playback_configure;
    hda->ops[SND_PLAYBACK].prepare = playback_prepare;
    hda->ops[SND_PLAYBACK].trigger = playback_trigger;
    hda->ops[SND_PLAYBACK].position = playback_position;
    hda->ops[SND_PLAYBACK].release = playback_release;
    if (hda->streams[SND_CAPTURE].bdl) {
        fill_hardware(&hda->ops[SND_CAPTURE].hardware, widget_for(hda, hda->adc)->pcm_caps);
        hda->ops[SND_CAPTURE].configure = capture_configure;
        hda->ops[SND_CAPTURE].prepare = capture_prepare;
        hda->ops[SND_CAPTURE].trigger = capture_trigger;
        hda->ops[SND_CAPTURE].position = capture_position;
        hda->ops[SND_CAPTURE].release = capture_release;
    }

    hda->present = 1;
    setup_mixer(hda);
    build_controls(hda);
    describe_codec(hda);
    enable_interrupts(hda);

    size_t at = 0;
    put_text(hda->longname, &at, sizeof(hda->longname), controller_name(found));
    put_text(hda->longname, &at, sizeof(hda->longname), " at 0x");
    put_hex(hda->longname, &at, sizeof(hda->longname), (uint32_t)physical, 8);
    put_text(hda->longname, &at, sizeof(hda->longname), hda->vector ? " msi" : " polled");
    hda->longname[at] = '\0';

    hda->backend.driver = "HDA-Intel";
    hda->backend.name = controller_name(found);
    hda->backend.longname = hda->longname;
    hda->backend.mixer_name = hda->mixer_name;
    hda->backend.components = hda->components;
    hda->backend.location = SOUND_ON_PCI | ((uint32_t)found->bus << 16) |
        ((uint32_t)found->slot << 8) | found->function;
    hda->backend.context = hda;
    hda->backend.streams[SND_PLAYBACK] = &hda->ops[SND_PLAYBACK];
    hda->backend.streams[SND_CAPTURE] =
        hda->streams[SND_CAPTURE].bdl ? &hda->ops[SND_CAPTURE] : NULL;
    hda->backend.controls = hda->controls;
    hda->backend.control_count = hda->control_count;
    hda->backend.control_get = hda_control_get;
    hda->backend.control_put = hda_control_put;
    hda->backend.poll = hda_poll;
    hda->backend.proc_codec = hda_proc_codec;

    for (unsigned index = 0; index < hda->output_count; index++) {
        const struct output_path *path = &hda->outputs[index];
        kprintf("HDA: %s pin %u <- dac %u%s%s\n", path->label, (unsigned)path->pin,
                (unsigned)path->dac, path->detect ? ", jack" : "",
                path->detect && path->present ? " (plugged)" : "");
    }
    for (unsigned index = 0; index < hda->input_count; index++) {
        const struct input_path *path = &hda->inputs[index];
        kprintf("HDA: %s pin %u -> adc %u%s\n", path->label, (unsigned)path->pin,
                (unsigned)path->adc, path->detect ? ", jack" : "");
    }
    kprintf("HDA: %s codec %u, playback rates %u-%u%s, %s\n", hda->mixer_name, (unsigned)hda->codec,
            hda->ops[0].hardware.rates[0],
            hda->ops[0].hardware.rates[hda->ops[0].hardware.rate_count - 1U],
            hda->ops[0].hardware.formats & (1U << SND_FORMAT_S32_LE) ? ", 32-bit" : ", 16-bit",
            hda->vector ? "interrupts" : "polled");

    controllers[slot] = hda;
    if (snd_register_card(&hda->backend) < 0) {
        controllers[slot] = NULL;
        hda->present = 0;
        controller_free(hda);
        return -1;
    }
    return 0;
}

static void hda_remove(const struct pci_device *device) {
    for (unsigned index = 0; index < MAX_CONTROLLERS; index++) {
        struct hda_controller *hda = controllers[index];
        if (!hda || hda->pci.bus != device->bus || hda->pci.slot != device->slot ||
            hda->pci.function != device->function)
            continue;
        snd_unregister_card(&hda->backend);
        for (int direction = 0; direction < 2; direction++)
            if (hda->streams[direction].bdl) stop_stream(hda, &hda->streams[direction]);
        write32(hda, HDA_INTCTL, 0);
        hda->present = 0;
        controllers[index] = NULL;
        controller_free(hda);
    }
}

static const struct pci_device_id hda_ids[] = {
    {PCI_ANY_ID, PCI_ANY_ID, PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_HDA},
};

static struct pci_driver hda_driver = {
    .name = "snd_hda",
    .ids = hda_ids,
    .id_count = sizeof(hda_ids) / sizeof(hda_ids[0]),
    .probe = hda_probe,
    .remove = hda_remove,
};

static int snd_hda_start(void) { return pci_register_driver(&hda_driver); }

static void snd_hda_stop(void) { pci_unregister_driver(&hda_driver); }

MODULE_MAIN(snd_hda_start, snd_hda_stop);
MODULE_ALIAS("pci:v*d*sv*sd*bc04sc03i*");
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("Intel HD Audio controller");
MODULE_AUTHOR("Tunix");
