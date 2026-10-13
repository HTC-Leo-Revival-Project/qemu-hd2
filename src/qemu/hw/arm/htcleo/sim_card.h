/*
 * GSM SIM card (ISO 7816-3 T=0, GSM 11.11) for the emulated HD2
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_SIM_CARD_H
#define HW_ARM_HTCLEO_SIM_CARD_H

/* the T-Mobile US SIM both basebands present (like the AOSP emulator's) */
#define SIM_IMSI        "310260000000000"
#define SIM_ICCID       "89014103211118510720"
#define SIM_MSISDN      "+15555218135"
#define SIM_SMSC        "+12063130004"   /* the network's SMS centre */
#define SIM_SPN         "T-Mobile"

typedef struct SimCard SimCard;

SimCard *sim_card_new(void);
/* cold/warm reset: returns the ATR */
const uint8_t *sim_card_reset(SimCard *c, int *len);
/*
 * One byte from the terminal.  Bytes the card sends back are appended to
 * out (procedure bytes, data, status words).
 */
void sim_card_rx(SimCard *c, uint8_t byte, GByteArray *out);

#endif
