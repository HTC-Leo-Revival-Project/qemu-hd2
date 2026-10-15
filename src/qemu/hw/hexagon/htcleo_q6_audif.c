/*
 * QSD8250 QDSP6 subsystem: the audio interface DMA (PA 0xA0700000)
 *
 * The DSP1 firmware's audio front end (AFE) streams PCM between rings in
 * its memory and the codec ports through this block; reverse-engineered
 * from the firmware's driver:
 *   0x00          interface configuration (the firmware writes
 *                 0x07001070 when playback starts)
 *   0x20          interrupt enable, 0x24 status, 0x2c clear (write 1s):
 *                 per DMA channel k bit 3k "period done", bits 3k+1 and
 *                 3k+2 errors; bit 12 a flag the ISR reports first
 *   0x34 + 0x30k  DMA channel k (four, the firmware's "DMA channel 0..3"):
 *     +0x00       control (2: run)
 *     +0x04       ring base (physical, in the DSP's memory)
 *     +0x08       ring length in words, minus 1
 *     +0x10       period in words, minus 1
 *     +0x14       position: bits 0-19 the current address, 20-22 the
 *                 FIFO fill (read)
 * The block interrupts on DSP interrupt 12.  Channel 0 feeds the codec's
 * playback path; one word is a stereo frame of two 16-bit samples, at
 * 48 kHz.  Playback goes to the QEMU audio backend (audiodev property).
 * On the speaker path the firmware leaves the right sample zero: the HD2's
 * one loudspeaker plays the codec's left channel, so with "speaker" (the
 * default; no headset is ever plugged in) the host hears that on both
 * channels.  The dump file keeps the interface's own samples.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "system/physmem.h"
#include "hw/hexagon/htcleo_q6_audif.h"

OBJECT_DECLARE_SIMPLE_TYPE(Q6AudifState, HTCLEO_Q6_AUDIF)

#define AUDIF_SIZE      0x100
#define R_IRQ_EN        0x20
#define R_IRQ_STAT      0x24
#define R_IRQ_CLR       0x2c
#define DMA_BASE        0x34
#define DMA_STRIDE      0x30
#define DMA_NUM         4
#define D_CTL           0x00
#define D_ADDR          0x04
#define D_LEN           0x08
#define D_PERIOD        0x10
#define D_POS           0x14
#define D_RUN           2

#define AUDIF_RATE      48000

typedef struct Q6AudifDma {
    QEMUTimer *timer;
    uint32_t pos;               /* words from the ring base */
    bool running;
} Q6AudifDma;

struct Q6AudifState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[AUDIF_SIZE / 4];
    Q6AudifDma dma[DMA_NUM];
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    bool trace;
    bool speaker;               /* the host hears the mono loudspeaker */
    char *dump;                 /* raw 48 kHz S16 stereo of DMA channel 0 */
    FILE *dump_f;
};

static uint32_t dreg(Q6AudifState *s, int k, int r)
{
    return s->regs[(DMA_BASE + DMA_STRIDE * k + r) / 4];
}

static void audif_update_irq(Q6AudifState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[R_IRQ_STAT / 4] &
                            s->regs[R_IRQ_EN / 4]));
}

static void audif_voice_cb(void *opaque, int avail)
{
    /* paced by the DMA timer; the backend just takes what it gets */
}

static void audif_start(Q6AudifState *s, int k)
{
    Q6AudifDma *d = &s->dma[k];
    uint32_t period = (dreg(s, k, D_PERIOD) & 0xffff) + 1;

    d->running = true;
    d->pos = 0;
    if (k == 0 && s->audio_be) {
        struct audsettings as = {
            .freq = AUDIF_RATE,
            .nchannels = 2,
            .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };

        s->voice = audio_be_open_out(s->audio_be, s->voice, "htcleo.q6.afe",
                                     s, audif_voice_cb, &as);
        if (s->voice) {
            audio_be_set_active_out(s->audio_be, s->voice, true);
        }
    }
    if (s->trace) {
        qemu_log("q6-audif: dma %d start, ring %08x %u words, period %u\n",
                 k, dreg(s, k, D_ADDR), (dreg(s, k, D_LEN) & 0xffff) + 1,
                 period);
    }
    timer_mod(d->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              muldiv64(period, NANOSECONDS_PER_SECOND, AUDIF_RATE));
}

static void audif_stop(Q6AudifState *s, int k)
{
    Q6AudifDma *d = &s->dma[k];

    d->running = false;
    timer_del(d->timer);
    if (k == 0 && s->voice && s->audio_be) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
    if (s->trace) {
        qemu_log("q6-audif: dma %d stop\n", k);
    }
}

