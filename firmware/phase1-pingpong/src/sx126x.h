/*
 * Minimal SX1262 LoRa driver, written from the Semtech SX1261/2 datasheet (rev 2.x).
 * Blocking, polled, no dynamic memory. Only what Phase 1 needs: init, TX, RX, packet status.
 */
#ifndef SX126X_H
#define SX126X_H

#include <stdint.h>
#include <stdbool.h>

/* Error codes (negative) */
#define SX_OK              0
#define SX_ERR_BUSY       -1   /* BUSY stuck high: wiring, power or reset problem   */
#define SX_ERR_SELFTEST   -2   /* register readback mismatch: SPI wiring problem    */
#define SX_ERR_TIMEOUT    -3   /* operation did not finish in time                  */
#define SX_ERR_CRC        -4   /* packet received with bad CRC or bad header        */
#define SX_ERR_PARAM      -5

/* TCXO supply voltage codes for SetDio3AsTcxoCtrl */
enum {
    SX_TCXO_1V6 = 0, SX_TCXO_1V7, SX_TCXO_1V8, SX_TCXO_2V2,
    SX_TCXO_2V4, SX_TCXO_2V7, SX_TCXO_3V0, SX_TCXO_3V3
};

typedef struct {
    uint32_t freq_hz;        /* e.g. 869525000                          */
    int8_t   power_dbm;      /* -9 .. +22 (high-power PA)               */
    uint8_t  sf;             /* 7 .. 12                                 */
    uint16_t bw_khz;         /* 125, 250 or 500                         */
    uint8_t  cr;             /* 1..4 -> 4/5 .. 4/8                      */
    uint16_t preamble;       /* symbols, 8 is standard                  */
    bool     use_tcxo;       /* TCXO powered from DIO3                  */
    uint8_t  tcxo_voltage;   /* SX_TCXO_xxx                             */
    bool     use_dcdc;       /* DC-DC regulator (needs inductor on module) else LDO */
} sx_config_t;

typedef struct {
    int16_t rssi_dbm;        /* average RSSI of the packet              */
    int8_t  snr_q4;          /* SNR in quarter dB (divide by 4)         */
    int16_t signal_rssi_dbm; /* RSSI of the LoRa signal after despreading */
} sx_pkt_status_t;

int      sx_init(const sx_config_t *cfg);
int      sx_selftest(void);                       /* call right after reset: checks default sync word */
int      sx_reset(void);

int      sx_set_modulation(uint8_t sf, uint16_t bw_khz, uint8_t cr);
int      sx_set_power(int8_t dbm);
int      sx_standby(void);

/* Blocking transmit. Returns SX_OK when TxDone fired. */
int      sx_transmit(const uint8_t *data, uint8_t len, uint32_t timeout_ms);

/* Start receiving. timeout_ms = 0 -> continuous RX (stays in RX after each packet). */
int      sx_rx_start(uint32_t timeout_ms);

/* Poll for a received packet.
 *   returns  1 : packet copied to buf (len set, st filled)
 *            0 : nothing yet
 *   SX_ERR_TIMEOUT : RX timeout fired (single RX mode)
 *   SX_ERR_CRC     : corrupted packet received (st still filled)            */
int      sx_rx_poll(uint8_t *buf, uint8_t *len, uint8_t maxlen, sx_pkt_status_t *st);

/* Diagnostics */
uint8_t  sx_get_status(void);                     /* raw status byte          */
uint16_t sx_get_errors(void);                     /* OpError bitfield         */
void     sx_clear_errors(void);
const char *sx_chip_mode_str(uint8_t status);
const char *sx_cmd_status_str(uint8_t status);
int      sx_read_reg(uint16_t addr, uint8_t *data, uint8_t n);

/* LoRa time-on-air in microseconds (Semtech AN1200.13 formula, explicit header, CRC on). */
uint32_t sx_time_on_air_us(uint8_t sf, uint16_t bw_khz, uint8_t cr,
                           uint16_t preamble, uint8_t payload_len);

#endif
