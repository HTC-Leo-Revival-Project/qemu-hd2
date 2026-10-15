/*
 * GSM SIM card (ISO 7816-3 T=0, GSM 11.11) for the emulated HD2
 *
 * A 2G SIM with the T-Mobile US identity the modelled baseband also uses
 * (IMSI 310 260 000000000, SPN "T-Mobile", no PIN).  It answers the class
 * A0 commands a baseband issues while it reads the card: SELECT, GET
 * RESPONSE, STATUS, READ/UPDATE BINARY and RECORD, VERIFY/ENABLE/DISABLE
 * CHV, RUN GSM ALGORITHM, TERMINAL PROFILE and ENVELOPE.  Class 00 (3G
 * USIM) commands get 6E00, which makes a terminal fall back to GSM.
 *
 * T=0 on the wire: the terminal sends a 5-byte header (CLA INS P1 P2 P3);
 * the card acknowledges with INS, then either receives P3 data bytes or
 * sends them, and finishes with SW1 SW2.  A SELECT answers 9F xx, the
 * length GET RESPONSE then returns.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "sim_card.h"

enum { EF_TRANSPARENT = 0, EF_LINEAR = 1, EF_CYCLIC = 3 };
enum { T_MF = 1, T_DF = 2, T_EF = 4 };

typedef struct SimFile {
    uint16_t id, parent;
    uint8_t type, structure;
    uint8_t rec_len;            /* records: length of one */
    uint16_t size;
    uint8_t *data;
} SimFile;

struct SimCard {
    SimFile *files;
    int nfiles;
    int cur_df, cur_ef;         /* indexes into files[] */
    uint8_t hdr[5];
    int hdr_len;
    uint8_t data[256];
    int data_len, data_want;    /* incoming data phase */
    uint8_t resp[64];           /* what GET RESPONSE returns */
    int resp_len;
    int rec_ptr;                /* current record for NEXT/PREVIOUS */
};

/* ATR: direct convention, T=0, no interface bytes, "SIM" history */
static const uint8_t sim_atr[] = { 0x3B, 0x02, 0x14, 0x50 };

static void bcd(const char *digits, uint8_t *out, int n)
{
    int len = strlen(digits);

    for (int i = 0; i < n; i++) {
        int lo = 2 * i < len ? digits[2 * i] - '0' : 0xF;
        int hi = 2 * i + 1 < len ? digits[2 * i + 1] - '0' : 0xF;
        out[i] = (hi << 4) | lo;
    }
}

static SimFile *add_file(SimCard *c, uint16_t id, uint16_t parent, int type,
                         int structure, int rec_len, int size)
{
    SimFile *f;

    c->files = g_renew(SimFile, c->files, c->nfiles + 1);
    f = &c->files[c->nfiles++];
    f->id = id;
    f->parent = parent;
    f->type = type;
    f->structure = structure;
    f->rec_len = rec_len;
    f->size = size;
    f->data = g_malloc(size ? size : 1);
    memset(f->data, 0xFF, size ? size : 1);
    return f;
}

