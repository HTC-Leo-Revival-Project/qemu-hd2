/*
 * Qualcomm MSM legacy I2C controller (0xA9900000), as driven by
 * drivers/i2c/busses/i2c-msm.c.
 *
 *   0x00 WRITE_DATA   bits 7:0 data, bit 8 address byte, bit 9 last byte
 *   0x04 CLK_CTL
 *   0x08 STATUS       bit0 WR_BUFFER_FULL, bit1 RD_BUFFER_FULL, bit3 NACK,
 *                     bits 7:6 FAILED, bit8 BUS_ACTIVE, bit9 BUS_MASTER
 *   0x0C READ_DATA
 *   0x10 INTERFACE_SELECT (bit8 SCL, bit9 SDA line levels)
 *
 * The driver is interrupt driven: it reads STATUS, then feeds or drains one
 * byte per interrupt.  Bytes move instantly here, so the interrupt is raised
 * after every byte-level action and dropped when STATUS is read.
 *
 * Reads: the byte after the address is fetched immediately; writing
 * LAST_BYTE during a read means "the next byte is the final one" (the
 * driver does this when two bytes remain), after which the transfer stops.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmI2cState, MSM_I2C)

#define I2C_WRITE_DATA      0x00
#define I2C_CLK_CTL         0x04
#define I2C_STATUS          0x08
#define I2C_READ_DATA       0x0C
#define I2C_INTERFACE_SELECT 0x10

#define WD_ADDR_BYTE        (1u << 8)
#define WD_LAST_BYTE        (1u << 9)

#define ST_WR_FULL          (1u << 0)
#define ST_RD_FULL          (1u << 1)
#define ST_NACKED           (1u << 3)
#define ST_FAILED           (3u << 6)
#define ST_BUS_ACTIVE       (1u << 8)
#define ST_BUS_MASTER       (1u << 9)

struct MsmI2cState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;

    uint32_t clk_ctl, intf_sel;
    bool active, reading, error;
    bool rd_full, last_pending, final_byte;
    uint8_t rd_data;
    bool event;
};

static void i2c_update(MsmI2cState *s)
{
    qemu_set_irq(s->irq, s->event);
}

static uint32_t i2c_status(MsmI2cState *s)
{
    uint32_t st = 0;

    if (s->rd_full) {
        st |= ST_RD_FULL;
    }
    if (s->active) {
        st |= ST_BUS_ACTIVE | ST_BUS_MASTER;
    }
    if (s->error) {
        st |= ST_NACKED | ST_FAILED;
    }
    return st;
}

static void i2c_stop(MsmI2cState *s)
{
    if (s->active) {
        i2c_end_transfer(s->bus);
    }
    s->active = false;
    s->reading = false;
    s->last_pending = false;
    s->final_byte = false;
}

static void i2c_fetch(MsmI2cState *s)
{
    s->rd_data = i2c_recv(s->bus);
    s->rd_full = true;
}

static void i2c_write_data(MsmI2cState *s, uint32_t val)
{
    if (val & WD_ADDR_BYTE) {
        uint8_t addr = (val >> 1) & 0x7F;
        bool rd = val & 1;

        s->error = false;
        s->rd_full = false;
        s->last_pending = false;
        s->final_byte = false;
        qemu_log_mask(LOG_TRACE, "msm-i2c: start 0x%02x %s\n", addr,
                      rd ? "rd" : "wr");
        if (i2c_start_transfer(s->bus, addr, rd)) {
            /* nobody answered */
            if (s->active) {
                i2c_end_transfer(s->bus);
            }
            s->active = false;
            s->error = true;
        } else {
            s->active = true;
            s->reading = rd;
            if (rd) {
                i2c_fetch(s);
            } else if (val & WD_LAST_BYTE) {
                i2c_stop(s);
            }
        }
    } else if (s->active && !s->reading) {
        qemu_log_mask(LOG_TRACE, "msm-i2c: send 0x%02x%s\n", val & 0xFF,
                      (val & WD_LAST_BYTE) ? " (stop)" : "");
        if (i2c_send(s->bus, val & 0xFF)) {
            s->error = true;
            i2c_stop(s);
        } else if (val & WD_LAST_BYTE) {
            i2c_stop(s);
        }
    } else if (s->active && s->reading) {
        if (val & WD_LAST_BYTE) {
            s->last_pending = true;
        }
    }
    s->event = true;
    i2c_update(s);
}

static uint32_t i2c_read_data(MsmI2cState *s)
{
    uint8_t v = s->rd_data;

    if (!s->rd_full) {
        return 0;
    }
    s->rd_full = false;
    if (s->active && s->reading) {
        if (s->final_byte) {
            i2c_stop(s);
        } else {
            if (s->last_pending) {
                s->final_byte = true;
                s->last_pending = false;
            }
            i2c_fetch(s);
        }
    }
    s->event = true;
    i2c_update(s);
    return v;
}

static uint64_t i2c_mmio_read(void *opaque, hwaddr off, unsigned size)
{
    MsmI2cState *s = opaque;
    uint32_t v;

    switch (off) {
    case I2C_CLK_CTL:
        return s->clk_ctl;
    case I2C_STATUS:
        v = i2c_status(s);
        /* error flags are reported once, then cleared */
        s->error = false;
        s->event = false;
        i2c_update(s);
        return v;
    case I2C_READ_DATA:
        return i2c_read_data(s);
    case I2C_INTERFACE_SELECT:
        /* bus idle: SCL and SDA released high */
        return (s->intf_sel & 1) | (1u << 8) | (1u << 9);
    }
    return 0;
}

static void i2c_mmio_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    MsmI2cState *s = opaque;

    switch (off) {
    case I2C_WRITE_DATA:
        i2c_write_data(s, val);
        break;
    case I2C_CLK_CTL:
        s->clk_ctl = val;
        break;
    case I2C_INTERFACE_SELECT:
        s->intf_sel = val;
        break;
    }
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_mmio_read,
    .write = i2c_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm_i2c_reset(DeviceState *dev)
{
    MsmI2cState *s = MSM_I2C(dev);

    i2c_stop(s);
    s->error = false;
    s->rd_full = false;
    s->event = false;
    i2c_update(s);
}

static void msm_i2c_init(Object *obj)
{
    MsmI2cState *s = MSM_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &i2c_ops, s, "msm-i2c", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static void msm_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, msm_i2c_reset);
}

static const TypeInfo msm_i2c_info = {
    .name = TYPE_MSM_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmI2cState),
    .instance_init = msm_i2c_init,
    .class_init = msm_i2c_class_init,
};

static void msm_i2c_register(void)
{
    type_register_static(&msm_i2c_info);
}
type_init(msm_i2c_register)
