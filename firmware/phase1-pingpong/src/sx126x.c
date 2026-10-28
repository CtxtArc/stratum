/*
 * SX1262 driver — see sx126x.h. Opcodes and register addresses from the SX1261/2 datasheet.
 */
#include "sx126x.h"
#include "board.h"

/* ---------------------------------------------------------------- opcodes */
#define OP_SET_SLEEP              0x84
#define OP_SET_STANDBY            0x80
#define OP_SET_TX                 0x83
#define OP_SET_RX                 0x82
#define OP_SET_REGULATOR_MODE     0x96
#define OP_CALIBRATE              0x89
#define OP_CALIBRATE_IMAGE        0x98
#define OP_SET_PA_CONFIG          0x95
#define OP_SET_DIO_IRQ_PARAMS     0x08
#define OP_GET_IRQ_STATUS         0x12
#define OP_CLEAR_IRQ_STATUS       0x02
#define OP_SET_DIO2_AS_RF_SWITCH  0x9D
#define OP_SET_DIO3_AS_TCXO_CTRL  0x97
#define OP_SET_RF_FREQUENCY       0x86
#define OP_SET_PACKET_TYPE        0x8A
#define OP_SET_TX_PARAMS          0x8E
#define OP_SET_MODULATION_PARAMS  0x8B
#define OP_SET_PACKET_PARAMS      0x8C
#define OP_SET_BUFFER_BASE_ADDR   0x8F
#define OP_GET_STATUS             0xC0
#define OP_GET_RX_BUFFER_STATUS   0x13
#define OP_GET_PACKET_STATUS      0x14
#define OP_GET_DEVICE_ERRORS      0x17
#define OP_CLEAR_DEVICE_ERRORS    0x07
#define OP_WRITE_REGISTER         0x0D
#define OP_READ_REGISTER          0x1D
#define OP_WRITE_BUFFER           0x0E
#define OP_READ_BUFFER            0x1E

/* ---------------------------------------------------------------- registers */
#define REG_LORA_SYNC_MSB         0x0740
#define REG_LORA_SYNC_LSB         0x0741
#define REG_IQ_POLARITY           0x0736   /* errata 15.4 */
#define REG_RX_GAIN               0x08AC
#define REG_TX_CLAMP              0x08D8   /* errata 15.2 */
#define REG_OCP                   0x08E7

/* ---------------------------------------------------------------- irq bits */
#define IRQ_TX_DONE      (1u << 0)
#define IRQ_RX_DONE      (1u << 1)
#define IRQ_HEADER_ERR   (1u << 5)
#define IRQ_CRC_ERR      (1u << 6)
#define IRQ_TIMEOUT      (1u << 9)
#define IRQ_USED         (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT)

#define BUSY_TIMEOUT_MS  50u

static sx_config_t s_cfg;

/* ---------------------------------------------------------------- low level */

static int wait_busy(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while (rf_busy()) {
        if ((uint32_t)(millis() - t0) > timeout_ms) return SX_ERR_BUSY;
    }
    return SX_OK;
}

/* One SPI command frame: NSS low, n bytes full duplex, NSS high. */
static int xfer(const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    int e = wait_busy(BUSY_TIMEOUT_MS);
    if (e) return e;
    rf_nss(false);
    spi_transfer(tx, rx, n);
    rf_nss(true);
    delay_us(1);              /* BUSY rises up to ~600 ns after NSS goes high */
    return SX_OK;
}

static int cmd(uint8_t op, const uint8_t *args, uint8_t nargs)
{
    uint8_t buf[16];
    if (nargs > sizeof buf - 1) return SX_ERR_PARAM;
    buf[0] = op;
    for (uint8_t i = 0; i < nargs; i++) buf[1 + i] = args[i];
    return xfer(buf, 0, (uint16_t)(nargs + 1u));
}

static int write_reg(uint16_t addr, const uint8_t *data, uint8_t n)
{
    uint8_t buf[3 + 16];
    if (n > 16) return SX_ERR_PARAM;
    buf[0] = OP_WRITE_REGISTER;
    buf[1] = (uint8_t)(addr >> 8);
    buf[2] = (uint8_t)addr;
    for (uint8_t i = 0; i < n; i++) buf[3 + i] = data[i];
    return xfer(buf, 0, (uint16_t)(3u + n));
}

int sx_read_reg(uint16_t addr, uint8_t *data, uint8_t n)
{
    uint8_t tx[4 + 16] = {0}, rx[4 + 16];
    if (n > 16) return SX_ERR_PARAM;
    tx[0] = OP_READ_REGISTER;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)addr;
    /* tx[3] = NOP while the chip returns its status */
    int e = xfer(tx, rx, (uint16_t)(4u + n));
    if (e) return e;
    for (uint8_t i = 0; i < n; i++) data[i] = rx[4 + i];
    return SX_OK;
}

