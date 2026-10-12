/*
 * Qualcomm MSM legacy UART (UART1..3 at 0xA9A00000..0xA9C00000)
 *
 * Register map from drivers/serial/msm_serial.h:
 *   0x00 MR1  0x04 MR2  0x08 SR(read)/CSR(write)  0x0C RF(read)/TF(write)
 *   0x10 CR   0x14 ISR(read)/IMR(write)  0x18 IPR  0x1C TFWR  0x20 RFWR
 *   0x24 HCR
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/fifo8.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmUartState, MSM_UART)

#define SR_RXRDY        0x01
#define SR_RXFULL       0x02
#define SR_TXRDY        0x04
#define SR_TXEMT        0x08

#define ISR_TXLEV       0x01
#define ISR_RXSTALE     0x08
#define ISR_RXLEV       0x10

#define CR_CMD_MASK     0xF0
#define CR_CMD_RESET_RX         0x10
#define CR_CMD_RESET_TX         0x20
#define CR_CMD_RESET_ERR        0x30
#define CR_CMD_RESET_STALE_INT  0x80

#define RX_FIFO 512

struct MsmUartState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    CharFrontend chr;
    Fifo8 rx;

    uint32_t mr1, mr2, csr, cr, imr, ipr, tfwr, rfwr, hcr;
    uint32_t regs[0x400];
    bool stale;
};

static uint32_t uart_isr(MsmUartState *s)
{
    uint32_t isr = ISR_TXLEV;       /* the TX FIFO is always empty */

    if (fifo8_num_used(&s->rx) > s->rfwr) {
        isr |= ISR_RXLEV;
    }
    if (!fifo8_is_empty(&s->rx) && s->stale) {
        isr |= ISR_RXSTALE;
    }
    return isr;
}

static void uart_update(MsmUartState *s)
{
    qemu_set_irq(s->irq, !!(uart_isr(s) & s->imr));
}

static uint64_t uart_read(void *opaque, hwaddr off, unsigned size)
{
    MsmUartState *s = opaque;
    uint32_t v;

    switch (off) {
    case 0x00: return s->mr1;
    case 0x04: return s->mr2;
    case 0x08:
        v = SR_TXRDY | SR_TXEMT;
        if (!fifo8_is_empty(&s->rx)) {
            v |= SR_RXRDY;
        }
        if (fifo8_is_full(&s->rx)) {
            v |= SR_RXFULL;
        }
        return v;
    case 0x0C:
        v = fifo8_is_empty(&s->rx) ? 0 : fifo8_pop(&s->rx);
        if (fifo8_is_empty(&s->rx)) {
            s->stale = false;
        }
        qemu_chr_fe_accept_input(&s->chr);
        uart_update(s);
        return v;
    case 0x10: return s->cr;
    case 0x14: return uart_isr(s);
    case 0x18: return s->ipr;
    case 0x1C: return s->tfwr;
    case 0x20: return s->rfwr;
    case 0x24: return s->hcr;
    }
    return s->regs[(off >> 2) & 0x3ff];
}

static void uart_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmUartState *s = opaque;
    uint8_t ch;

    switch (off) {
    case 0x00: s->mr1 = val; break;
    case 0x04: s->mr2 = val; break;
    case 0x08: s->csr = val; break;
    case 0x0C:
        ch = val;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;
    case 0x10:
        s->cr = val;
        switch (val & CR_CMD_MASK) {
        case CR_CMD_RESET_RX:
            fifo8_reset(&s->rx);
            break;
        case CR_CMD_RESET_STALE_INT:
            s->stale = false;
            break;
        }
        break;
    case 0x14: s->imr = val; break;
    case 0x18: s->ipr = val; break;
    case 0x1C: s->tfwr = val; break;
    case 0x20: s->rfwr = val; break;
    case 0x24: s->hcr = val; break;
    default:
        s->regs[(off >> 2) & 0x3ff] = val;
        break;
    }
    uart_update(s);
}

static const MemoryRegionOps uart_ops = {
    .read = uart_read,
    .write = uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static int uart_can_receive(void *opaque)
{
    MsmUartState *s = opaque;
    return fifo8_num_free(&s->rx);
}

static void uart_receive(void *opaque, const uint8_t *buf, int size)
{
    MsmUartState *s = opaque;

    for (int i = 0; i < size && !fifo8_is_full(&s->rx); i++) {
        fifo8_push(&s->rx, buf[i]);
    }
    s->stale = true;
    uart_update(s);
}

static void uart_reset(DeviceState *dev)
{
    MsmUartState *s = MSM_UART(dev);

    fifo8_reset(&s->rx);
    s->imr = 0;
    s->rfwr = 0;
    s->stale = false;
    uart_update(s);
}

static void uart_realize(DeviceState *dev, Error **errp)
{
    MsmUartState *s = MSM_UART(dev);

    fifo8_create(&s->rx, RX_FIFO);
    qemu_chr_fe_set_handlers(&s->chr, uart_can_receive, uart_receive,
                             NULL, NULL, s, NULL, true);
}

static void uart_init(Object *obj)
{
    MsmUartState *s = MSM_UART(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &uart_ops, s, "msm-uart", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property uart_props[] = {
    DEFINE_PROP_CHR("chardev", MsmUartState, chr),
};

static void uart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = uart_realize;
    device_class_set_legacy_reset(dc, uart_reset);
    device_class_set_props(dc, uart_props);
}

static const TypeInfo msm_uart_info = {
    .name = TYPE_MSM_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmUartState),
    .instance_init = uart_init,
    .class_init = uart_class_init,
};

static void msm_uart_register(void)
{
    type_register_static(&msm_uart_info);
}
type_init(msm_uart_register)