static void audif_period_k(Q6AudifState *s, int k)
{
    Q6AudifDma *d = &s->dma[k];
    uint32_t base = dreg(s, k, D_ADDR);
    uint32_t len = (dreg(s, k, D_LEN) & 0xffff) + 1;
    uint32_t period = (dreg(s, k, D_PERIOD) & 0xffff) + 1;

    if (!d->running || !len || period > len) {
        return;
    }
    if (k == 0 && ((s->voice && s->audio_be) || s->dump_f)) {
        g_autofree uint8_t *buf = g_malloc(period * 4);
        uint32_t first = MIN(period, len - d->pos);

        physical_memory_read(base + d->pos * 4, buf, first * 4);
        if (first < period) {
            physical_memory_read(base, buf + first * 4,
                                 (period - first) * 4);
        }
        if (s->dump_f) {
            fwrite(buf, 4, period, s->dump_f);
            fflush(s->dump_f);
        }
        if (s->voice && s->audio_be) {
            if (s->speaker) {
                for (uint32_t i = 0; i < period; i++) {
                    memcpy(buf + 4 * i + 2, buf + 4 * i, 2);
                }
            }
            audio_be_write(s->audio_be, s->voice, buf, period * 4);
        }
    }
    d->pos = (d->pos + period) % len;
    s->regs[R_IRQ_STAT / 4] |= 1u << (3 * k);
    audif_update_irq(s);
    timer_mod(d->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              muldiv64(period, NANOSECONDS_PER_SECOND, AUDIF_RATE));
}

#define DMA_TIMER_FN(k) \
    static void audif_timer##k(void *opaque) \
    { \
        audif_period_k(opaque, k); \
    }
DMA_TIMER_FN(0) DMA_TIMER_FN(1) DMA_TIMER_FN(2) DMA_TIMER_FN(3)

static uint64_t audif_read(void *opaque, hwaddr off, unsigned size)
{
    Q6AudifState *s = opaque;

    if (off >= DMA_BASE && off < DMA_BASE + DMA_STRIDE * DMA_NUM) {
        int k = (off - DMA_BASE) / DMA_STRIDE;
        int r = (off - DMA_BASE) % DMA_STRIDE;

        if (r == D_POS) {
            return (dreg(s, k, D_ADDR) + s->dma[k].pos * 4) & 0xfffff;
        }
    }
    return s->regs[off / 4];
}

static void audif_write(void *opaque, hwaddr off, uint64_t val,
                        unsigned size)
{
    Q6AudifState *s = opaque;

    if (s->trace) {
        qemu_log("q6-audif: [%02x] <- %08x\n", (unsigned)off,
                 (unsigned)val);
    }
    if (off == R_IRQ_CLR) {
        s->regs[R_IRQ_STAT / 4] &= ~val;
        audif_update_irq(s);
        return;
    }
    s->regs[off / 4] = val;
    if (off == R_IRQ_EN) {
        audif_update_irq(s);
    } else if (off >= DMA_BASE && off < DMA_BASE + DMA_STRIDE * DMA_NUM &&
               (off - DMA_BASE) % DMA_STRIDE == D_CTL) {
        int k = (off - DMA_BASE) / DMA_STRIDE;
        bool run = val & D_RUN;

        if (run && !s->dma[k].running) {
            audif_start(s, k);
        } else if (!run && s->dma[k].running) {
            audif_stop(s, k);
        }
    }
}

static const MemoryRegionOps audif_ops = {
    .read = audif_read,
    .write = audif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void audif_reset(DeviceState *dev)
{
    Q6AudifState *s = HTCLEO_Q6_AUDIF(dev);

    for (int k = 0; k < DMA_NUM; k++) {
        if (s->dma[k].running) {
            audif_stop(s, k);
        }
    }
    memset(s->regs, 0, sizeof(s->regs));
    audif_update_irq(s);
}

static void audif_realize(DeviceState *dev, Error **errp)
{
    Q6AudifState *s = HTCLEO_Q6_AUDIF(dev);
    static QEMUTimerCB *const cb[DMA_NUM] = {
        audif_timer0, audif_timer1, audif_timer2, audif_timer3,
    };

    if (!audio_be_check(&s->audio_be, NULL)) {
        s->audio_be = NULL;
    }
    if (s->dump) {
        s->dump_f = fopen(s->dump, "wb");
    }
    for (int k = 0; k < DMA_NUM; k++) {
        s->dma[k].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cb[k], s);
    }
}

static void audif_init(Object *obj)
{
    Q6AudifState *s = HTCLEO_Q6_AUDIF(obj);

    memory_region_init_io(&s->iomem, obj, &audif_ops, s, "q6-audif",
                          AUDIF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property audif_props[] = {
    DEFINE_AUDIO_PROPERTIES(Q6AudifState, audio_be),
    DEFINE_PROP_BOOL("trace", Q6AudifState, trace, false),
    DEFINE_PROP_BOOL("speaker", Q6AudifState, speaker, true),
    DEFINE_PROP_STRING("dump", Q6AudifState, dump),
};

static void audif_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = audif_realize;
    device_class_set_legacy_reset(dc, audif_reset);
    device_class_set_props(dc, audif_props);
}

static const TypeInfo audif_info = {
    .name = TYPE_HTCLEO_Q6_AUDIF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Q6AudifState),
    .instance_init = audif_init,
    .class_init = audif_class_init,
};

static void audif_register(void)
{
    type_register_static(&audif_info);
}
type_init(audif_register)
