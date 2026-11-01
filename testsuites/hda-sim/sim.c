#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/modules/snd_hda.c"

uint64_t time_uptime_ns(void) {
    static uint64_t now;
    return now += 1000;
}
void *kmalloc(size_t size) { return calloc(1, size); }
void kfree(void *pointer) { free(pointer); }
void lock_init(struct lock *lock, const char *name, unsigned rank) {
    (void)lock;
    (void)name;
    (void)rank;
}
void lock_acquire(struct lock *lock) { (void)lock; }
void lock_release(struct lock *lock) { (void)lock; }
void kprintf(const char *format, ...) { (void)format; }
void *pmm_alloc_page(void) { return aligned_alloc(4096, 4096); }
void *vmm_phys_to_virt(uint64_t physical) { return (void *)physical; }
uint64_t vmm_map_device(uint64_t physical, uint64_t size) {
    (void)physical;
    return (uint64_t)calloc(1, size);
}
uint8_t pci_find_capability(const struct pci_device *device, uint8_t id) {
    (void)device;
    (void)id;
    return 0;
}
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    (void)bus;
    (void)slot;
    (void)function;
    (void)offset;
    return 0;
}
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset,
                        uint32_t value) {
    (void)bus;
    (void)slot;
    (void)function;
    (void)offset;
    (void)value;
}
void pci_enable_bus_mastering(const struct pci_device *device) { (void)device; }
unsigned irq_request(const char *name, const char *kind, irq_handler_fn handler, void *context) {
    (void)name;
    (void)kind;
    (void)handler;
    (void)context;
    return 0;
}
void irq_release(unsigned vector) { (void)vector; }
int pci_msi_bind(struct pci_device *device, unsigned vector) {
    (void)device;
    (void)vector;
    return -1;
}
int pci_register_driver(struct pci_driver *driver) {
    (void)driver;
    return 0;
}
void pci_unregister_driver(struct pci_driver *driver) { (void)driver; }

static unsigned notified[64];
static unsigned notify_count;
int snd_register_card(const struct snd_backend *backend) {
    (void)backend;
    return 0;
}
void snd_unregister_card(const struct snd_backend *backend) { (void)backend; }
void snd_pcm_period_elapsed(const struct snd_backend *backend, int direction) {
    (void)backend;
    (void)direction;
}
void snd_ctl_notify(const struct snd_backend *backend, unsigned control, unsigned mask) {
    (void)backend;
    (void)mask;
    if (notify_count < 64) notified[notify_count++] = control;
}

struct node {
    uint8_t nid;
    uint32_t caps;
    uint32_t pin_caps;
    uint32_t config;
    uint32_t out_amp;
    uint32_t in_amp;
    uint32_t pcm;
    uint8_t connections[16];
    uint8_t connection_count;
};

struct model {
    const char *name;
    uint32_t vendor;
    uint32_t subsystem;
    uint8_t first;
    uint8_t count;
    uint32_t afg_pcm;
    uint32_t afg_out_amp;
    uint32_t afg_in_amp;
    struct node nodes[48];
    unsigned node_count;
};

static const struct model *model;
static uint32_t pin_control[256];
static uint32_t eapd[256];
static uint32_t connect_select[256];
static uint32_t amp_out[256][2];
static uint32_t amp_in[256][16][2];
static uint32_t converter_format[256];
static uint32_t converter_stream[256];
static int presence[256];

static const struct node *find_node(uint8_t nid) {
    for (unsigned index = 0; index < model->node_count; index++)
        if (model->nodes[index].nid == nid) return &model->nodes[index];
    return NULL;
}

