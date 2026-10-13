/*
 * Qualcomm UART DM (high-speed UART with data mover), as driven by
 * drivers/serial/msm_serial_hs.c.  On the HD2, UART1DM carries the BCM4329
 * Bluetooth HCI.
 *
 * Register map (msm_serial_hs_hwreg.h; reads and writes differ):
 *   0x00 MR1   0x04 MR2   0x08 CSR (w) / SR (r)
 *   0x10 CR (w) / MISR (r)            0x14 IMR (w) / ISR (r)
 *   0x18 IPR   0x1c TFWR  0x20 RFWR   0x24 HCR
 *   0x34 DMRX  0x38 IRDA (w) / RX_TOTAL_SNAP (r)   0x3c DMEN
 *   0x40 NCF_TX           0x44 BADR   0x50 RXFS
 *   0x70..0x7c TF (w) / RF (r)
 *
 * Transmit: the driver programs NCF_TX with the byte count and has the ADM
 * copy the bytes into TF; once all NCF_TX bytes arrived TX_READY and TXLEV
 * are raised and the bytes go to the peer.
 *
 * Receive: the driver writes DMRX, enables the stale event and queues an ADM
 * box command reading RF.  Bytes from the peer are pushed into that command
 * (msm_adm_rx_push); a short idle period later RXSTALE is raised, the driver
 * flushes the command and reads RX_TOTAL_SNAP.  While no receive command is
 * queued (the port is closed or its clock is off) the peer's bytes wait
 * here, as they would wait in the peer behind RFR flow control.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmUartDmState, MSM_UARTDM)

#define R_MR1       0x00
#define R_MR2       0x04
#define R_SR_CSR    0x08
#define R_MISR_CR   0x10
#define R_ISR_IMR   0x14
#define R_IPR       0x18
#define R_TFWR      0x1c
#define R_RFWR      0x20
#define R_HCR       0x24
#define R_DMRX      0x34
#define R_SNAP_IRDA 0x38
#define R_DMEN      0x3c
#define R_NCF_TX    0x40
#define R_BADR      0x44
#define R_RXFS      0x50
#define R_FIFO      0x70        /* TF/RF and their aliases up to 0x7c */

#define SR_RXRDY    (1u << 0)
#define SR_TXRDY    (1u << 2)
#define SR_TXEMT    (1u << 3)

#define ISR_TXLEV       (1u << 0)
#define ISR_RXSTALE     (1u << 3)
#define ISR_RXLEV       (1u << 4)
#define ISR_CURRENT_CTS (1u << 6)
#define ISR_TX_READY    (1u << 7)

#define DMEN_RX     (1u << 1)

/* CR channel commands (bits 7:4, bit 11 extends) and general commands */
#define CH_RESET_RX         1
#define CH_RESET_TX         2
#define CH_RESET_STALE      8
#define GEN_CLEAR_TX_READY  3
#define GEN_FORCE_STALE     4
#define GEN_STALE_ENABLE    5
#define GEN_STALE_DISABLE   6

/* idle time on the line after which a partial receive is handed over */
#define STALE_NS    (200 * SCALE_US)
#define RX_MAX      (64 * 1024)

struct MsmUartDmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    DeviceState *adm;
    uint32_t base;

    uint32_t mr1, mr2, csr, ipr, tfwr, rfwr, hcr, dmrx, irda, dmen, badr;
    uint32_t imr, isr;          /* latched ISR bits: TX_READY, RXSTALE, RXLEV */
    bool stale_en;
    uint32_t snap;              /* bytes delivered in this receive */
    uint32_t ncf_tx;            /* bytes still expected in this transmit */
    bool txlev;                 /* the TX FIFO drained after a transmit */
    GByteArray *rx;             /* bytes from the peer not yet delivered */
    GByteArray *tx;
    QEMUTimer *stale_timer;

    MsmUartDmTx peer_tx;
    void *peer;
};

/*
 * TXLEV: the TX FIFO has drained to its watermark (TFWR) after a transmit;
 * the FIFO empties at once here, so it comes with the end of each transmit
 * and goes with the start of the next.  It does not stand while the port
 * is idle: Windows Mobile's driver unmasks it as it opens the port, with
 * nothing to send, and its interrupt would never stop.
 */
static uint32_t uartdm_isr(MsmUartDmState *s)
{
    return s->isr | (s->txlev ? ISR_TXLEV : 0);
}

