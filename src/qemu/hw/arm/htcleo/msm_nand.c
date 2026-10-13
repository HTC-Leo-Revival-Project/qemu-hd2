/*
 * Qualcomm MSM NAND controller (0xA0A00000) with the HD2's 512 MiB x16
 * NAND behind it.
 *
 * Registers: drivers/mtd/devices/msm_nand.h.  Both cLK
 * (target/htcleo/nand.c) and Linux (drivers/mtd/devices/htcleo_nand.c)
 * drive it through the data mover: they write FLASH_CMD/ADDR0/ADDR1/
 * CHIP_SELECT and DEV0_CFG0/1, then kick EXEC_CMD once per 528-byte
 * codeword and move data through the FLASH_BUFFER window.
 *
 * Backing store: a raw image of 4096 blocks x 64 pages x 2112 bytes.
 * Each page is four 528-byte codewords laid out exactly as the ECC-mode
 * read buffer presents them (on writes the host supplies [data][spare]
 * and the controller inserts the parity in between):
 *
 *     [0..511] user data  [512..521] ECC  [522..525] spare  [526..527] bbf
 *
 * In ECC mode with the bad-block byte in the user area (CFG1 bit16 = 0)
 * the controller keeps the factory marker at BAD_BLOCK_BYTE_NUM of the last
 * codeword and relocates the user data there (one byte, or two on the HD2's
 * x16 part) to the bbf positions at the end of the codeword.  A read puts
 * the user data back and reports the marker in BUFFER_STATUS[31:16]
 * (BAD_BLOCK_STATUS), which is how AMSS's bad-block scan reads it.  Raw
 * mode (CFG1 bit0 = 1) moves the codeword untouched.  Programming can only
 * clear bits, erase sets a whole block back to 0xFF.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/block-backend.h"
#include "htcleo.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmNandState, MSM_NAND)

#define NAND_FLASH_CMD          0x00
#define NAND_ADDR0              0x04
#define NAND_ADDR1              0x08
#define NAND_FLASH_CHIP_SELECT  0x0C
#define NAND_EXEC_CMD           0x10
#define NAND_FLASH_STATUS       0x14
#define NAND_BUFFER_STATUS      0x18
#define NAND_DEV0_CFG0          0x20
#define NAND_DEV0_CFG1          0x24
#define NAND_READ_ID            0x40
#define NAND_READ_STATUS        0x44
#define NAND_FLASH_BUFFER       0x100
#define NAND_BUF_SIZE           0x400

#define CMD_SOFT_RESET          0x01
#define CMD_FETCH_ID            0x0B
#define CMD_STATUS              0x0C
#define CMD_RESET               0x0D
#define CMD_SEQ_PAGE_READ       0x15
#define CMD_PAGE_READ           0x32
#define CMD_PAGE_READ_ECC       0x33
#define CMD_PAGE_READ_ALL       0x34
#define CMD_PRG_PAGE            0x36
#define CMD_PRG_PAGE_ECC        0x37
#define CMD_PRG_PAGE_ALL        0x39
#define CMD_BLOCK_ERASE         0x3A

#define PAGE_DATA               2048
#define PAGE_RAW                2112
#define CW_RAW                  528
#define PAGES_PER_BLOCK         64
#define NUM_BLOCKS              4096
#define NAND_RAW_SIZE           ((uint64_t)NUM_BLOCKS * PAGES_PER_BLOCK * PAGE_RAW)

/*
 * FLASH_STATUS after an operation.  A real HD2 idles at 0x00C00020: bit 5
 * READY, OPER_STATUS (bits 3:0) idle, the chip's status byte (0xC0) in
 * bits 23:16.  MAGLDR checks (status & 0x713F) == 0x20; Linux and cLK also
 * want bit 7 after a program or erase.
 */
#define FLASH_STATUS_OK         0x00C00020
#define FLASH_STATUS_PROG_OK    0x00000080

struct MsmNandState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq_op;
    qemu_irq irq_wr;
    BlockBackend *blk;
    uint8_t *mem;           /* used when no drive is attached */

    uint32_t regs[0x40];
    uint8_t buf[NAND_BUF_SIZE];
    uint32_t flash_id;
    bool trace;

    /* multi-codeword operation in progress */
    uint32_t op_cmd;
    uint32_t op_page;
    int op_cw;
    int op_off;                 /* column offset inside the first codeword */
    int op_remaining;
};