static uint32_t mock_transport(struct hda_controller *hda, uint32_t value) {
    (void)hda;
    uint8_t nid = (uint8_t)((value >> 20) & 0x7FU);
    uint32_t high = (value >> 16) & 0xFU;
    uint32_t verb;
    uint32_t payload;
    if (high == VERB_SET_CONVERTER_FORMAT || high == VERB_SET_AMP_GAIN_MUTE) {
        verb = high;
        payload = value & 0xFFFFU;
    } else {
        verb = (value >> 8) & 0xFFFU;
        payload = value & 0xFFU;
    }
    if (verb == VERB_GET_PARAMETER) {
        if (nid == 0) {
            if (payload == PARAM_VENDOR_ID) return model->vendor;
            if (payload == PARAM_REVISION_ID) return 0x100100;
            if (payload == PARAM_NODE_COUNT) return (1U << 16) | 1U;
            return 0;
        }
        if (nid == 1) {
            if (payload == PARAM_FUNCTION_TYPE) return FUNCTION_TYPE_AUDIO;
            if (payload == PARAM_NODE_COUNT) return ((uint32_t)model->first << 16) | model->count;
            if (payload == PARAM_PCM) return model->afg_pcm;
            if (payload == PARAM_OUT_AMP_CAP) return model->afg_out_amp;
            if (payload == PARAM_IN_AMP_CAP) return model->afg_in_amp;
            return 0;
        }
        const struct node *node = find_node(nid);
        if (!node) return payload == PARAM_AUDIO_WIDGET_CAP ? 0x00F00000U : 0;
        switch (payload) {
        case PARAM_AUDIO_WIDGET_CAP: return node->caps;
        case PARAM_PIN_CAP:          return node->pin_caps;
        case PARAM_OUT_AMP_CAP:      return node->out_amp;
        case PARAM_IN_AMP_CAP:       return node->in_amp;
        case PARAM_PCM:              return node->pcm;
        case PARAM_CONNLIST_LEN:     return node->connection_count;
        default:                     return 0;
        }
    }
    const struct node *node = find_node(nid);
    switch (verb) {
    case VERB_GET_CONNECT_LIST: {
        uint32_t response = 0;
        for (unsigned slot = 0; slot < 4; slot++)
            if (node && payload + slot < node->connection_count)
                response |= (uint32_t)node->connections[payload + slot] << (slot * 8U);
        return response;
    }
    case VERB_GET_CONFIG_DEFAULT:   return node ? node->config : 0;
    case VERB_GET_SUBSYSTEM_ID:     return model->subsystem;
    case VERB_GET_PIN_SENSE:        return presence[nid] ? 0x80000000U : 0;
    case VERB_SET_PIN_CONTROL:      pin_control[nid] = payload; return 0;
    case VERB_SET_EAPD:             eapd[nid] = payload; return 0;
    case VERB_SET_CONNECT_SEL:      connect_select[nid] = payload; return 0;
    case VERB_SET_STREAM_CHANNEL:   converter_stream[nid] = payload; return 0;
    case VERB_SET_CONVERTER_FORMAT: converter_format[nid] = payload; return 0;
    case VERB_SET_AMP_GAIN_MUTE:    {
        unsigned index = (payload >> 8) & 0xFU;
        uint32_t level = payload & 0xFFU;
        for (unsigned channel = 0; channel < 2; channel++) {
            if (!(payload & (channel ? AMP_SET_RIGHT : AMP_SET_LEFT))) continue;
            if (payload & AMP_SET_OUTPUT) amp_out[nid][channel] = level;
            if (payload & AMP_SET_INPUT) amp_in[nid][index][channel] = level;
        }
        return 0;
    }
    default: return 0;
    }
}

static unsigned failures;
static unsigned checks;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("hda-sim: FAIL  %s: %s\n", model->name, what);
    }
}

static int has_control(struct hda_controller *hda, const char *name, unsigned *index) {
    for (unsigned at = 0; at < hda->control_count; at++)
        if (!strcmp(hda->controls[at].name, name)) {
            if (index) *index = at;
            return 1;
        }
    return 0;
}

#define WCAP(type, extra) (((uint32_t)(type) << 20) | (extra))
#define STEREO            0x1U
#define IN_AMP            0x2U
#define OUT_AMP           0x4U
#define OVERRIDE          0x8U
#define CONN              0x100U
#define DIGITAL           0x200U
#define POWER             0x400U
#define UNSOL             0x80U

