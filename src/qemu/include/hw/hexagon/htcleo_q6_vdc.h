/*
 * QSD8250 QDSP6 subsystem: the "Janus" video decoder core
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_HEXAGON_HTCLEO_Q6_VDC_H
#define HW_HEXAGON_HTCLEO_Q6_VDC_H

/* run one H.264 command buffer; 0 on success */
int htcleo_vdc_decode(uint32_t addr, bool trace, const char *dump_yuv);

#endif