static uint32_t reg(MsmNandState *s, hwaddr off)
{
    return s->regs[off >> 2];
}

static bool nand_io(MsmNandState *s, uint64_t off, void *data, int len,
                    bool write)
{
    if (off + len > NAND_RAW_SIZE) {
        return false;
    }
    if (s->blk) {
        int r = write ? blk_pwrite(s->blk, off, len, data, 0)
                      : blk_pread(s->blk, off, len, data, 0);
        return r >= 0;
    }
    if (write) {
        memcpy(s->mem + off, data, len);
    } else {
        memcpy(data, s->mem + off, len);
    }
    return true;
}

static void nand_read_cw(MsmNandState *s, uint32_t page, int cw, uint8_t *raw)
{
    if (!nand_io(s, (uint64_t)page * PAGE_RAW + cw * CW_RAW, raw, CW_RAW,
                 false)) {
        memset(raw, 0xFF, CW_RAW);
    }
}

static void nand_write_cw(MsmNandState *s, uint32_t page, int cw,
                          const uint8_t *raw)
{
    nand_io(s, (uint64_t)page * PAGE_RAW + cw * CW_RAW, (void *)raw, CW_RAW,
            true);
}

static int bb_pos(MsmNandState *s)
{
    uint32_t cfg1 = reg(s, NAND_DEV0_CFG1);
    int n = (cfg1 >> 6) & 0x3FF;
    return n ? n - 1 : -1;
}

static bool ecc_on(MsmNandState *s)
{
    return !(reg(s, NAND_DEV0_CFG1) & 1);
}

/* x16 parts keep a 16-bit marker, so two user bytes are displaced */
static int bb_width(MsmNandState *s)
{
    return (reg(s, NAND_DEV0_CFG1) & 2) ? 2 : 1;
}

static bool bb_relocated(MsmNandState *s, int cw)
{
    /* last codeword, ECC on, marker in the user area */
    return ecc_on(s) && cw == 3 && !(reg(s, NAND_DEV0_CFG1) & (1 << 16)) &&
           bb_pos(s) >= 0 && bb_pos(s) < CW_RAW - 2;
}

/*
 * The image keeps each codeword the way an ECC-mode read presents it:
 * [user data][parity][spare][2 bytes].  On the chip the last two (the bytes
 * displaced by the bad-block marker) sit between the parity and the spare:
 * [user][parity][2 bytes][spare].  Raw (ECC off) transfers see the chip
 * order; Windows Mobile's FMD reads and writes each codeword's 4 spare bytes
 * raw at column 524 and the same bytes at offset 522 of ECC-mode reads.
 */
#define CW_SPARE_AT     522
static void cw_image_to_chip(const uint8_t *img, uint8_t *chip)
{
    memcpy(chip, img, CW_SPARE_AT);
    memcpy(chip + CW_SPARE_AT, img + CW_RAW - 2, 2);
    memcpy(chip + CW_SPARE_AT + 2, img + CW_SPARE_AT, 4);
}

static void cw_chip_to_image(const uint8_t *chip, uint8_t *img)
{
    memcpy(img, chip, CW_SPARE_AT);
    memcpy(img + CW_SPARE_AT, chip + CW_SPARE_AT + 2, 4);
    memcpy(img + CW_RAW - 2, chip + CW_SPARE_AT, 2);
}

static void nand_do_read(MsmNandState *s)
{
    uint8_t raw[CW_RAW];

    nand_read_cw(s, s->op_page, s->op_cw, raw);
    if (ecc_on(s)) {
        memcpy(s->buf, raw, CW_RAW);
    } else {
        cw_image_to_chip(raw, s->buf);
    }
    if (bb_relocated(s, s->op_cw)) {
        uint32_t bbs = 0xFFFF;

        for (int i = 0; i < bb_width(s); i++) {
            s->buf[bb_pos(s) + i] = raw[CW_RAW - 2 + i];
            bbs = deposit32(bbs, 8 * i, 8, raw[bb_pos(s) + i]);
        }
        /* BUFFER_STATUS[31:16] BAD_BLOCK_STATUS: the marker it found */
        s->regs[NAND_BUFFER_STATUS >> 2] =
            deposit32(s->regs[NAND_BUFFER_STATUS >> 2], 16, 16, bbs);
    }
    if (s->trace && s->op_off) {
        qemu_log("msm-nand: read page 0x%x cw %d at +%d\n", s->op_page,
                 s->op_cw, s->op_off);
    }
    if (s->op_off) {
        /*
         * The column address starts the transfer inside the codeword, e.g.
         * a 2-byte raw read of the bad-block marker at column 2048.
         */
        memmove(s->buf, s->buf + s->op_off, CW_RAW - s->op_off);
        memset(s->buf + CW_RAW - s->op_off, 0xFF, s->op_off);
        s->op_off = 0;
    }
    if (s->trace) {
        qemu_log("msm-nand: read page 0x%x cw %d cmd %02x cfg0 %08x "
                 "cfg1 %08x data %02x%02x bb@%d %02x%02x\n",
                 s->op_page, s->op_cw, s->op_cmd, reg(s, NAND_DEV0_CFG0),
                 reg(s, NAND_DEV0_CFG1), s->buf[0], s->buf[1], bb_pos(s),
                 bb_pos(s) >= 0 ? s->buf[bb_pos(s)] : 0,
                 bb_pos(s) >= 0 ? s->buf[bb_pos(s) + 1] : 0);
    }
}