static const struct model alc269 = {
    .name = "ALC269 laptop",
    .vendor = 0x10EC0269,
    .subsystem = 0x152D0831,
    .first = 0x02,
    .count = 0x23,
    .afg_pcm = 0x000E0560,
    .afg_out_amp = 0,
    .afg_in_amp = 0,
    .nodes =
        {
            {0x02,
             WCAP(0, STEREO | OUT_AMP | OVERRIDE | 0x10U),
             0,
             0,
             0x00034040,
             0,
             0x000E0560,
             {0},
             0},
            {0x03,
             WCAP(0, STEREO | OUT_AMP | OVERRIDE | 0x10U),
             0,
             0,
             0x00034040,
             0,
             0x000E0560,
             {0},
             0},
            {0x07, WCAP(1, STEREO | CONN | DIGITAL), 0, 0, 0, 0, 0x000E0560, {0x24}, 1},
            {0x08,
             WCAP(1, STEREO | IN_AMP | OVERRIDE | CONN),
             0,
             0,
             0,
             0x80051F0B,
             0x000E0560,
             {0x23},
             1},
            {0x09,
             WCAP(1, STEREO | IN_AMP | OVERRIDE | CONN),
             0,
             0,
             0,
             0x80051F0B,
             0x000E0560,
             {0x22},
             1},
            {0x0B,
             WCAP(2, STEREO | IN_AMP | OVERRIDE | CONN),
             0,
             0,
             0,
             0x80051F17,
             0,
             {0x18, 0x19, 0x1A, 0x1B, 0x1D},
             5},
            {0x0C,
             WCAP(2, STEREO | IN_AMP | OVERRIDE | CONN),
             0,
             0,
             0,
             0x80000000,
             0,
             {0x02, 0x0B},
             2},
            {0x0D,
             WCAP(2, STEREO | IN_AMP | OVERRIDE | CONN),
             0,
             0,
             0,
             0x80000000,
             0,
             {0x03, 0x0B},
             2},
            {0x0F, WCAP(2, IN_AMP | OVERRIDE | CONN), 0, 0, 0, 0x80000000, 0, {0x02, 0x0B}, 2},
            {0x12, WCAP(4, STEREO | CONN), 0x00000020, 0x40000000, 0, 0, 0, {0}, 0},
            {0x14,
             WCAP(4, STEREO | OUT_AMP | OVERRIDE | CONN | UNSOL),
             0x00010014,
             0x99130110,
             0x80000000,
             0,
             0,
             {0x0C, 0x0D},
             2},
            {0x15,
             WCAP(4, STEREO | OUT_AMP | OVERRIDE | CONN | UNSOL),
             0x0001001C,
             0x0321101F,
             0x80000000,
             0,
             0,
             {0x0C, 0x0D},
             2},
            {0x17,
             WCAP(4, OUT_AMP | OVERRIDE | CONN),
             0x00000010,
             0x411111F0,
             0x80000000,
             0,
             0,
             {0x0F},
             1},
            {0x18,
             WCAP(4, STEREO | IN_AMP | OUT_AMP | OVERRIDE | CONN | UNSOL),
             0x00003734,
             0x03A11820,
             0x80000000,
             0x00270300,
             0,
             {0x0C, 0x0D},
             2},
            {0x19,
             WCAP(4, STEREO | IN_AMP | OVERRIDE | UNSOL),
             0x00003724,
             0x99A30930,
             0,
             0x00270300,
             0,
             {0},
             0},
            {0x1A,
             WCAP(4, STEREO | IN_AMP | OUT_AMP | OVERRIDE | CONN | UNSOL),
             0x0000373C,
             0x411111F0,
             0x80000000,
             0x00270300,
             0,
             {0x0C, 0x0D},
             2},
            {0x1B,
             WCAP(4, STEREO | IN_AMP | OUT_AMP | OVERRIDE | CONN | UNSOL),
             0x0000373C,
             0x411111F0,
             0x80000000,
             0x00270300,
             0,
             {0x0C, 0x0D},
             2},
            {0x1D, WCAP(4, STEREO | CONN), 0x00000020, 0x40079A2D, 0, 0, 0, {0}, 0},
            {0x1E, WCAP(4, CONN | DIGITAL), 0x00000010, 0x411111F0, 0, 0, 0, {0x06}, 1},
            {0x22,
             WCAP(2, STEREO | IN_AMP | OVERRIDE | CONN),
             0,
             0,
             0,
             0x80000000,
             0,
             {0x18, 0x19, 0x1A, 0x1B, 0x1D, 0x0B, 0x12},
             7},
            {0x23,
             WCAP(2, STEREO | IN_AMP | OVERRIDE | CONN),
             0,
             0,
             0,
             0x80000000,
             0,
             {0x18, 0x19, 0x1A, 0x1B, 0x1D, 0x0B, 0x12},
             7},
        },
    .node_count = 21,
};

