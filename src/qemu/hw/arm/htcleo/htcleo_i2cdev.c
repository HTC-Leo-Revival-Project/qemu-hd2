/*
 * HTC HD2 I2C peripherals on the MSM I2C bus
 *
 *  0x15  touchscreen controller, "type 2A" (board-htcleo-ts.c)
 *  0x66  HTC microP (atmega): LEDs, LCD backlight, light/proximity sensor,
 *        BMA150 G-sensor, button backlight (board-htcleo-microp.c)
 *  0x48  TI TPS65023 PMIC (CPU core voltage)
 *  0x26  Maxim DS2746 battery gauge (drivers/power/ds2746_*.c)
 *  0x1C  AKM8973 compass: a measurement (MS1 = 0) completes 10 ms later
 *        and raises INT (GPIO 39, level high) until ST is read; then the
 *        part is back in power-down.  The field is a constant one.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "htcleo.h"
#include "htcleo_i2cdev.h"

/* ------------------------------------------------------------------ */
/* Generic 8-bit register device with auto-incrementing pointer        */
/* ------------------------------------------------------------------ */

#define TYPE_HTCLEO_REGDEV "htcleo-regdev"
OBJECT_DECLARE_SIMPLE_TYPE(HtcleoRegdev, HTCLEO_REGDEV)

struct HtcleoRegdev {
    I2CSlave parent_obj;
    uint8_t regs[256];
    uint8_t ptr;
    bool have_ptr;
};