static void uartdm_update(MsmUartDmState *s)
{
    /* CURRENT_CTS only mirrors the line; it does not interrupt */
    qemu_set_irq(s->irq, !!(uartdm_isr(s) & s->imr & ~ISR_CURRENT_CTS));
}

static void uartdm_rx_pump(MsmUartDmState *s)
{
    int n;

    if (!s->rx->len || !(s->dmen & DMEN_RX) || !s->adm) {
        return;
    }
    n = msm_adm_rx_push(s->adm, s->base + R_FIFO, 0x10, s->rx->data,
                        MIN(s->rx->len, s->dmrx > s->snap ?
                            s->dmrx - s->snap : 0));
    if (n <= 0) {
        return;
    }
    g_byte_array_remove_range(s->rx, 0, n);
    s->snap += n;
    s->isr |= ISR_RXLEV;
    timer_mod(s->stale_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              STALE_NS);
    uartdm_update(s);
}

static void uartdm_stale(void *opaque)
{
    MsmUartDmState *s = opaque;

    if (s->stale_en && s->snap) {
        s->isr |= ISR_RXSTALE;
        uartdm_update(s);
    }
}

static void uartdm_adm_ready(void *opaque)
{
    uartdm_rx_pump(opaque);
}

void msm_uartdm_receive(DeviceState *dev, const uint8_t *buf, int len)
{
    MsmUartDmState *s = MSM_UARTDM(dev);

    if (s->rx->len + len > RX_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "msm-uartdm: receive overrun\n");
        return;
    }
    g_byte_array_append(s->rx, buf, len);
    uartdm_rx_pump(s);
}

void msm_uartdm_set_peer(DeviceState *dev, MsmUartDmTx tx, void *opaque)
{
    MsmUartDmState *s = MSM_UARTDM(dev);

    s->peer_tx = tx;
    s->peer = opaque;
}

static void uartdm_tx_word(MsmUartDmState *s, uint32_t v)
{
    for (int i = 0; i < 4 && s->ncf_tx; i++, s->ncf_tx--) {
        uint8_t b = v >> (8 * i);
        g_byte_array_append(s->tx, &b, 1);
    }
    if (!s->ncf_tx && s->tx->len) {
        if (s->peer_tx) {
            s->peer_tx(s->peer, s->tx->data, s->tx->len);
        }
        g_byte_array_set_size(s->tx, 0);
        s->isr |= ISR_TX_READY;
        s->txlev = true;
        uartdm_update(s);
    }
}

static void uartdm_cr(MsmUartDmState *s, uint32_t v)
{
    unsigned ch = ((v >> 4) & 0xF) | (((v >> 11) & 1) << 4);
    unsigned gen = (v >> 8) & 7;

    switch (ch) {
    case CH_RESET_RX:
        s->snap = 0;
        s->isr &= ~(ISR_RXSTALE | ISR_RXLEV);
        break;
    case CH_RESET_TX:
        s->ncf_tx = 0;
        s->txlev = false;
        g_byte_array_set_size(s->tx, 0);
        break;
    case CH_RESET_STALE:
        s->isr &= ~ISR_RXSTALE;
        break;
    }
    switch (gen) {
    case GEN_CLEAR_TX_READY:
        s->isr &= ~ISR_TX_READY;
        break;
    case GEN_FORCE_STALE:
        s->isr |= ISR_RXSTALE;
        break;
    case GEN_STALE_ENABLE:
        s->stale_en = true;
        if (s->snap) {
            timer_mod(s->stale_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      STALE_NS);
        }
        break;
    case GEN_STALE_DISABLE:
        s->stale_en = false;
        break;
    }
    uartdm_update(s);
}

