/*
 * HTC HD2 baseband (AMSS) shared-memory protocol model
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_MSM_MODEM_H
#define HW_ARM_HTCLEO_MSM_MODEM_H

#include "hw/core/qdev.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "net/net.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmModemState, MSM_MODEM)

#define SMD_NUM_CH  64
#define M2A_DSP     7           /* -> INT_ADSP_A11 */
#define M2A_NUM     8

typedef struct SmdChan {
    char name[21];
    int n;
    uint32_t cid, ctype;
    uint32_t shared;            /* SMEM offset of the two half channels */
    uint32_t fifo;
    uint32_t fifo_size;         /* per direction */
    uint32_t apps, modem;       /* half-channel offsets */
    uint32_t apps_data, modem_data;
    bool packet;
} SmdChan;

typedef struct RpcServer {
    uint32_t prog, vers;
    uint32_t pid, cid;
} RpcServer;

struct MsmModemState {
    DeviceState parent_obj;
    uint8_t *smem;
    char *smem_image;
    uint32_t boot_reason;
    bool trace;
    qemu_irq m2a[M2A_NUM];
    QEMUTimer *wake_timer;
    QEMUTimer *park_timer;      /* watches MAGLDR's modem mailbox */
    QEMUTimer *q6_timer;        /* qdsp6-only: waits for AMSS's SMEM */
    bool qdsp6_only;            /* baseband=lle: only play the QDSP6 */
    bool q6_up;
    uint32_t last_apps;
    GByteArray *pkt_acc[SMD_NUM_CH];

    /* RPC router (msm_rpc.c) */
    bool rpc_hello_done;
    GArray *rpc_servers;        /* RpcServer */
    GArray *rpc_apps_servers;   /* RpcServer registered by the apps */
    GArray *rpc_clients;        /* RpcServer: last apps caller per program */
    uint32_t rpc_xid;
    GHashTable *rpc_frag;       /* (pid,cid) -> partial message */
    uint32_t rpc_next_cid;
    QEMUTimer *rpc_timer;
    uint64_t rpc_calls;

    /* QDSP6 (msm_qdsp6.c) */
    AudioBackend *audio_be;

    /* codec aux input (the FM receiver's line out), looped back to the
     * speaker/headset while AMSS has aux loopback on */
    SWVoiceOut *aux_voice;
    void (*aux_src)(void *opaque, int16_t *buf, int frames);
    void *aux_opaque;

    /* output not yet accepted by a full SMD FIFO */
    GByteArray *tx_backlog[SMD_NUM_CH];

    /* AT channel, PPP and packet data (msm_atmodem.c) */
    struct MsmAtModem *at;
    NICConf nic_conf;
    NICState *nic;

    /* GPS (msm_gps.c) */
    struct MsmGps *gps;
    char *gps_lat, *gps_lon, *gps_alt;

    /* MAGLDR has taken the modem core over (AMSS no longer runs) */
    bool taken_over;
};

uint32_t smem_rd(MsmModemState *s, uint32_t off);
void smem_wr(MsmModemState *s, uint32_t off, uint32_t v);
uint32_t smem_item(MsmModemState *s, unsigned id, uint32_t *size);

void msm_modem_set_smem(MsmModemState *s, uint8_t *smem);
void msm_modem_boot(MsmModemState *s);
/* baseband=lle: the real AMSS owns SMEM; bring up only the QDSP6 side */
void msm_modem_qdsp6_boot(MsmModemState *s);

int smd_modem_write(MsmModemState *s, SmdChan *c, const void *buf, int len);
int smd_modem_send_packet(MsmModemState *s, SmdChan *c, const void *buf,
                          int len);
void smd_kick_apps(MsmModemState *s, SmdChan *c);
/* queue bytes for a stream channel by name; sent as the FIFO drains */
void msm_modem_chan_send(MsmModemState *s, const char *name, const void *buf,
                         int len);

/* the device driving the codec's aux input: 48 kHz stereo S16 frames */
void msm_modem_set_aux_source(MsmModemState *s,
                              void (*src)(void *opaque, int16_t *buf,
                                          int frames),
                              void *opaque);

/* msm_atmodem.c */
void msm_atmodem_init(MsmModemState *s);
void msm_atmodem_reset(MsmModemState *s);
void msm_atmodem_opened(MsmModemState *s, SmdChan *c);
void msm_at_input(MsmModemState *s, SmdChan *c, const uint8_t *buf, int len);
void msm_ppp_input(MsmModemState *s, SmdChan *c, const uint8_t *buf, int len);

/* msm_gps.c */
void msm_gps_init(MsmModemState *s);
void msm_gps_reset(MsmModemState *s);
/* PDAPI (0x3000005b) calls; returns the result word */
uint32_t msm_gps_pdapi_call(MsmModemState *s, uint32_t proc,
                            const uint8_t *args, uint32_t len);
void msm_rpc_apps_call(MsmModemState *s, uint32_t prog, uint32_t vers,
                       uint32_t proc, const uint32_t *args, int nargs);
bool msm_modem_find_channel(MsmModemState *s, const char *name, int type,
                            SmdChan *out);

/* msm_rpc.c */
void msm_modem_rpc_init(MsmModemState *s);
void msm_modem_rpc_reset(MsmModemState *s);
void msm_modem_smd_opened(MsmModemState *s, SmdChan *c);
void msm_modem_smd_packet(MsmModemState *s, SmdChan *c, const uint8_t *buf,
                          uint32_t len);
void msm_qdsp6_dal_stream(MsmModemState *s, SmdChan *c, GByteArray *acc);
void msm_qdsp6_reset(MsmModemState *s);
void msm_modem_smd_stream(MsmModemState *s, SmdChan *c, const uint8_t *buf,
                          uint32_t len);

#endif