static int regdev_event(I2CSlave *i2c, enum i2c_event ev)
{
    HtcleoRegdev *s = HTCLEO_REGDEV(i2c);

    if (ev == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static int regdev_send(I2CSlave *i2c, uint8_t data)
{
    HtcleoRegdev *s = HTCLEO_REGDEV(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
    } else {
        s->regs[s->ptr++] = data;
    }
    return 0;
}

static uint8_t regdev_recv(I2CSlave *i2c)
{
    HtcleoRegdev *s = HTCLEO_REGDEV(i2c);
    return s->regs[s->ptr++];
}

static void regdev_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = regdev_event;
    k->send = regdev_send;
    k->recv = regdev_recv;
}

I2CSlave *htcleo_regdev_create(I2CBus *bus, uint8_t addr,
                               const uint8_t *init, int n)
{
    I2CSlave *d = i2c_slave_create_simple(bus, TYPE_HTCLEO_REGDEV, addr);
    HtcleoRegdev *s = HTCLEO_REGDEV(d);

    for (int i = 0; i + 1 < n; i += 2) {
        s->regs[init[i]] = init[i + 1];
    }
    return d;
}

/* ------------------------------------------------------------------ */
/* Touchscreen (type 2A)                                               */
/* ------------------------------------------------------------------ */

OBJECT_DECLARE_SIMPLE_TYPE(HtcleoTs, HTCLEO_TS)

struct HtcleoTs {
    I2CSlave parent_obj;
    qemu_irq irq;           /* GPIO 92, active low */
    bool powered;
    bool enabled;           /* got the D0 00 01 enable command */
    uint8_t cmd[4];
    int cmd_len;
    bool data_pending;
    bool down;
    bool was_down;
    uint16_t x, y;
    uint8_t buf[9];
    int pos;
    QEMUTimer *repeat;
};

static void ts_update_irq(HtcleoTs *s)
{
    /* the line is pulled low while a report waits to be read */
    qemu_set_irq(s->irq, !(s->powered && s->data_pending));
}

static void ts_build_report(HtcleoTs *s)
{
    memset(s->buf, 0, sizeof(s->buf));
    s->buf[0] = 0x5A;
    if (s->down) {
        s->buf[1] = ((s->x >> 8) & 0xF) << 4 | ((s->y >> 8) & 0xF);
        s->buf[2] = s->x & 0xFF;
        s->buf[3] = s->y & 0xFF;
        s->buf[8] = 1 << 1;            /* one point */
    }
}

static int ts_event(I2CSlave *i2c, enum i2c_event ev)
{
    HtcleoTs *s = HTCLEO_TS(i2c);

    if (ev == I2C_START_SEND) {
        s->cmd_len = 0;
        s->pos = 0;
    } else if (ev == I2C_FINISH && s->cmd_len) {
        if (s->cmd_len == 3 && s->cmd[0] == 0xD0 && s->cmd[1] == 0x00 &&
            s->cmd[2] == 0x01) {
            s->enabled = true;          /* start reporting */
        }
        s->cmd_len = 0;
    }
    if (ev == I2C_START_RECV) {
        s->pos = 0;
        if (!s->enabled) {
            /* until it is enabled the controller answers with its ID */
            memset(s->buf, 0, sizeof(s->buf));
            s->buf[0] = 0x55;
        } else {
            /* the current contact state; no contact is a valid report */
            ts_build_report(s);
        }
    } else if (ev == I2C_FINISH && s->pos > 0) {
        s->data_pending = false;
        s->was_down = s->down;
        ts_update_irq(s);
        if (s->down) {
            /* the controller keeps reporting while a finger is down */
            timer_mod(s->repeat, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      20 * SCALE_MS);
        }
    }
    return s->powered ? 0 : -1;
}

static int ts_send(I2CSlave *i2c, uint8_t data)
{
    HtcleoTs *s = HTCLEO_TS(i2c);

    if (s->cmd_len < (int)sizeof(s->cmd)) {
        s->cmd[s->cmd_len++] = data;    /* the enable command is D0 00 01 */
    }
    return 0;
}

static uint8_t ts_recv(I2CSlave *i2c)
{
    HtcleoTs *s = HTCLEO_TS(i2c);
    uint8_t v = s->pos < (int)sizeof(s->buf) ? s->buf[s->pos] : 0;

    s->pos++;
    return v;
}

static void ts_repeat(void *opaque)
{
    HtcleoTs *s = opaque;

    if (s->down || s->was_down) {
        s->data_pending = true;
        ts_update_irq(s);
    }
}

void htcleo_ts_set_power(I2CSlave *d, bool on)
{
    HtcleoTs *s = HTCLEO_TS(d);

    if (on && !s->powered) {
        /* controller signals readiness by pulling its IRQ low */
        s->data_pending = false;
        s->powered = true;
        qemu_set_irq(s->irq, 0);
        return;
    }
    s->powered = on;
    if (!on) {
        s->data_pending = false;
        s->enabled = false;
    }
    ts_update_irq(s);
}

void htcleo_ts_touch(I2CSlave *d, bool down, int x, int y)
{
    HtcleoTs *s = HTCLEO_TS(d);

    s->down = down;
    s->x = x;
    s->y = y;
    s->data_pending = true;
    ts_update_irq(s);
}

static void ts_realize(DeviceState *dev, Error **errp)
{
    HtcleoTs *s = HTCLEO_TS(dev);
    s->repeat = timer_new_ns(QEMU_CLOCK_VIRTUAL, ts_repeat, s);
}

static void ts_init(Object *obj)
{
    HtcleoTs *s = HTCLEO_TS(obj);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq, "irq", 1);
}

static void ts_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ts_realize;
    k->event = ts_event;
    k->send = ts_send;
    k->recv = ts_recv;
}

/* ------------------------------------------------------------------ */
/* microP                                                              */
/* ------------------------------------------------------------------ */

OBJECT_DECLARE_SIMPLE_TYPE(HtcleoMicrop, HTCLEO_MICROP)

#define MICROP_RCMD_VERSION         0x30
#define MICROP_WCMD_LCM_BL_MANU_CTL 0x22
#define MICROP_WCMD_AUTO_BL_CTL     0x23
#define MICROP_RCMD_SPI_BL_STATUS   0x24
#define MICROP_WCMD_BL_EN           0x26
#define MICROP_WCMD_READ_ADC_VALUE_REQ 0x60
#define MICROP_RCMD_ADC_VALUE       0x62
#define MICROP_RCMD_GSENSOR_X       0x77
#define MICROP_RCMD_GSENSOR_Y       0x78
#define MICROP_RCMD_GSENSOR_Z       0x79
#define MICROP_RCMD_GSENSOR_DATA    0x7A
#define MICROP_RCMD_GPI_INT_STATUS  0x82
#define MICROP_RCMD_GPIO_STATUS     0x83