static int write_reg8(uint16_t addr, uint8_t v) { return write_reg(addr, &v, 1); }

static int rmw_reg8(uint16_t addr, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t v;
    int e = sx_read_reg(addr, &v, 1);
    if (e) return e;
    v = (uint8_t)((v & (uint8_t)~clear_mask) | set_mask);
    return write_reg8(addr, v);
}

static uint16_t get_irq(void)
{
    uint8_t tx[4] = { OP_GET_IRQ_STATUS, 0, 0, 0 }, rx[4] = {0};
    if (xfer(tx, rx, 4)) return 0;
    return (uint16_t)((rx[2] << 8) | rx[3]);
}

static int clear_irq(uint16_t mask)
{
    uint8_t a[2] = { (uint8_t)(mask >> 8), (uint8_t)mask };
    return cmd(OP_CLEAR_IRQ_STATUS, a, 2);
}

/* freq_hz * 2^25 / 32 MHz computed in 32-bit arithmetic (no 64-bit division needed). */
static uint32_t freq_to_frf(uint32_t f)
{
    uint32_t q = f / 32000000u, r = f % 32000000u;
    uint32_t a = r / 15625u,    b = r % 15625u;   /* 2^25/32e6 = 2^14/15625 */
    return (q << 25) + (a << 14) + ((b << 14) / 15625u);
}

static uint8_t bw_code(uint16_t bw_khz)
{
    switch (bw_khz) {
    case 125: return 0x04;
    case 250: return 0x05;
    case 500: return 0x06;
    default:  return 0xFF;
    }
}

/* Symbol time above 16 ms needs Low Data Rate Optimisation. */
static uint8_t ldro_needed(uint8_t sf, uint16_t bw_khz)
{
    uint32_t tsym_us = ((uint32_t)1u << sf) * 1000u / bw_khz;
    return tsym_us > 16000u ? 1u : 0u;
}

static int set_packet_params(uint8_t payload_len)
{
    uint8_t a[6] = {
        (uint8_t)(s_cfg.preamble >> 8), (uint8_t)s_cfg.preamble,
        0x00,                 /* explicit (variable length) header */
        payload_len,
        0x01,                 /* CRC on */
        0x00                  /* standard IQ */
    };
    return cmd(OP_SET_PACKET_PARAMS, a, 6);
}

/* ---------------------------------------------------------------- public */

int sx_reset(void)
{
    rf_nss(true);
    rf_reset(false);
    delay_ms(2);              /* datasheet minimum is 100 us */
    rf_reset(true);
    delay_ms(10);
    return wait_busy(100);
}

int sx_selftest(void)
{
    uint8_t sw[2] = {0, 0};
    int e = sx_read_reg(REG_LORA_SYNC_MSB, sw, 2);
    if (e) return e;
    /* Reset value of the LoRa sync word registers is 0x14 0x24. */
    return (sw[0] == 0x14 && sw[1] == 0x24) ? SX_OK : SX_ERR_SELFTEST;
}

int sx_standby(void)
{
    uint8_t a = 0x00;         /* STDBY_RC */
    return cmd(OP_SET_STANDBY, &a, 1);
}

int sx_set_modulation(uint8_t sf, uint16_t bw_khz, uint8_t cr)
{
    uint8_t bw = bw_code(bw_khz);
    if (sf < 7 || sf > 12 || bw == 0xFF || cr < 1 || cr > 4) return SX_ERR_PARAM;
    uint8_t a[4] = { sf, bw, cr, ldro_needed(sf, bw_khz) };
    int e = cmd(OP_SET_MODULATION_PARAMS, a, 4);
    if (e) return e;
    s_cfg.sf = sf; s_cfg.bw_khz = bw_khz; s_cfg.cr = cr;
    return SX_OK;
}

int sx_set_power(int8_t dbm)
{
    if (dbm < -9)  dbm = -9;
    if (dbm > 22)  dbm = 22;
    uint8_t a[2] = { (uint8_t)dbm, 0x04 /* ramp 200 us */ };
    int e = cmd(OP_SET_TX_PARAMS, a, 2);
    if (e) return e;
    s_cfg.power_dbm = dbm;
    return SX_OK;
}