static void build_fs(SimCard *c)
{
    SimFile *f;
    char tmp[20];

    add_file(c, 0x3F00, 0, T_MF, 0, 0, 0);
    add_file(c, 0x7F10, 0x3F00, T_DF, 0, 0, 0);         /* DF_TELECOM */
    add_file(c, 0x7F20, 0x3F00, T_DF, 0, 0, 0);         /* DF_GSM */

    f = add_file(c, 0x2FE2, 0x3F00, T_EF, EF_TRANSPARENT, 0, 10);  /* ICCID */
    bcd(SIM_ICCID, f->data, 10);
    f = add_file(c, 0x2F05, 0x3F00, T_EF, EF_TRANSPARENT, 0, 4);   /* ELP */
    memcpy(f->data, "en", 2);

    /* DF_GSM */
    f = add_file(c, 0x6F07, 0x7F20, T_EF, EF_TRANSPARENT, 0, 9);   /* IMSI */
    snprintf(tmp, sizeof(tmp), "9%s", SIM_IMSI);    /* odd: parity nibble 9 */
    f->data[0] = 8;
    bcd(tmp, f->data + 1, 8);
    f = add_file(c, 0x6F05, 0x7F20, T_EF, EF_TRANSPARENT, 0, 4);   /* LP */
    f->data[0] = 0x01;                              /* English */
    f = add_file(c, 0x6FAD, 0x7F20, T_EF, EF_TRANSPARENT, 0, 4);   /* AD */
    memcpy(f->data, "\x00\x00\x00\x03", 4);         /* normal, MNC 3 digits */
    f = add_file(c, 0x6F46, 0x7F20, T_EF, EF_TRANSPARENT, 0, 17);  /* SPN */
    f->data[0] = 0x01;
    memcpy(f->data + 1, SIM_SPN, strlen(SIM_SPN));
    f = add_file(c, 0x6FAE, 0x7F20, T_EF, EF_TRANSPARENT, 0, 1);   /* PHASE */
    f->data[0] = 0x03;                              /* phase 2+ */
    f = add_file(c, 0x6F38, 0x7F20, T_EF, EF_TRANSPARENT, 0, 10);  /* SST */
    /* CHV1 disable, ADN, FDN, SMS, AoC, CCP, PLMNsel, MSISDN, SPN, GPRS */
    memcpy(f->data, "\xFF\x3F\xFF\xFF\x3F\x00\x3F\x00\x0F\x00", 10);
    f = add_file(c, 0x6F31, 0x7F20, T_EF, EF_TRANSPARENT, 0, 1);   /* HPLMN */
    f->data[0] = 0x0A;                              /* search period 30 min */
    f = add_file(c, 0x6F30, 0x7F20, T_EF, EF_TRANSPARENT, 0, 24);  /* PLMNsel */
    memcpy(f->data, "\x13\x00\x62", 3);             /* 310 260 */
    f = add_file(c, 0x6F7B, 0x7F20, T_EF, EF_TRANSPARENT, 0, 12);  /* FPLMN */
    f = add_file(c, 0x6F7E, 0x7F20, T_EF, EF_TRANSPARENT, 0, 11);  /* LOCI */
    memcpy(f->data, "\xFF\xFF\xFF\xFF\x13\x00\x62\xFF\xFE\x00\x01", 11);
    f = add_file(c, 0x6F20, 0x7F20, T_EF, EF_TRANSPARENT, 0, 9);   /* Kc */
    f->data[8] = 0x07;                              /* no key */
    f = add_file(c, 0x6F52, 0x7F20, T_EF, EF_TRANSPARENT, 0, 9);   /* KcGPRS */
    f->data[8] = 0x07;
    f = add_file(c, 0x6F53, 0x7F20, T_EF, EF_TRANSPARENT, 0, 14);  /* LOCIGPRS */
    memcpy(f->data + 7, "\x13\x00\x62\xFF\xFE\x00\x02", 7);
    f = add_file(c, 0x6F78, 0x7F20, T_EF, EF_TRANSPARENT, 0, 2);   /* ACC */
    memcpy(f->data, "\x00\x01", 2);                 /* access class 0 */
    f = add_file(c, 0x6F74, 0x7F20, T_EF, EF_TRANSPARENT, 0, 16);  /* BCCH */
    f = add_file(c, 0x6FB7, 0x7F20, T_EF, EF_TRANSPARENT, 0, 3);   /* ECC */
    memcpy(f->data, "\x11\xF9\xFF", 3);             /* 911 */
    add_file(c, 0x6F45, 0x7F20, T_EF, EF_TRANSPARENT, 0, 2);       /* CBMI */
    add_file(c, 0x6F48, 0x7F20, T_EF, EF_TRANSPARENT, 0, 2);       /* CBMID */

    /* DF_TELECOM */
    f = add_file(c, 0x6F40, 0x7F10, T_EF, EF_LINEAR, 28, 28);      /* MSISDN */
    f->data[14] = 1 + (strlen(SIM_MSISDN + 1) + 1) / 2;
    f->data[15] = 0x91;                             /* international */
    bcd(SIM_MSISDN + 1, f->data + 16, 10);
    add_file(c, 0x6F3A, 0x7F10, T_EF, EF_LINEAR, 28, 28 * 10);     /* ADN */
    add_file(c, 0x6F3B, 0x7F10, T_EF, EF_LINEAR, 28, 28 * 2);      /* FDN */
    f = add_file(c, 0x6F3C, 0x7F10, T_EF, EF_LINEAR, 176, 176 * 10); /* SMS */
    for (int i = 0; i < 10; i++) {
        f->data[176 * i] = 0x00;                    /* free */
    }
    f = add_file(c, 0x6F42, 0x7F10, T_EF, EF_LINEAR, 40, 40);      /* SMSP */
    /* 12 octets of alpha tag, then only the service centre's address */
    f->data[12] = 0xFD;
    f->data[25] = 1 + (strlen(SIM_SMSC + 1) + 1) / 2;
    f->data[26] = 0x91;
    bcd(SIM_SMSC + 1, f->data + 27, 10);
    f = add_file(c, 0x6F43, 0x7F10, T_EF, EF_TRANSPARENT, 0, 2);   /* SMSS */
    memcpy(f->data, "\x00\xFF", 2);
    add_file(c, 0x6F44, 0x7F10, T_EF, EF_CYCLIC, 28, 28 * 5);      /* LND */
    add_file(c, 0x6F4A, 0x7F10, T_EF, EF_LINEAR, 13, 13 * 2);      /* EXT1 */
}