struct HtcleoMicrop {
    I2CSlave parent_obj;
    uint8_t cmd;
    bool have_cmd;
    uint8_t wbuf[32];
    int wlen;
    int rpos;
    uint8_t reg[256][8];        /* last data written per command */
    int16_t gsensor[3];
    uint16_t light_adc;
    bool bl_en;
    uint8_t bl_level;           /* 0..9 */
    qemu_irq backlight;         /* panel brightness 0..255 */
};

static void microp_update_backlight(HtcleoMicrop *s)
{
    /*
     * Only a hint of the level reaches the emulated panel (level 0 is still
     * clearly readable); off is black.
     */
    qemu_set_irq(s->backlight, s->bl_en ? 175 + s->bl_level * 80 / 9 : 0);
}

static int microp_event(I2CSlave *i2c, enum i2c_event ev)
{
    HtcleoMicrop *s = HTCLEO_MICROP(i2c);

    switch (ev) {
    case I2C_START_SEND:
        s->have_cmd = false;
        s->wlen = 0;
        break;
    case I2C_START_RECV:
        s->rpos = 0;
        break;
    case I2C_FINISH:
        if (s->have_cmd && s->wlen) {
            memcpy(s->reg[s->cmd], s->wbuf, MIN(s->wlen, 8));
            qemu_log_mask(LOG_TRACE, "htcleo-microp: cmd 0x%02x len %d "
                          "data %02x %02x\n", s->cmd, s->wlen, s->wbuf[0],
                          s->wlen > 1 ? s->wbuf[1] : 0);
            if (s->cmd == MICROP_WCMD_BL_EN) {
                s->bl_en = s->wbuf[0] & 1;
                microp_update_backlight(s);
            } else if (s->cmd == MICROP_WCMD_LCM_BL_MANU_CTL) {
                s->bl_level = MIN(s->wbuf[0] >> 4, 9);
                microp_update_backlight(s);
            }
        }
        break;
    default:
        break;
    }
    return 0;
}

static int microp_send(I2CSlave *i2c, uint8_t data)
{
    HtcleoMicrop *s = HTCLEO_MICROP(i2c);

    if (!s->have_cmd) {
        s->cmd = data;
        s->have_cmd = true;
    } else if (s->wlen < (int)sizeof(s->wbuf)) {
        s->wbuf[s->wlen++] = data;
    }
    return 0;
}

static uint8_t microp_recv(I2CSlave *i2c)
{
    HtcleoMicrop *s = HTCLEO_MICROP(i2c);
    int i = s->rpos++;

    switch (s->cmd) {
    case MICROP_RCMD_VERSION:
        return i == 0 ? 0x0A : 0x05;     /* firmware 0x0A05 */
    case MICROP_RCMD_ADC_VALUE:
        return i == 0 ? s->light_adc >> 8 : s->light_adc & 0xFF;
    case MICROP_RCMD_GSENSOR_X:
    case MICROP_RCMD_GSENSOR_Y:
    case MICROP_RCMD_GSENSOR_Z: {
        int16_t v = s->gsensor[s->cmd - MICROP_RCMD_GSENSOR_X];
        return i == 0 ? (v >> 8) & 0xFF : v & 0xFF;
    }
    case MICROP_RCMD_GSENSOR_DATA: {
        int16_t v = s->gsensor[(i / 2) % 3];
        return (i & 1) ? v & 0xFF : (v >> 8) & 0xFF;
    }
    case MICROP_RCMD_GPI_INT_STATUS:
        return 0;
    case MICROP_RCMD_GPIO_STATUS:
        return 0;
    case MICROP_RCMD_SPI_BL_STATUS:
        return 1;
    default:
        return i < 8 ? s->reg[s->cmd][i] : 0;
    }
}

static void microp_init(Object *obj)
{
    HtcleoMicrop *s = HTCLEO_MICROP(obj);

    s->light_adc = 0x0100;
    /* phone lying flat, screen up: ~1 g on Z (BMA150 10-bit, 256 LSB/g) */
    s->gsensor[0] = 0;
    s->gsensor[1] = 0;
    s->gsensor[2] = 256 << 6;
    qdev_init_gpio_out_named(DEVICE(obj), &s->backlight, "backlight", 1);
}

