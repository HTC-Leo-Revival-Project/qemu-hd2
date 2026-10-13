/*
 * The GPRS side of the modelled GSM network (gsm_net.c): RLC/MAC on one
 * PDCH, LLC, GMM, SM, SNDCP, and the MS's IP packets to and from the host
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_GSM_GPRS_H
#define HW_ARM_HTCLEO_GSM_GPRS_H

#include "gsm_net.h"

typedef struct Gprs Gprs;
typedef void GprsToHost(void *opaque, const uint8_t *ip, size_t len);

Gprs *gprs_new(GprsToHost *to_host, void *opaque);
void gprs_reset(Gprs *g);
void gprs_set_trace(Gprs *g, bool on);

/* the MS got an uplink TBF from an IMMEDIATE ASSIGNMENT */
void gprs_ul_assigned(Gprs *g);
/* an IMMEDIATE ASSIGNMENT of a downlink TBF to send on the CCCH */
bool gprs_ccch_assignment(Gprs *g, uint32_t fn, uint8_t b[GSM_BLOCK]);
/* the PDCH's downlink block that starts at frame fn; and an uplink one */
void gprs_pdch_dl(Gprs *g, uint32_t fn, uint8_t b[GSM_BLOCK]);
void gprs_pdch_ul(Gprs *g, uint32_t fn, const uint8_t b[GSM_BLOCK]);
/* an IP packet from the host for the MS */
void gprs_from_host(Gprs *g, const uint8_t *ip, size_t len);
/* the MS's address, 0 while no PDP context is active */
uint32_t gprs_ms_ip(Gprs *g);

#endif