static void nand_do_program(MsmNandState *s)
{
    uint8_t raw[CW_RAW], img[CW_RAW];

    if (s->trace) {
        qemu_log("msm-nand: program page 0x%x cw %d +%d cfg0 %08x cfg1 %08x "
                 "data %02x%02x%02x%02x spare %02x%02x%02x%02x\n",
                 s->op_page, s->op_cw, s->op_off, reg(s, NAND_DEV0_CFG0),
                 reg(s, NAND_DEV0_CFG1), s->buf[0], s->buf[1], s->buf[2],
                 s->buf[3], s->buf[522], s->buf[523], s->buf[524],
                 s->buf[525]);
    }

    memcpy(img, s->buf, CW_RAW);
    if (!ecc_on(s)) {
        /*
         * Raw mode moves UD_SIZE + ECC_PARITY + SPARE bytes from the column
         * address on: Windows Mobile's FAL rewrites a page's 8-byte
         * SectorInfo as 4-byte raw programs into the spare area.
         */
        uint32_t cfg0 = reg(s, NAND_DEV0_CFG0);
        int len = ((cfg0 >> 9) & 0x3FF) + ((cfg0 >> 19) & 0xF) +
                  ((cfg0 >> 23) & 0xF);

        if (len && (s->op_off || len < CW_RAW)) {
            len = MIN(len, CW_RAW - s->op_off);
            memset(img, 0xFF, CW_RAW);
            memcpy(img + s->op_off, s->buf, len);
        }
        s->op_off = 0;
    }
    if (ecc_on(s)) {
        uint32_t cfg0 = reg(s, NAND_DEV0_CFG0);
        int ud = (cfg0 >> 9) & 0x3FF;
        int ecc = (cfg0 >> 19) & 0xF;
        int spare = (cfg0 >> 23) & 0xF;
        /*
         * The host fills user data and spare back to back
         * ([ud][spare]); the controller inserts its parity after the user
         * data, so the codeword on flash (and in the read buffer) is
         * [ud][ecc][spare].  Parity is modelled as zero bytes.
         */
        if (ud + ecc + spare <= CW_RAW - 2) {
            memcpy(img + ud + ecc, s->buf + ud, spare);
            memset(img + ud, 0, ecc);
        }
        img[CW_RAW - 2] = img[CW_RAW - 1] = 0xFF;
        if (bb_relocated(s, s->op_cw)) {
            for (int i = 0; i < bb_width(s); i++) {
                img[CW_RAW - 2 + i] = s->buf[bb_pos(s) + i];
                img[bb_pos(s) + i] = 0xFF;
            }
        }
    }
    if (!ecc_on(s)) {
        uint8_t chip[CW_RAW];

        memcpy(chip, img, CW_RAW);
        cw_chip_to_image(chip, img);
    }
    nand_read_cw(s, s->op_page, s->op_cw, raw);
    for (int i = 0; i < CW_RAW; i++) {
        raw[i] &= img[i];               /* programming only clears bits */
    }
    nand_write_cw(s, s->op_page, s->op_cw, raw);
}

