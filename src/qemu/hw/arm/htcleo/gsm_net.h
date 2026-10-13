/*
 * A GSM cell and the network behind it, for the mDSP model (msm_mdsp.c)
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_GSM_NET_H
#define HW_ARM_HTCLEO_GSM_NET_H

#define GSM_BLOCK       23              /* octets of a signalling block */
#define GSM_PDCH_BLOCK  53              /* of a packet data block (CS-4) */
#define GSM_ARFCN       133             /* the cell's, in its allocation */
#define GSM_BSIC        0x0A            /* NCC 1, BCC 2 */
#define GSM_TSC         2               /* = BCC */
#define GSM_RAC         1               /* routing area (GPRS) */
#define GSM_PDCH_TN     3               /* the packet data channel's slot */
#define GSM_USF         1               /* the MS's uplink state flag */
#define GSM_UL_TFI      0

typedef struct GsmNet GsmNet;

/* to_host: an IP packet from the MS (GPRS) */
GsmNet *gsm_net_new(void (*to_host)(void *opaque, const uint8_t *ip,
                                    size_t len), void *opaque);
void gsm_net_reset(GsmNet *n);
void gsm_net_set_trace(GsmNet *n, bool on);
void gsm_net_set_gprs_trace(GsmNet *n, bool on);

/* the idle-mode block (BCCH or CCCH) whose four frames include fn, else
 * the next one */
uint32_t gsm_net_block_fn(uint32_t fn);
/* the MS is on the dedicated channel the network gave it */
bool gsm_net_dedicated(GsmNet *n);
/* the idle-mode block (BCCH/CCCH) that starts at cell frame bfn */
void gsm_net_dl(GsmNet *n, uint32_t bfn, uint8_t b[GSM_BLOCK]);
/*
 * the dedicated channel's block that starts at cell frame bfn: the
 * SACCH's (sacch: the MS says so, as it does on a traffic channel), else
 * the SDCCH's, or on a traffic channel a FACCH frame or speech
 */
enum { GSM_DL_SPEECH, GSM_DL_SIGNAL, GSM_DL_FACCH };
int gsm_net_ded_dl(GsmNet *n, uint32_t bfn, bool sacch, uint8_t b[GSM_BLOCK]);
/* an uplink block sent at cell frame fn on the dedicated channel */
enum { GSM_UL_SPEECH = 0, GSM_UL_SDCCH = 4, GSM_UL_FACCH = 5, GSM_UL_PDTCH = 8 };
void gsm_net_ul(GsmNet *n, uint32_t fn, int chan, const uint8_t f[GSM_BLOCK]);
/*
 * the PDCH's downlink block that starts at frame bfn, and its coding
 * scheme (0-3: CS-1 to CS-4); an uplink one, sent at frame fn
 */
int gsm_net_pdch_dl(GsmNet *n, uint32_t bfn, uint8_t b[GSM_PDCH_BLOCK]);
void gsm_net_pdch_ul(GsmNet *n, uint32_t fn, const uint8_t *b, int cs);
/* the coding scheme the network uses for packet data (0-3) */
void gsm_net_set_gprs_cs(GsmNet *n, int cs);
/* an IP packet from the host for the MS; the MS's address (0: none) */
void gsm_net_from_host(GsmNet *n, const uint8_t *ip, size_t len);
uint32_t gsm_net_ms_ip(GsmNet *n);
/* a RACH: its content, L1's frame number and the cell's */
void gsm_net_rach(GsmNet *n, uint8_t ra, uint32_t l1_fn, uint32_t fn);

/* the network's user: a text to the MS, a call to it, hanging up */
void gsm_net_mt_sms(GsmNet *n, const char *from, const char *text);
void gsm_net_mt_call(GsmNet *n, const char *from);
void gsm_net_hangup(GsmNet *n);
/* the call's state, and the last events (texts sent by the MS ...) */
const char *gsm_net_call_state(GsmNet *n);
char *gsm_net_events(GsmNet *n);

#endif