static const struct model qemu_output = {
    .name = "QEMU hda-output",
    .vendor = 0x1AF40012,
    .subsystem = 0x1AF40012,
    .first = 0x02,
    .count = 2,
    .afg_pcm = 0x000201FC,
    .nodes =
        {
            {0x02,
             WCAP(0, STEREO | OUT_AMP | OVERRIDE | 0x10U),
             0,
             0,
             0x80004A4A,
             0,
             0x000201FC,
             {0},
             0},
            {0x03, WCAP(4, STEREO | CONN), 0x00000010, 0x01014010, 0, 0, 0, {0x02}, 1},
        },
    .node_count = 2,
};

static const struct model hdmi_only = {
    .name = "NVIDIA HDMI",
    .vendor = 0x10DE000B,
    .subsystem = 0x10DE0101,
    .first = 0x04,
    .count = 2,
    .afg_pcm = 0x000E07F0,
    .nodes =
        {
            {0x04, WCAP(0, STEREO | DIGITAL | 0x10U), 0, 0, 0, 0, 0x000E07F0, {0}, 0},
            {0x05,
             WCAP(4, STEREO | CONN | DIGITAL | UNSOL),
             0x00000094,
             0x18560010,
             0,
             0,
             0,
             {0x04},
             1},
        },
    .node_count = 2,
};

static const struct model desktop = {
    .name = "desktop with a capture selector",
    .vendor = 0x111D76D1,
    .subsystem = 0x10431234,
    .first = 0x02,
    .count = 0x11,
    .afg_pcm = 0x000E01E0,
    .nodes =
        {
            {0x02,
             WCAP(0, STEREO | OUT_AMP | OVERRIDE | 0x10U),
             0,
             0,
             0x80051F1F,
             0,
             0x000E01E0,
             {0},
             0},
            {0x03,
             WCAP(0, STEREO | OUT_AMP | OVERRIDE | 0x10U),
             0,
             0,
             0x80051F1F,
             0,
             0x000E01E0,
             {0},
             0},
            {0x0A,
             WCAP(4, STEREO | CONN | UNSOL),
             0x00010014,
             0x01014010,
             0,
             0,
             0,
             {0x02, 0x03},
             2},
            {0x0B,
             WCAP(4, STEREO | CONN | UNSOL),
             0x0001001C,
             0x02214020,
             0,
             0,
             0,
             {0x02, 0x03},
             2},
            {0x0C,
             WCAP(4, STEREO | IN_AMP | OVERRIDE | UNSOL),
             0x00001724,
             0x01A19030,
             0,
             0x00270300,
             0,
             {0},
             0},
            {0x0D,
             WCAP(4, STEREO | IN_AMP | OVERRIDE | UNSOL),
             0x00001724,
             0x02A19040,
             0,
             0x00270300,
             0,
             {0},
             0},
            {0x0E, WCAP(4, STEREO | UNSOL), 0x00000024, 0x01813050, 0, 0, 0, {0}, 0},
            {0x10,
             WCAP(3, STEREO | OUT_AMP | OVERRIDE | CONN),
             0,
             0,
             0x80050F00,
             0,
             0,
             {0x0C, 0x0D, 0x0E},
             3},
            {0x12, WCAP(1, STEREO | CONN | 0x10U), 0, 0, 0, 0, 0x000E01E0, {0x10}, 1},
        },
    .node_count = 9,
};

