/*
 * QSD8250 UIM: the legacy UART at 0xA9B00000 in SIM mode, with a SIM card
 *
 * With baseband=lle AMSS's UIM driver (uimdrv.c) runs the smart card
 * through UART2: UART_SIM_CFG (0x3C) puts it in SIM mode, the M/N/D
 * registers (0x28-0x34) set the card clock and MR1/MR2 an 8E2 frame.
 * Registers (drivers/serial/msm_serial.h):
 *   0x00 MR1  0x04 MR2  0x08 SR (read) / CSR (write)  0x0C RF / TF
 *   0x10 MISR (read) / CR (write)  0x14 ISR (read) / IMR (write)
 *   0x18 IPR  0x1C TFWR  0x20 RFWR  0x24 HCR  0x3C SIM_CFG
 *   SR:  0 RXRDY  1 RXFULL  2 TXRDY  3 TXEMT
 *   ISR: 0 TXLEV  3 RXSTALE  4 RXLEV
 * The card answers with its ATR 2 ms after the receiver is enabled while
 * its clock runs (SIM_CFG bit 7: AMSS powers the card and releases its
 * reset through PMIC VREGs and MPPs, then enables RX and waits 40000
 * clocks for the ATR), then speaks T=0 (sim_card.c).  In SIM mode the
 * receiver also takes bytes without RX_EN, and as the card's I/O is a
 * single wire every transmitted byte is received back first.  Its bytes arrive one per 100 us; the stale
 * interrupt comes when they stop.  The interrupt goes to the modem's
 * second-level controller (msm_mtmr.c, sub-controller 7 bit 3).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/fifo8.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "htcleo.h"
#include "sim_card.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmUimState, MSM_UIM)

#define R_MR1       0x00
#define R_MR2       0x04
#define R_SR        0x08
#define R_RF        0x0C
#define R_CR        0x10
#define R_ISR       0x14
#define R_IPR       0x18
#define R_TFWR      0x1C
#define R_RFWR      0x20
#define R_SIM_CFG   0x3C

#define SR_RXRDY    0x01
#define SR_RXFULL   0x02
#define SR_TXRDY    0x04
#define SR_TXEMT    0x08

#define ISR_TXLEV   0x01
#define ISR_RXSTALE 0x08
#define ISR_RXLEV   0x10

#define CR_RX_EN    0x01
#define CR_RX_DIS   0x02
#define CR_TX_EN    0x04
#define CR_TX_DIS   0x08
#define CR_CMD(v)   ((v) & 0xF0)
#define CMD_RESET_RX    0x10
#define CMD_RESET_TX    0x20
#define CMD_RESET_STALE 0x80

#define SIM_CFG_SEL     0x01
#define SIM_CFG_CLK_ON  0x80    /* card clock running (power-up) */
#define ATR_DELAY_NS    2000000     /* after uim_reset enables the receiver */

#define RX_FIFO     64
#define BYTE_NS     100000      /* one card byte every 100 us */

struct MsmUimState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;
    SimCard *card;
    Fifo8 rx;
    GByteArray *pending;        /* card bytes still on the wire */

    uint32_t regs[0x40];
    uint32_t imr;
    bool rx_en, stale;
    bool trace;
};

static void uim_update(MsmUimState *s)
{
    uint32_t isr = ISR_TXLEV;

    if (fifo8_num_used(&s->rx) > s->regs[R_RFWR >> 2]) {
        isr |= ISR_RXLEV;
    }
    /* stale: data left in the FIFO after the line went quiet */
    if (s->stale && !fifo8_is_empty(&s->rx)) {
        isr |= ISR_RXSTALE;
    } else {
        s->stale = false;
    }
    s->regs[R_ISR >> 2] = isr;
    qemu_set_irq(s->irq, !!(isr & s->imr));
}

static void uim_tick(void *opaque)
{
    MsmUimState *s = opaque;

    if (s->pending->len) {
        /* in SIM mode the receiver listens without RX_EN */
        if (!fifo8_is_full(&s->rx)) {
            fifo8_push(&s->rx, s->pending->data[0]);
        }
        g_byte_array_remove_index(s->pending, 0);
        if (s->pending->len) {
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      BYTE_NS);
        } else {
            /* the line went quiet: stale after a few character times */
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      4 * BYTE_NS);
        }
    } else if (!fifo8_is_empty(&s->rx)) {
        s->stale = true;
    }
    uim_update(s);
}

