/*
 * HTC HD2: interrupt lines to and from a QDSP6 run in its own QEMU
 *
 * With qdsp6=lle the multimedia DSP runs its real firmware in
 * qemu-system-hexagon -M htcleo-q6, which shares the DRAM (and so the
 * SMEM) with this machine.  What is left between the two processes are
 * the interrupt lines, carried over a chardev one byte per edge: the line
 * number, 0-127.
 *   to-q6 n    raised here, line n there:
 *              0  apps -> DSP (A2M 8, the SMD/DAL doorbell)
 *              1  modem -> DSP (the modem's eighth outgoing interrupt)
 *   from-q6 n  line n raised there, pulsed here:
 *              0  DSP -> apps (INT_ADSP_A11)
 *              1  DSP -> modem
 * Byte 0x7f releases the QDSP6 from reset (msm_q6_link_boot): OSBL does
 * that once the shared memory is set up, so the DSP cannot run before.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmQ6LinkState, MSM_Q6_LINK)

#define Q6_LINK_LINES   16

struct MsmQ6LinkState {
    SysBusDevice parent_obj;
    CharFrontend chr;
    qemu_irq from_q6[Q6_LINK_LINES];
    bool trace;
    bool released;              /* resend the release when the DSP connects */
};

static void q6_link_to_q6(void *opaque, int n, int level)
{
    MsmQ6LinkState *s = opaque;
    uint8_t b = n;

    if (!level) {
        return;
    }
    if (s->trace) {
        qemu_log("q6-link: -> line %d\n", n);
    }
    qemu_chr_fe_write_all(&s->chr, &b, 1);
}

void msm_q6_link_boot(DeviceState *dev)
{
    MsmQ6LinkState *s = MSM_Q6_LINK(dev);
    uint8_t b = 0x7f;

    if (s->trace) {
        qemu_log("q6-link: -> release the QDSP6\n");
    }
    s->released = true;
    if (qemu_chr_fe_backend_open(&s->chr)) {
        qemu_chr_fe_write_all(&s->chr, &b, 1);
    }
}

static void q6_link_event(void *opaque, QEMUChrEvent event)
{
    MsmQ6LinkState *s = opaque;
    uint8_t b = 0x7f;

    /* the DSP's QEMU connected after the reset that released it */
    if (event == CHR_EVENT_OPENED && s->released) {
        qemu_chr_fe_write_all(&s->chr, &b, 1);
    }
}

static int q6_link_can_receive(void *opaque)
{
    return 64;
}

static void q6_link_receive(void *opaque, const uint8_t *buf, int size)
{
    MsmQ6LinkState *s = opaque;

    for (int i = 0; i < size; i++) {
        int n = buf[i] & 0x7F;

        if (s->trace) {
            qemu_log("q6-link: <- line %d\n", n);
        }
        if (n < Q6_LINK_LINES) {
            qemu_irq_pulse(s->from_q6[n]);
        }
    }
}

static void q6_link_realize(DeviceState *dev, Error **errp)
{
    MsmQ6LinkState *s = MSM_Q6_LINK(dev);

    qemu_chr_fe_set_handlers(&s->chr, q6_link_can_receive, q6_link_receive,
                             q6_link_event, NULL, s, NULL, true);
}

static void q6_link_init(Object *obj)
{
    MsmQ6LinkState *s = MSM_Q6_LINK(obj);

    qdev_init_gpio_in_named(DEVICE(obj), q6_link_to_q6, "to-q6",
                            Q6_LINK_LINES);
    qdev_init_gpio_out_named(DEVICE(obj), s->from_q6, "from-q6",
                             Q6_LINK_LINES);
}

static const Property q6_link_props[] = {
    DEFINE_PROP_CHR("chardev", MsmQ6LinkState, chr),
    DEFINE_PROP_BOOL("trace", MsmQ6LinkState, trace, false),
};

static void q6_link_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = q6_link_realize;
    device_class_set_props(dc, q6_link_props);
}

static const TypeInfo msm_q6_link_info = {
    .name = TYPE_MSM_Q6_LINK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmQ6LinkState),
    .instance_init = q6_link_init,
    .class_init = q6_link_class_init,
};

static void msm_q6_link_register(void)
{
    type_register_static(&msm_q6_link_info);
}
type_init(msm_q6_link_register)