int sx_init(const sx_config_t *cfg)
{
    int e;
    s_cfg = *cfg;

    if ((e = sx_reset()))   return e;
    if ((e = sx_standby())) return e;

    if (cfg->use_tcxo) {
        /* TCXO supply on DIO3, 5 ms start-up (320 x 15.625 us). */
        uint8_t a[4] = { cfg->tcxo_voltage, 0x00, 0x01, 0x40 };
        if ((e = cmd(OP_SET_DIO3_AS_TCXO_CTRL, a, 4))) return e;
    }

    /* Calibrate all blocks (needed after enabling the TCXO), then clear the
     * XOSC start error that is always latched at power-up with a TCXO. */
    {
        uint8_t a = 0x7F;
        if ((e = cmd(OP_CALIBRATE, &a, 1))) return e;
        if ((e = wait_busy(50))) return e;
        sx_clear_errors();
    }

    { uint8_t a = cfg->use_dcdc ? 0x01 : 0x00; if ((e = cmd(OP_SET_REGULATOR_MODE, &a, 1))) return e; }
    { uint8_t a = 0x01;                        if ((e = cmd(OP_SET_DIO2_AS_RF_SWITCH, &a, 1))) return e; }
    { uint8_t a = 0x01; /* LoRa */             if ((e = cmd(OP_SET_PACKET_TYPE, &a, 1))) return e; }

    {
        uint32_t frf = freq_to_frf(cfg->freq_hz);
        uint8_t a[4] = { (uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf };
        if ((e = cmd(OP_SET_RF_FREQUENCY, a, 4))) return e;
    }
    {
        /* Image calibration for the 863-870 MHz band. */
        uint8_t a[2] = { 0xD7, 0xDB };
        if ((e = cmd(OP_CALIBRATE_IMAGE, a, 2))) return e;
        if ((e = wait_busy(50))) return e;
    }
    {
        /* SX1262 high-power PA, optimal settings for +22 dBm. */
        uint8_t a[4] = { 0x04, 0x07, 0x00, 0x01 };
        if ((e = cmd(OP_SET_PA_CONFIG, a, 4))) return e;
    }
    if ((e = sx_set_power(cfg->power_dbm))) return e;
    if ((e = write_reg8(REG_OCP, 0x38))) return e;                 /* 140 mA current limit */
    if ((e = rmw_reg8(REG_TX_CLAMP, 0x00, 0x1E))) return e;        /* errata 15.2 */
    if ((e = rmw_reg8(REG_IQ_POLARITY, 0x00, 0x04))) return e;     /* errata 15.4, standard IQ */

    { uint8_t a[2] = { 0x00, 0x00 }; if ((e = cmd(OP_SET_BUFFER_BASE_ADDR, a, 2))) return e; }

    if ((e = sx_set_modulation(cfg->sf, cfg->bw_khz, cfg->cr))) return e;
    if ((e = set_packet_params(255))) return e;

    { uint8_t sw[2] = { 0x14, 0x24 }; if ((e = write_reg(REG_LORA_SYNC_MSB, sw, 2))) return e; } /* private network */
    if ((e = write_reg8(REG_RX_GAIN, 0x96))) return e;             /* boosted RX gain */

    {
        uint8_t a[8] = {
            (uint8_t)(IRQ_USED >> 8), (uint8_t)IRQ_USED,   /* IRQ mask  */
            (uint8_t)(IRQ_USED >> 8), (uint8_t)IRQ_USED,   /* DIO1 mask */
            0, 0, 0, 0                                     /* DIO2, DIO3 unused */
        };
        if ((e = cmd(OP_SET_DIO_IRQ_PARAMS, a, 8))) return e;
    }
    clear_irq(0xFFFF);
    return sx_standby();
}

int sx_transmit(const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
    int e;
    if ((e = sx_standby())) return e;
    if ((e = set_packet_params(len))) return e;

    {
        uint8_t buf[2 + 255];
        buf[0] = OP_WRITE_BUFFER;
        buf[1] = 0x00;                                     /* offset */
        for (uint8_t i = 0; i < len; i++) buf[2 + i] = data[i];
        if ((e = xfer(buf, 0, (uint16_t)(2u + len)))) return e;
    }

    clear_irq(0xFFFF);
    {
        uint32_t t = timeout_ms * 64u;                     /* 15.625 us units */
        if (t > 0xFFFFFFu) t = 0xFFFFFFu;
        uint8_t a[3] = { (uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t };
        if ((e = cmd(OP_SET_TX, a, 3))) return e;
    }

    uint32_t t0 = millis();
    for (;;) {
        if (rf_dio1()) {
            uint16_t irq = get_irq();
            clear_irq(irq);
            if (irq & IRQ_TX_DONE) return SX_OK;
            if (irq & IRQ_TIMEOUT) return SX_ERR_TIMEOUT;
        }
        if ((uint32_t)(millis() - t0) > timeout_ms + 100u) {
            sx_standby();
            return SX_ERR_TIMEOUT;
        }
    }
}

int sx_rx_start(uint32_t timeout_ms)
{
    int e;
    if ((e = sx_standby())) return e;
    if ((e = set_packet_params(255))) return e;
    clear_irq(0xFFFF);

    uint32_t t;
    if (timeout_ms == 0) {
        t = 0xFFFFFFu;                                     /* continuous RX */
    } else {
        t = timeout_ms * 64u;
        if (t >= 0xFFFFFFu) t = 0xFFFFFEu;
    }
    uint8_t a[3] = { (uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t };
    return cmd(OP_SET_RX, a, 3);
}

int sx_rx_poll(uint8_t *buf, uint8_t *len, uint8_t maxlen, sx_pkt_status_t *st)
{
    if (!rf_dio1()) return 0;

    uint16_t irq = get_irq();
    clear_irq(irq);

    if (irq & IRQ_TIMEOUT) return SX_ERR_TIMEOUT;
    if (!(irq & (IRQ_RX_DONE | IRQ_HEADER_ERR))) return 0;

    /* Packet status: RSSI and SNR of the last packet. */
    {
        uint8_t tx[5] = { OP_GET_PACKET_STATUS, 0, 0, 0, 0 }, rx[5] = {0};
        if (xfer(tx, rx, 5) == SX_OK && st) {
            st->rssi_dbm        = (int16_t)(-(int16_t)rx[2] / 2);
            st->snr_q4          = (int8_t)rx[3];
            st->signal_rssi_dbm = (int16_t)(-(int16_t)rx[4] / 2);
        }
    }

    if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) return SX_ERR_CRC;

    uint8_t plen, start;
    {
        uint8_t tx[4] = { OP_GET_RX_BUFFER_STATUS, 0, 0, 0 }, rx[4] = {0};
        if (xfer(tx, rx, 4)) return SX_ERR_BUSY;
        plen = rx[2];
        start = rx[3];
    }
    if (plen > maxlen) plen = maxlen;

    {
        uint8_t tx[3 + 255] = {0}, rx[3 + 255];
        tx[0] = OP_READ_BUFFER;
        tx[1] = start;
        /* tx[2] = NOP (status) */
        if (xfer(tx, rx, (uint16_t)(3u + plen))) return SX_ERR_BUSY;
        for (uint8_t i = 0; i < plen; i++) buf[i] = rx[3 + i];
    }
    *len = plen;
    return 1;
}

uint8_t sx_get_status(void)
{
    uint8_t tx[2] = { OP_GET_STATUS, 0 }, rx[2] = {0};
    if (xfer(tx, rx, 2)) return 0;
    return rx[1];
}

uint16_t sx_get_errors(void)
{
    uint8_t tx[4] = { OP_GET_DEVICE_ERRORS, 0, 0, 0 }, rx[4] = {0};
    if (xfer(tx, rx, 4)) return 0xFFFF;
    return (uint16_t)((rx[2] << 8) | rx[3]);
}

void sx_clear_errors(void)
{
    uint8_t a[2] = { 0, 0 };
    cmd(OP_CLEAR_DEVICE_ERRORS, a, 2);
}

const char *sx_chip_mode_str(uint8_t status)
{
    switch ((status >> 4) & 7u) {
    case 2: return "STBY_RC";
    case 3: return "STBY_XOSC";
    case 4: return "FS";
    case 5: return "RX";
    case 6: return "TX";
    default: return "UNKNOWN";
    }
}

const char *sx_cmd_status_str(uint8_t status)
{
    switch ((status >> 1) & 7u) {
    case 2: return "data available";
    case 3: return "command timeout";
    case 4: return "command processing error";
    case 5: return "failure to execute";
    case 6: return "tx done";
    default: return "ok";
    }
}

uint32_t sx_time_on_air_us(uint8_t sf, uint16_t bw_khz, uint8_t cr,
                           uint16_t preamble, uint8_t payload_len)
{
    uint32_t tsym_us = ((uint32_t)1u << sf) * 1000u / bw_khz;
    int32_t  de      = ldro_needed(sf, bw_khz);
    int32_t  num     = 8 * (int32_t)payload_len - 4 * (int32_t)sf + 28 + 16 /* CRC on */ - 0 /* explicit header */;
    int32_t  den     = 4 * ((int32_t)sf - 2 * de);
    int32_t  ceil_   = num > 0 ? (num + den - 1) / den : 0;
    uint32_t n_payload = 8u + (uint32_t)(ceil_ * ((int32_t)cr + 4));
    /* preamble time = (n_preamble + 4.25) * Tsym */
    uint32_t t_pre = ((4u * preamble + 17u) * tsym_us) / 4u;
    return t_pre + n_payload * tsym_us;
}