static void uim_send(MsmUimState *s, const uint8_t *buf, int len)
{
    if (s->trace) {
        g_autoptr(GString) h = g_string_new(NULL);
        for (int i = 0; i < len; i++) {
            g_string_append_printf(h, " %02x", buf[i]);
        }
        qemu_log("uim: card ->%s\n", h->str);
    }
    if (!len) {
        return;
    }
    g_byte_array_append(s->pending, buf, len);
    if (!timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + BYTE_NS);
    }
}

static uint64_t uim_read(void *opaque, hwaddr off, unsigned size)
{
    MsmUimState *s = opaque;
    uint32_t v;

    switch (off) {
    case R_SR:
        v = SR_TXRDY | SR_TXEMT;
        if (!fifo8_is_empty(&s->rx)) {
            v |= SR_RXRDY;
        }
        if (fifo8_is_full(&s->rx)) {
            v |= SR_RXFULL;
        }
        return v;
    case R_RF:
        v = fifo8_is_empty(&s->rx) ? 0 : fifo8_pop(&s->rx);
        uim_update(s);
        return v;
    case R_CR:                  /* MISR */
        return s->regs[R_ISR >> 2] & s->imr;
    case R_ISR:
        return s->regs[R_ISR >> 2];
    }
    return s->regs[(off >> 2) & 0x3F];
}

static void uim_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmUimState *s = opaque;

    if (s->trace && off != R_RF) {
        qemu_log("uim: [%02x] <- %08x\n", (unsigned)off, (unsigned)val);
    }
    switch (off) {
    case R_RF: {                /* TF: a byte to the card */
        g_autoptr(GByteArray) out = g_byte_array_new();
        if (s->trace) {
            qemu_log("uim: card <- %08x (size %u)\n", (unsigned)val, size);
        }
        /* one wire: the UART hears its own byte before the card answers */
        g_byte_array_append(out, (const uint8_t *)&(uint8_t){ val }, 1);
        sim_card_rx(s->card, val, out);
        uim_send(s, out->data, out->len);
        uim_update(s);
        return;
    }
    case R_CR:
        if ((val & CR_RX_EN) && !s->rx_en &&
            (s->regs[R_SIM_CFG >> 2] & (SIM_CFG_SEL | SIM_CFG_CLK_ON)) ==
            (SIM_CFG_SEL | SIM_CFG_CLK_ON)) {
            int len;
            const uint8_t *atr = sim_card_reset(s->card, &len);
            g_byte_array_set_size(s->pending, 0);
            g_byte_array_append(s->pending, atr, len);
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      ATR_DELAY_NS);
        }
        if (val & CR_RX_EN) {
            s->rx_en = true;
        }
        if (val & CR_RX_DIS) {
            s->rx_en = false;
        }
        switch (CR_CMD(val)) {
        case CMD_RESET_RX:
            fifo8_reset(&s->rx);
            s->stale = false;
            break;
        case CMD_RESET_STALE:
            s->stale = false;
            break;
        }
        uim_update(s);
        return;
    case R_ISR:                 /* IMR */
        s->imr = val;
        uim_update(s);
        return;
    case R_SIM_CFG: {
        s->regs[R_SIM_CFG >> 2] = val;
        if (!(val & SIM_CFG_CLK_ON)) {
            s->rx_en = false;               /* powered down */
        }
        return;
    }
    }
    s->regs[(off >> 2) & 0x3F] = val;
    if (off == R_RFWR) {
        uim_update(s);
    }
}

static const MemoryRegionOps uim_ops = {
    .read = uim_read,
    .write = uim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void uim_reset(DeviceState *dev)
{
    MsmUimState *s = MSM_UIM(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->imr = 0;
    s->rx_en = false;
    s->stale = false;
    fifo8_reset(&s->rx);
    g_byte_array_set_size(s->pending, 0);
    timer_del(s->timer);
    uim_update(s);
}

static void uim_init(Object *obj)
{
    MsmUimState *s = MSM_UIM(obj);

    memory_region_init_io(&s->iomem, obj, &uim_ops, s, "msm-uim", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    fifo8_create(&s->rx, RX_FIFO);
    s->pending = g_byte_array_new();
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, uim_tick, s);
    s->card = sim_card_new();
}

static const Property uim_props[] = {
    DEFINE_PROP_BOOL("trace", MsmUimState, trace, false),
};

static void uim_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, uim_reset);
    device_class_set_props(dc, uim_props);
}

static const TypeInfo msm_uim_info = {
    .name = TYPE_MSM_UIM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmUimState),
    .instance_init = uim_init,
    .class_init = uim_class_init,
};

static void msm_uim_register(void)
{
    type_register_static(&msm_uim_info);
}
type_init(msm_uim_register)