static uint64_t uartdm_read(void *opaque, hwaddr off, unsigned size)
{
    MsmUartDmState *s = opaque;

    switch (off) {
    case R_MR1:
        return s->mr1;
    case R_MR2:
        return s->mr2;
    case R_SR_CSR:
        return SR_TXEMT | SR_TXRDY |
               (s->rx->len && !(s->dmen & DMEN_RX) ? SR_RXRDY : 0);
    case R_MISR_CR:
        return uartdm_isr(s) & s->imr;
    case R_ISR_IMR:
        return uartdm_isr(s);
    case R_IPR:
        return s->ipr;
    case R_TFWR:
        return s->tfwr;
    case R_RFWR:
        return s->rfwr;
    case R_HCR:
        return s->hcr;
    case R_DMRX:
        return s->dmrx;
    case R_SNAP_IRDA:
        return s->snap;
    case R_DMEN:
        return s->dmen;
    case R_NCF_TX:
        return s->ncf_tx;
    case R_BADR:
        return s->badr;
    case R_RXFS:
        return 0;
    case R_FIFO ... R_FIFO + 0xF: {
        /* programmed I/O receive, only used without the data mover */
        uint32_t v = 0;
        int n = MIN(s->rx->len, 4);
        for (int i = 0; i < n; i++) {
            v |= s->rx->data[i] << (8 * i);
        }
        g_byte_array_remove_range(s->rx, 0, n);
        return v;
    }
    }
    qemu_log_mask(LOG_UNIMP, "msm-uartdm: read 0x%" HWADDR_PRIx "\n", off);
    return 0;
}

static void uartdm_write(void *opaque, hwaddr off, uint64_t val,
                         unsigned size)
{
    MsmUartDmState *s = opaque;

    switch (off) {
    case R_MR1:
        s->mr1 = val;
        break;
    case R_MR2:
        s->mr2 = val;
        break;
    case R_SR_CSR:
        s->csr = val;
        break;
    case R_MISR_CR:
        uartdm_cr(s, val);
        break;
    case R_ISR_IMR:
        s->imr = val;
        uartdm_update(s);
        break;
    case R_IPR:
        s->ipr = val;
        break;
    case R_TFWR:
        s->tfwr = val;
        break;
    case R_RFWR:
        s->rfwr = val;
        break;
    case R_HCR:
        s->hcr = val;
        break;
    case R_DMRX:
        /* a new receive transfer of up to val bytes */
        s->dmrx = val;
        s->snap = 0;
        s->isr &= ~ISR_RXLEV;
        uartdm_update(s);
        break;
    case R_SNAP_IRDA:
        s->irda = val;
        break;
    case R_DMEN:
        s->dmen = val;
        uartdm_rx_pump(s);
        break;
    case R_NCF_TX:
        s->ncf_tx = val;
        s->txlev = false;
        g_byte_array_set_size(s->tx, 0);
        break;
    case R_BADR:
        s->badr = val;
        break;
    case R_FIFO ... R_FIFO + 0xF:
        uartdm_tx_word(s, val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "msm-uartdm: write 0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", off, val);
    }
}

static const MemoryRegionOps uartdm_ops = {
    .read = uartdm_read,
    .write = uartdm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void uartdm_reset(DeviceState *dev)
{
    MsmUartDmState *s = MSM_UARTDM(dev);

    s->mr1 = s->mr2 = s->csr = s->ipr = s->tfwr = s->rfwr = s->hcr = 0;
    s->dmrx = s->irda = s->dmen = s->badr = 0;
    s->imr = s->isr = 0;
    s->stale_en = false;
    s->snap = s->ncf_tx = 0;
    s->txlev = false;
    g_byte_array_set_size(s->rx, 0);
    g_byte_array_set_size(s->tx, 0);
    timer_del(s->stale_timer);
    uartdm_update(s);
}

static void uartdm_realize(DeviceState *dev, Error **errp)
{
    MsmUartDmState *s = MSM_UARTDM(dev);

    if (s->adm) {
        msm_adm_set_rx_source(s->adm, s->base + R_FIFO, 0x10,
                              uartdm_adm_ready, s);
    }
}

static void uartdm_init(Object *obj)
{
    MsmUartDmState *s = MSM_UARTDM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &uartdm_ops, s, "msm-uartdm",
                          0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->rx = g_byte_array_new();
    s->tx = g_byte_array_new();
    s->stale_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, uartdm_stale, s);
}

static const Property uartdm_props[] = {
    DEFINE_PROP_LINK("adm", MsmUartDmState, adm, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT32("base", MsmUartDmState, base, 0),
};

static void uartdm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = uartdm_realize;
    device_class_set_legacy_reset(dc, uartdm_reset);
    device_class_set_props(dc, uartdm_props);
}

static const TypeInfo msm_uartdm_info = {
    .name = TYPE_MSM_UARTDM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmUartDmState),
    .instance_init = uartdm_init,
    .class_init = uartdm_class_init,
};

static void msm_uartdm_register(void)
{
    type_register_static(&msm_uartdm_info);
}
type_init(msm_uartdm_register)