static void nand_do_erase(MsmNandState *s)
{
    uint32_t block = s->op_page / PAGES_PER_BLOCK;
    g_autofree uint8_t *ff = g_malloc(PAGES_PER_BLOCK * PAGE_RAW);

    if (block >= NUM_BLOCKS) {
        return;
    }
    if (s->trace) {
        qemu_log("msm-nand: erase block 0x%x\n", block);
    }
    memset(ff, 0xFF, PAGES_PER_BLOCK * PAGE_RAW);
    nand_io(s, (uint64_t)block * PAGES_PER_BLOCK * PAGE_RAW, ff,
            PAGES_PER_BLOCK * PAGE_RAW, true);
}

static void nand_exec(MsmNandState *s)
{
    uint32_t cmd = reg(s, NAND_FLASH_CMD) & 0xFF;

    s->regs[NAND_BUFFER_STATUS >> 2] = 0;
    s->regs[NAND_FLASH_STATUS >> 2] = FLASH_STATUS_OK;
    if (cmd == CMD_PRG_PAGE || cmd == CMD_PRG_PAGE_ECC ||
        cmd == CMD_PRG_PAGE_ALL || cmd == CMD_BLOCK_ERASE) {
        s->regs[NAND_FLASH_STATUS >> 2] |= FLASH_STATUS_PROG_OK;
    }

    switch (cmd) {
    case CMD_FETCH_ID:
        s->regs[NAND_READ_ID >> 2] = s->flash_id;
        s->op_remaining = 0;
        return;
    case CMD_SOFT_RESET:
    case CMD_RESET:
    case CMD_STATUS:
        s->regs[NAND_READ_STATUS >> 2] = 0xE0;  /* ready, not protected */
        s->op_remaining = 0;
        return;
    case CMD_BLOCK_ERASE:
        s->op_page = reg(s, NAND_ADDR0) | (reg(s, NAND_ADDR1) << 16);
        nand_do_erase(s);
        s->regs[NAND_READ_STATUS >> 2] = 0xE0;
        s->op_remaining = 0;
        qemu_irq_pulse(s->irq_wr);
        return;
    }

    if (s->op_remaining == 0) {
        uint32_t a0 = reg(s, NAND_ADDR0), a1 = reg(s, NAND_ADDR1);
        uint32_t col = a0 & 0xFFFF;
        bool wide = reg(s, NAND_DEV0_CFG1) & 2;

        s->op_cmd = cmd;
        s->op_page = (a0 >> 16) | ((a1 & 0xFF) << 16);
        s->op_cw = ((wide ? col * 2 : col) / CW_RAW) & 3;
        s->op_off = (wide ? col * 2 : col) % CW_RAW;
        s->op_remaining = ((reg(s, NAND_DEV0_CFG0) >> 6) & 7) + 1;
    }

    if (s->op_page >= NUM_BLOCKS * PAGES_PER_BLOCK) {
        memset(s->buf, 0xFF, CW_RAW);
    } else {
        switch (cmd) {
        case CMD_PAGE_READ:
        case CMD_PAGE_READ_ECC:
        case CMD_PAGE_READ_ALL:
        case CMD_SEQ_PAGE_READ:
            nand_do_read(s);
            break;
        case CMD_PRG_PAGE:
        case CMD_PRG_PAGE_ECC:
        case CMD_PRG_PAGE_ALL:
            nand_do_program(s);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "msm-nand: command 0x%x\n", cmd);
            break;
        }
    }
    /*
     * FLASH_STATUS[14:12] counts down the codewords still to come in this
     * operation (3, 2, 1, 0 for a whole 2 KiB page); MAGLDR's flash HAL
     * checks it for every codeword it reads.
     */
    s->regs[NAND_FLASH_STATUS >> 2] |= ((s->op_remaining - 1) & 7) << 12;
    s->op_cw = (s->op_cw + 1) & 3;
    if (--s->op_remaining <= 0) {
        s->op_remaining = 0;
        if (cmd >= CMD_PRG_PAGE) {
            qemu_irq_pulse(s->irq_wr);
        } else {
            qemu_irq_pulse(s->irq_op);
        }
    }
}

static uint64_t nand_read(void *opaque, hwaddr off, unsigned size)
{
    MsmNandState *s = opaque;

    if (off >= NAND_FLASH_BUFFER && off < NAND_FLASH_BUFFER + NAND_BUF_SIZE) {
        uint32_t v = 0;
        hwaddr o = off - NAND_FLASH_BUFFER;
        for (unsigned i = 0; i < size && o + i < NAND_BUF_SIZE; i++) {
            v |= (uint32_t)s->buf[o + i] << (8 * i);
        }
        return v;
    }
    if (off < sizeof(s->regs)) {
        return s->regs[off >> 2];
    }
    return 0;
}