static int find_file(SimCard *c, uint16_t id)
{
    SimFile *df = &c->files[c->cur_df];

    for (int i = 0; i < c->nfiles; i++) {
        SimFile *f = &c->files[i];
        /* MF, the current DF, its children, its parent and its siblings */
        if (f->id == id && (f->type == T_MF || f->parent == df->id ||
                            f->id == df->parent ||
                            (f->type == T_DF && f->parent == df->parent))) {
            return i;
        }
    }
    return -1;
}

/* the GSM 11.11 response to SELECT / STATUS */
static int file_response(SimCard *c, int idx, uint8_t *r)
{
    SimFile *f = &c->files[idx];

    memset(r, 0, 22);
    stw_be_p(r + 4, f->id);
    if (f->type != T_EF) {
        int dfs = 0, efs = 0;
        for (int i = 0; i < c->nfiles; i++) {
            if (c->files[i].parent == f->id) {
                c->files[i].type == T_EF ? efs++ : dfs++;
            }
        }
        stw_be_p(r + 2, 0x1000);                    /* free memory */
        r[6] = f->type;
        r[12] = 10;                                 /* bytes that follow */
        r[13] = 0x93;                   /* CHV1 disabled, clock stop allowed */
        r[14] = dfs;
        r[15] = efs;
        r[16] = 4;                                  /* CHVs and unblock */
        r[18] = 0x83;                               /* CHV1 set, 3 tries */
        r[19] = 0x8A;                               /* PUK1, 10 tries */
        r[20] = 0x83;
        r[21] = 0x8A;
        return 22;
    }
    stw_be_p(r + 2, f->size);
    r[6] = T_EF;
    r[8] = 0x11;                                    /* read/update: CHV1 */
    r[9] = 0xF4;                                    /* increase: never */
    r[10] = 0x44;                                   /* (in)validate: ADM */
    r[11] = 0x01;                                   /* not invalidated */
    r[12] = 2;
    r[13] = f->structure;
    r[14] = f->rec_len;
    return 15;
}

static void sw(GByteArray *out, uint16_t w)
{
    uint8_t b[2] = { w >> 8, w & 0xFF };
    g_byte_array_append(out, b, 2);
}

/* RUN GSM ALGORITHM: SRES and Kc from RAND (a fixed mix: no Ki) */
static void gsm_algo(const uint8_t *rand, uint8_t *res)
{
    for (int i = 0; i < 12; i++) {
        res[i] = rand[i] ^ rand[(i + 5) & 15] ^ (0x5A + i * 17);
    }
}