static struct hda_controller *start(const struct model *which) {
    model = which;
    memset(pin_control, 0xFF, sizeof(pin_control));
    memset(eapd, 0, sizeof(eapd));
    memset(presence, 0, sizeof(presence));
    memset(amp_in, 0, sizeof(amp_in));
    memset(amp_out, 0, sizeof(amp_out));
    notify_count = 0;
    struct hda_controller *hda = calloc(1, sizeof(*hda));
    hda->transport = mock_transport;
    hda->base = (uint64_t)calloc(1, 0x4000);
    return hda;
}

static int input_open(unsigned mixer, unsigned index) {
    return amp_in[mixer][index][0] != 0x80U && !(amp_in[mixer][index][0] & 0x80U);
}

int main(void) {
    struct hda_controller *hda = start(&alc269);
    check(probe_codec(hda, 0) == 0, "the codec probes");
    check(hda->output_count == 2, "two outputs are found");
    check(hda->outputs[0].pin == 0x14 && hda->outputs[0].role == ROLE_SPEAKER,
          "the speaker comes first");
    check(hda->outputs[1].pin == 0x15 && hda->outputs[1].role == ROLE_HEADPHONE,
          "then the headphone");
    check(hda->outputs[0].dac == 0x02 && hda->outputs[1].dac == 0x03, "each has its own DAC");
    check(hda->outputs[0].steps[1].nid == 0x0C && hda->outputs[0].steps[1].index == 0,
          "the speaker goes through 0x0c input 0");
    check(hda->outputs[1].steps[1].nid == 0x0D, "the headphone goes through 0x0d");
    check(hda->outputs[0].steps[2].nid == 0x14 && hda->outputs[0].steps[2].index == 0,
          "the speaker pin selects 0x0c");
    check(hda->outputs[1].steps[2].index == 1, "the headphone pin selects 0x0d");
    check(!hda->outputs[0].detect && hda->outputs[1].detect, "only the headphone senses a plug");
    check(hda->input_count == 2, "two inputs are found");
    check(hda->adc == 0x08, "capture uses ADC 0x08");
    check(hda->inputs[0].pin == 0x18 && hda->inputs[0].role == ROLE_MIC,
          "the mic jack is an input");
    check(hda->inputs[1].pin == 0x19 && hda->inputs[1].role == ROLE_INTERNAL_MIC,
          "the internal mic too");
    check(hda->inputs[0].boost == 0x18, "the mic has a boost amp");
    check(hda->capture_volume_nid == 0x08 && hda->capture_volume_input,
          "the capture volume is the ADC input amp");

    hda->present = 1;
    hda->ops[SND_PLAYBACK].hardware.rate_count = 0;
    fill_hardware(&hda->ops[SND_PLAYBACK].hardware, widget_for(hda, 0x02)->pcm_caps);
    struct snd_hardware *hw = &hda->ops[SND_PLAYBACK].hardware;
    check((hw->formats & (1U << SND_FORMAT_S32_LE)) && hw->msbits_s32 == 24,
          "32-bit samples carry 24 valid bits");
    check(hw->formats & (1U << SND_FORMAT_S16_LE), "16-bit is offered too");
    static const uint32_t expected[] = {44100, 48000, 96000, 192000};
    int rates_match = hw->rate_count == 4;
    for (unsigned at = 0; rates_match && at < 4; at++) rates_match = hw->rates[at] == expected[at];
    check(rates_match, "the rates are exactly the codec's");
    struct snd_stream_format format = {48000, 2, SND_FORMAT_S32_LE, 24};
    uint16_t encoded = 0;
    check(encode_format(&format, &encoded) == 0 && encoded == 0x0031,
          "S32 with 24 valid bits is sent as 24-bit");
    format.format = SND_FORMAT_S16_LE;
    check(encode_format(&format, &encoded) == 0 && encoded == 0x0011, "S16 is sent as 16-bit");
    format.rate = 44100;
    check(encode_format(&format, &encoded) == 0 && encoded == 0x4011,
          "44.1 kHz uses the 44.1 base");

    setup_mixer(hda);
    build_controls(hda);
    check(hda->has_auto_mute && hda->auto_mic, "auto-mute and auto-mic are set up");
    check(pin_control[0x14] == PIN_OUT, "the speaker plays with nothing plugged in");
    check(pin_control[0x15] == (PIN_OUT | PIN_HP), "the headphone pin drives headphones");
    check(eapd[0x14] == EAPD_ENABLE && eapd[0x15] == EAPD_ENABLE, "EAPD is on for both");
    check(pin_control[0x17] == 0 && pin_control[0x1A] == 0 && pin_control[0x1B] == 0,
          "unused pins are switched off");
    check(pin_control[0x18] == (PIN_IN | PIN_VREF_80), "the mic jack gets 80% bias");
    check(amp_out[0x02][0] == 0x40 && amp_out[0x03][0] == 0x40, "both DACs start at 0 dB");
    check(input_open(0x0C, 0) && !input_open(0x0C, 1),
          "the speaker mixer passes the DAC and mutes the loopback");
    check(input_open(0x23, 1) && !input_open(0x23, 0), "capture starts on the internal mic");
    unsigned volume = 0, jack = 0, mute_mode = 0, mic_jack = 0;
    check(has_control(hda, "Master Playback Volume", &volume), "Master Playback Volume exists");
    check(hda->controls[volume].db_min == -6400 && hda->controls[volume].db_step == 100,
          "with a dB scale of -64 to 0 dB");
    check(has_control(hda, "Speaker Playback Switch", NULL), "Speaker Playback Switch exists");
    check(has_control(hda, "Headphone Playback Switch", NULL), "Headphone Playback Switch exists");
    check(has_control(hda, "Auto-Mute Mode", &mute_mode), "Auto-Mute Mode exists");
    check(has_control(hda, "Capture Volume", NULL) && has_control(hda, "Capture Switch", NULL),
          "capture controls exist");
    check(has_control(hda, "Mic Boost Volume", NULL), "Mic Boost Volume exists");
    check(has_control(hda, "Internal Mic Boost Volume", NULL), "Internal Mic Boost Volume exists");
    check(has_control(hda, "Headphone Jack", &jack) && has_control(hda, "Mic Jack", &mic_jack),
          "jack controls exist");
    check(has_control(hda, "Speaker Phantom Jack", NULL), "the speaker has a phantom jack");
    check(!has_control(hda, "Input Source", NULL), "auto-mic hides the source switch");

    static char dump[32768];
    describe_codec(hda);
    size_t dumped = hda_proc_codec(hda, dump, sizeof(dump));
    check(dumped > 0 && strstr(dump, "Vendor Id: 0x10ec0269") != NULL,
          "the codec dump names the codec");
    check(strstr(dump, "Speaker: 0x02 -> 0x0c[0] -> 0x14[0]") != NULL,
          "and shows the speaker path");
    check(strstr(dump, "Headphone: 0x03 -> 0x0d[0] -> 0x15[1]") != NULL, "and the headphone path");
    check(strstr(dump, "Node 0x15 [Pin Complex]") != NULL, "and every pin");
    check(!strcmp(hda->mixer_name, "Realtek ALC269"), "the mixer is named after the codec");
    long values[2] = {20, 20};
    check(hda_control_put(hda, volume, values) == 1, "lowering Master is a change");
    check(amp_out[0x02][0] == 20 && amp_out[0x03][1] == 20, "and reaches both DACs");

    presence[0x15] = 1;
    hda_poll(hda);
    check(pin_control[0x14] == 0, "plugging headphones mutes the speaker");
    check(pin_control[0x15] == (PIN_OUT | PIN_HP), "and keeps the headphones");
    check(notify_count == 1 && notified[0] == jack, "and reports the headphone jack");
    long now[2];
    hda_control_get(hda, jack, now);
    check(now[0] == 1, "the jack control reads plugged");
    long off[1] = {0};
    check(hda_control_put(hda, mute_mode, off) == 1, "auto-mute can be disabled");
    check(pin_control[0x14] == PIN_OUT, "which brings the speaker back");
    long on[1] = {1};
    hda_control_put(hda, mute_mode, on);
    presence[0x15] = 0;
    notify_count = 0;
    hda_poll(hda);
    check(pin_control[0x14] == PIN_OUT, "unplugging restores the speaker");

    presence[0x18] = 1;
    notify_count = 0;
    hda_poll(hda);
    check(input_open(0x23, 0) && !input_open(0x23, 1), "plugging a mic switches capture to it");
    check(notify_count == 1 && notified[0] == mic_jack, "and reports the mic jack");
    presence[0x18] = 0;
    hda_poll(hda);
    check(input_open(0x23, 1) && !input_open(0x23, 0), "unplugging returns to the internal mic");

    hda = start(&desktop);
    check(probe_codec(hda, 0) == 0, "the codec probes");
    check(hda->output_count == 2 && hda->outputs[0].pin == 0x0A &&
              hda->outputs[0].role == ROLE_LINE_OUT,
          "the rear line out comes first");
    check(hda->outputs[1].pin == 0x0B && hda->outputs[1].role == ROLE_HEADPHONE,
          "then the front headphone");
    check(hda->outputs[0].dac == 0x02 && hda->outputs[1].dac == 0x03, "on separate DACs");
    check(hda->outputs[1].steps[1].nid == 0x0B && hda->outputs[1].steps[1].index == 1,
          "the headphone pin selects its DAC");
    check(hda->input_count == 3, "three inputs are found");
    check(!strcmp(hda->inputs[0].label, "Rear Mic") && !strcmp(hda->inputs[1].label, "Front Mic") &&
              !strcmp(hda->inputs[2].label, "Line"),
          "duplicate microphones are told apart by location");
    check(hda->capture_volume_nid == 0x10 && !hda->capture_volume_input,
          "the selector carries the capture volume");
    hda->present = 1;
    setup_mixer(hda);
    build_controls(hda);
    check(!hda->auto_mic, "two microphone jacks leave the choice to the user");
    unsigned source = 0;
    check(has_control(hda, "Input Source", &source), "Input Source exists");
    check(hda->controls[source].item_count == 3 &&
              !strcmp(hda->controls[source].items[1], "Front Mic"),
          "and lists the inputs by name");
    check(has_control(hda, "Rear Mic Boost Volume", NULL) &&
              has_control(hda, "Front Mic Boost Volume", NULL),
          "each microphone has its own boost");
    check(has_control(hda, "Line Out Playback Switch", NULL) && has_control(hda, "Line Jack", NULL),
          "line out and line in controls exist");
    check(connect_select[0x10] == 0, "capture starts on the first input");
    long front[1] = {1};
    check(hda_control_put(hda, source, front) == 1, "choosing the front mic is a change");
    check(connect_select[0x10] == 1, "and the selector follows");
    check(pin_control[0x0A] == PIN_OUT && pin_control[0x0B] == (PIN_OUT | PIN_HP),
          "both outputs play");
    presence[0x0B] = 1;
    hda_poll(hda);
    check(pin_control[0x0A] == 0, "front headphones mute the rear line out");

    hda = start(&qemu_output);
    check(probe_codec(hda, 0) == 0, "the codec probes");
    check(hda->output_count == 1 && hda->outputs[0].pin == 0x03 &&
              hda->outputs[0].role == ROLE_LINE_OUT,
          "one line out");
    fill_hardware(&hda->ops[SND_PLAYBACK].hardware, widget_for(hda, 0x02)->pcm_caps);
    check(hda->ops[SND_PLAYBACK].hardware.formats == (1U << SND_FORMAT_S16_LE), "16-bit only");

    hda = start(&hdmi_only);
    check(probe_codec(hda, 0) != 0, "an HDMI-only codec is not taken");

    printf("hda-sim: %s %u/%u\n", failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