static void nand_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmNandState *s = opaque;

    if (off >= NAND_FLASH_BUFFER && off < NAND_FLASH_BUFFER + NAND_BUF_SIZE) {
        hwaddr o = off - NAND_FLASH_BUFFER;
        for (unsigned i = 0; i < size && o + i < NAND_BUF_SIZE; i++) {
            s->buf[o + i] = val >> (8 * i);
        }
        return;
    }
    if (off >= sizeof(s->regs)) {
        return;
    }
    switch (off) {
    case NAND_ADDR0:
        /*
         * A new address starts a new operation.  AMSS rewrites the same
         * address before every codeword of a page, which carries on.
         */
        if (val != s->regs[off >> 2]) {
            s->op_remaining = 0;
        }
        s->regs[off >> 2] = val;
        break;
    case NAND_EXEC_CMD:
        if (val & 1) {
            nand_exec(s);
        }
        break;
    case NAND_FLASH_STATUS:
    case NAND_READ_STATUS:
        /* the drivers write these to clear them */
        s->regs[off >> 2] = (off == NAND_FLASH_STATUS) ? FLASH_STATUS_OK : 0;
        break;
    case NAND_READ_ID:
        break;
    default:
        s->regs[off >> 2] = val;
    }
}

static const MemoryRegionOps nand_ops = {
    .read = nand_read,
    .write = nand_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void nand_reset(DeviceState *dev)
{
    MsmNandState *s = MSM_NAND(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /*
     * The SPL leaves the controller configured; both cLK and Linux read
     * DEV0_CFG0/1 back and refuse to run if they are zero.  These are the
     * values read off a real HD2 (fastboot oem pwf 0xa0a00020).
     */
    s->regs[NAND_DEV0_CFG0 >> 2] = 0xAA5400C0;
    s->regs[NAND_DEV0_CFG1 >> 2] = 0x0006744E;
    s->regs[NAND_READ_ID >> 2] = s->flash_id;
    s->regs[NAND_FLASH_STATUS >> 2] = FLASH_STATUS_OK;
    s->regs[0xA0 >> 2] = 0x1080D060;    /* DEV_CMD0 defaults */
    s->regs[0xA4 >> 2] = 0xF00F3000;
    s->regs[0xA8 >> 2] = 0xF0FF7090;
    s->regs[0xAC >> 2] = 0x0000000D;    /* DEV_CMD_VLD */
    s->regs[0xF0 >> 2] = 0x000001FF;    /* EBI2_ECC_BUF_CFG */
    s->op_remaining = 0;
}

static void nand_realize(DeviceState *dev, Error **errp)
{
    MsmNandState *s = MSM_NAND(dev);

    if (s->blk) {
        uint64_t perm = BLK_PERM_CONSISTENT_READ |
                        (blk_supports_write_perm(s->blk) ? BLK_PERM_WRITE : 0);
        if (blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (blk_getlength(s->blk) < NAND_RAW_SIZE) {
            error_setg(errp, "msm-nand: image must be at least %" PRIu64
                       " bytes (4096 blocks x 64 pages x 2112)", NAND_RAW_SIZE);
            return;
        }
    } else {
        s->mem = g_malloc(NAND_RAW_SIZE);
        memset(s->mem, 0xFF, NAND_RAW_SIZE);
    }
}

static void nand_init(Object *obj)
{
    MsmNandState *s = MSM_NAND(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &nand_ops, s, "msm-nand", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_op);
    sysbus_init_irq(sbd, &s->irq_wr);
}

static const Property nand_props[] = {
    DEFINE_PROP_DRIVE("drive", MsmNandState, blk),
    /* READ_ID from a real HD2: maker 0xEC, device 0xBC, cfg 0x55 */
    DEFINE_PROP_UINT32("flash-id", MsmNandState, flash_id, 0x5500BCEC),
    DEFINE_PROP_BOOL("trace", MsmNandState, trace, false),
};

static void nand_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = nand_realize;
    device_class_set_legacy_reset(dc, nand_reset);
    device_class_set_props(dc, nand_props);
}

static const TypeInfo msm_nand_info = {
    .name = TYPE_MSM_NAND,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmNandState),
    .instance_init = nand_init,
    .class_init = nand_class_init,
};

static void msm_nand_register(void)
{
    type_register_static(&msm_nand_info);
}
type_init(msm_nand_register)