static void execute(SimCard *c, GByteArray *out)
{
    uint8_t cla = c->hdr[0], ins = c->hdr[1], p1 = c->hdr[2], p2 = c->hdr[3];
    int p3 = c->hdr[4];
    SimFile *ef = c->cur_ef >= 0 ? &c->files[c->cur_ef] : NULL;

    switch (ins) {
    case 0xA4: {                                    /* SELECT */
        int idx = find_file(c, lduw_be_p(c->data));
        if (idx < 0) {
            sw(out, 0x9404);
            return;
        }
        if (c->files[idx].type == T_EF) {
            c->cur_ef = idx;
        } else {
            c->cur_df = idx;
            c->cur_ef = -1;
        }
        c->rec_ptr = 0;
        c->resp_len = file_response(c, idx, c->resp);
        sw(out, 0x9F00 | c->resp_len);
        return;
    }
    case 0xC0:                                      /* GET RESPONSE */
    case 0xF2:                                      /* STATUS */
        if (ins == 0xF2) {
            c->resp_len = file_response(c, c->cur_df, c->resp);
        }
        g_byte_array_append(out, &ins, 1);
        for (int i = 0; i < p3; i++) {
            uint8_t b = i < c->resp_len ? c->resp[i] : 0;
            g_byte_array_append(out, &b, 1);
        }
        sw(out, 0x9000);
        return;
    case 0xB0:                                      /* READ BINARY */
    case 0xB2: {                                    /* READ RECORD */
        int off, len = p3 ? p3 : 256;
        if (!ef) {
            sw(out, 0x9400);
            return;
        }
        if (ins == 0xB0) {
            off = p1 << 8 | p2;
        } else {
            int n = ef->rec_len ? ef->size / ef->rec_len : 0;
            int rec = p2 == 0x04 ? p1 : p2 == 0x02 ? c->rec_ptr + 1 :
                      p2 == 0x03 ? c->rec_ptr - 1 : c->rec_ptr;
            if (rec < 1 || rec > n) {
                sw(out, 0x9402);
                return;
            }
            c->rec_ptr = rec;
            off = (rec - 1) * ef->rec_len;
        }
        if (off + len > ef->size) {
            sw(out, 0x6700 | MIN(ef->size - off, 0xFF));
            return;
        }
        g_byte_array_append(out, &ins, 1);
        g_byte_array_append(out, ef->data + off, len);
        sw(out, 0x9000);
        return;
    }
    case 0xD6:                                      /* UPDATE BINARY */
    case 0xDC: {                                    /* UPDATE RECORD */
        int off;
        if (!ef) {
            sw(out, 0x9400);
            return;
        }
        if (ins == 0xD6) {
            off = p1 << 8 | p2;
        } else {
            int n = ef->rec_len ? ef->size / ef->rec_len : 0;
            int rec = p2 == 0x04 ? p1 : p2 == 0x02 ? c->rec_ptr + 1 :
                      p2 == 0x03 ? c->rec_ptr - 1 : c->rec_ptr;
            if (rec < 1 || rec > n) {
                sw(out, 0x9402);
                return;
            }
            c->rec_ptr = rec;
            off = (rec - 1) * ef->rec_len;
        }
        if (off + p3 <= ef->size) {
            memcpy(ef->data + off, c->data, p3);
        }
        sw(out, 0x9000);
        return;
    }
    case 0x88:                                      /* RUN GSM ALGORITHM */
        gsm_algo(c->data, c->resp);
        c->resp_len = 12;
        sw(out, 0x9F0C);
        return;
    case 0x20: case 0x24: case 0x26: case 0x28: case 0x2C:
        sw(out, 0x9000);                            /* CHV: no PIN set */
        return;
    case 0x10:                                      /* TERMINAL PROFILE */
    case 0xC2:                                      /* ENVELOPE */
    case 0x14:                                      /* TERMINAL RESPONSE */
    case 0x04: case 0x44:                           /* (IN)VALIDATE */
    case 0xFA:                                      /* SLEEP */
        sw(out, 0x9000);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sim: CLA %02x INS %02x\n", cla, ins);
    sw(out, 0x6D00);
}

/* does this command carry data to the card? */
static bool data_in(uint8_t ins)
{
    switch (ins) {
    case 0xA4: case 0xD6: case 0xDC: case 0x88: case 0x20: case 0x24:
    case 0x26: case 0x28: case 0x2C: case 0x10: case 0xC2: case 0x14:
    case 0x32:
        return true;
    }
    return false;
}

void sim_card_rx(SimCard *c, uint8_t byte, GByteArray *out)
{
    if (c->data_want) {
        c->data[c->data_len++] = byte;
        if (c->data_len == c->data_want) {
            c->data_want = 0;
            execute(c, out);
        }
        return;
    }
    c->hdr[c->hdr_len++] = byte;
    if (c->hdr_len < 5) {
        return;
    }
    c->hdr_len = 0;
    c->data_len = 0;
    if (c->hdr[0] != 0xA0) {
        sw(out, 0x6E00);                            /* only the GSM class */
        return;
    }
    if (data_in(c->hdr[1]) && c->hdr[4]) {
        c->data_want = c->hdr[4];
        g_byte_array_append(out, &c->hdr[1], 1);    /* ACK: send the data */
        return;
    }
    execute(c, out);
}

const uint8_t *sim_card_reset(SimCard *c, int *len)
{
    c->hdr_len = 0;
    c->data_want = 0;
    c->cur_df = 0;                                  /* MF */
    c->cur_ef = -1;
    c->rec_ptr = 0;
    c->resp_len = 0;
    *len = sizeof(sim_atr);
    return sim_atr;
}

SimCard *sim_card_new(void)
{
    SimCard *c = g_new0(SimCard, 1);

    build_fs(c);
    c->cur_ef = -1;
    return c;
}