static void microp_reset(DeviceState *dev)
{
    HtcleoMicrop *s = HTCLEO_MICROP(dev);

    /* the bootloader leaves the backlight on */
    s->bl_en = true;
    s->bl_level = 9;
    microp_update_backlight(s);
}

static void microp_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = microp_event;
    k->send = microp_send;
    k->recv = microp_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), microp_reset);
}

/* ------------------------------------------------------------------ */
/* AKM8973 compass                                                     */
/* ------------------------------------------------------------------ */

#define AKM_ST          0xC0    /* bit 0: data ready */
#define AKM_TMPS        0xC1
#define AKM_H1X         0xC2
#define AKM_MS1         0xE0
#define AKM_MODE_MEASURE    0x00
#define AKM_MODE_POWERDOWN  0x03

OBJECT_DECLARE_SIMPLE_TYPE(HtcleoAkm, HTCLEO_AKM)

struct HtcleoAkm {
    I2CSlave parent_obj;
    qemu_irq irq;
    QEMUTimer *timer;
    uint8_t regs[256];
    uint8_t ptr;
    bool have_ptr;
};

static void akm_done(void *opaque)
{
    HtcleoAkm *s = opaque;

    s->regs[AKM_ST] = 1;
    s->regs[AKM_TMPS] = 0x90;
    /* about 0.3 G north and down, mid-scale is 0x80 */
    s->regs[AKM_H1X] = 0x98;
    s->regs[AKM_H1X + 1] = 0x80;
    s->regs[AKM_H1X + 2] = 0x60;
    s->regs[AKM_MS1] = AKM_MODE_POWERDOWN;
    qemu_set_irq(s->irq, 1);
}

static int akm_event(I2CSlave *i2c, enum i2c_event ev)
{
    HtcleoAkm *s = HTCLEO_AKM(i2c);

    if (ev == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static int akm_send(I2CSlave *i2c, uint8_t data)
{
    HtcleoAkm *s = HTCLEO_AKM(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
        return 0;
    }
    s->regs[s->ptr] = data;
    if (s->ptr == AKM_MS1 && data == AKM_MODE_MEASURE) {
        timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    }
    s->ptr++;
    return 0;
}

static uint8_t akm_recv(I2CSlave *i2c)
{
    HtcleoAkm *s = HTCLEO_AKM(i2c);
    uint8_t v = s->regs[s->ptr];

    if (s->ptr == AKM_ST && (v & 1)) {
        /* reading the status hands over the sample */
        s->regs[AKM_ST] = 0;
        qemu_set_irq(s->irq, 0);
    }
    s->ptr++;
    return v;
}

static void akm_reset(DeviceState *dev)
{
    HtcleoAkm *s = HTCLEO_AKM(dev);

    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[AKM_MS1] = AKM_MODE_POWERDOWN;
    qemu_set_irq(s->irq, 0);
}

static void akm_init(Object *obj)
{
    HtcleoAkm *s = HTCLEO_AKM(obj);

    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, akm_done, s);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq, "irq", 1);
}

static void akm_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = akm_event;
    k->send = akm_send;
    k->recv = akm_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), akm_reset);
}

static const TypeInfo htcleo_i2c_types[] = {
    {
        .name = TYPE_HTCLEO_REGDEV,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(HtcleoRegdev),
        .class_init = regdev_class_init,
    }, {
        .name = TYPE_HTCLEO_TS,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(HtcleoTs),
        .instance_init = ts_init,
        .class_init = ts_class_init,
    }, {
        .name = TYPE_HTCLEO_MICROP,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(HtcleoMicrop),
        .instance_init = microp_init,
        .class_init = microp_class_init,
    }, {
        .name = TYPE_HTCLEO_AKM,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(HtcleoAkm),
        .instance_init = akm_init,
        .class_init = akm_class_init,
    },
};
DEFINE_TYPES(htcleo_i2c_types)
