/*
 * STRATUM Phase 1 — SX1262 bring-up and ping-pong range test.
 *
 * Same firmware on every board. Each board starts as a RESPONDER (listens, answers pings).
 * Press the blue button or type 'p' on one board to make it the PING initiator.
 * Type 'n' on a third board to make it a SNIFFER (logs everything, never transmits).
 *
 * Console: ST-LINK virtual COM port, 115200 8N1. Type 'h' for help.
 * Every measurement is also printed as a CSV line starting with "CSV," (grep it into a file).
 */
#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "sx126x.h"
#include "fmt.h"

#define FW_VERSION      "phase1-pingpong 0.1"

#define FREQ_HZ         869525000u   /* CH-A, EU 869.4-869.65 MHz sub-band (500 mW ERP, 10 % duty) */
#define PKT_LEN         16u
#define DC_OFF_FACTOR   19u          /* after a TX of T ms, stay silent 19*T ms -> <= 5 % duty  */
#define PONG_DELAY_MS   30u
#define BENCH_POWER     0            /* dBm, safe for boards on the same desk                  */
#define FIELD_POWER     22           /* dBm, maximum of the SX1262 high-power PA                 */

enum { T_PING = 1, T_PONG = 2 };
typedef enum { ROLE_RESPONDER, ROLE_PING, ROLE_SNIFF } role_t;

static sx_config_t s_cfg = {
    .freq_hz      = FREQ_HZ,
    .power_dbm    = BENCH_POWER,
    .sf           = 9,
    .bw_khz       = 125,
    .cr           = 1,               /* 4/5 */
    .preamble     = 8,
    .use_tcxo     = true,            /* Core1262: TCXO powered from DIO3 */
    .tcxo_voltage = SX_TCXO_1V8,
    .use_dcdc     = false,           /* LDO: always works, DC-DC needs the module inductor */
};

static uint16_t s_node;
static role_t   s_role = ROLE_RESPONDER;
static uint32_t s_seq;
static uint32_t s_interval_ms = 5000;
static uint32_t s_next_ping_ms;
static bool     s_radio_ok;

/* statistics */
static uint32_t st_ping_sent, st_pong_ok, st_pong_lost, st_crc, st_dc_hold, st_pings_heard;

/* duty-cycle guard */
static uint32_t s_dc_next_ok_ms;
static uint32_t s_dc_bucket_ms[60], s_dc_bucket_min[60];

static const char *const k_role_name[] = { "RESPONDER", "PING", "SNIFFER" };

/* ------------------------------------------------------------------ helpers */

static uint32_t toa_ms(uint8_t len)
{
    return sx_time_on_air_us(s_cfg.sf, s_cfg.bw_khz, s_cfg.cr, s_cfg.preamble, len) / 1000u + 1u;
}

static void timestamp(void)
{
    uint32_t t = millis();
    out("[%6u.%03u] ", t / 1000u, t % 1000u);
}

static bool dc_ok(void) { return (int32_t)(millis() - s_dc_next_ok_ms) >= 0; }

static void dc_account(uint32_t airtime_ms)
{
    uint32_t now = millis(), m = now / 60000u, i = m % 60u;
    if (s_dc_bucket_min[i] != m) { s_dc_bucket_min[i] = m; s_dc_bucket_ms[i] = 0; }
    s_dc_bucket_ms[i] += airtime_ms;
    s_dc_next_ok_ms = now + airtime_ms * DC_OFF_FACTOR;
}

static uint32_t dc_last_hour_ms(void)
{
    uint32_t m = millis() / 60000u, sum = 0;
    for (int i = 0; i < 60; i++)
        if ((uint32_t)(m - s_dc_bucket_min[i]) < 60u) sum += s_dc_bucket_ms[i];
    return sum;
}

static void print_errors(uint16_t err)
{
    static const char *const names[9] = {
        "RC64K_CALIB", "RC13M_CALIB", "PLL_CALIB", "ADC_CALIB", "IMG_CALIB",
        "XOSC_START", "PLL_LOCK", "?", "PA_RAMP"
    };
    if (err == 0) { out("none"); return; }
    for (int b = 0; b < 9; b++) if (err & (1u << b)) out("%s ", names[b]);
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/*
 * Test packet, 16 bytes:
 *  0 'S'  1 'T'  2 type  3 version(1)  4-5 node id  6-9 seq
 * 10 sf  11 tx power (int8)  12-13 rssi of the ping as heard (pong only, int16)
 * 14 snr of the ping in quarter dB (pong only, int8)  15 reserved
 */
static void build_pkt(uint8_t *p, uint8_t type, uint32_t seq, int16_t rssi, int8_t snr_q4)
{
    for (unsigned i = 0; i < PKT_LEN; i++) p[i] = 0;
    p[0] = 'S'; p[1] = 'T'; p[2] = type; p[3] = 1;
    put_u16(&p[4], s_node);
    put_u32(&p[6], seq);
    p[10] = s_cfg.sf;
    p[11] = (uint8_t)s_cfg.power_dbm;
    put_u16(&p[12], (uint16_t)rssi);
    p[14] = (uint8_t)snr_q4;
}

static bool pkt_valid(const uint8_t *p, uint8_t len)
{
    return len == PKT_LEN && p[0] == 'S' && p[1] == 'T' && p[3] == 1;
}

/* Transmit with the duty-cycle guard. Leaves the radio in standby. */
static int radio_send(const uint8_t *p, uint8_t len)
{
    if (!dc_ok()) { st_dc_hold++; return SX_ERR_TIMEOUT; }
    uint32_t t = toa_ms(len);
    int e = sx_transmit(p, len, t * 2u + 100u);
    dc_account(t);
    return e;
}

/* ------------------------------------------------------------------ radio setup */

static void radio_bringup(void)
{
    out("\n--- SX1262 bring-up ---\n");
    int e = sx_reset();
    if (e) {
        out("[FAIL] BUSY stays high after reset.\n"
            "       Check: 3V3 and GND to the module, NRESET wire (PB10 / D6), BUSY wire (PB5 / D4).\n");
        s_radio_ok = false;
        return;
    }
    out("[ OK ] reset, BUSY released\n");

    uint8_t sw[2] = {0, 0};
    sx_read_reg(0x0740, sw, 2);
    if (sx_selftest() != SX_OK) {
        out("[FAIL] SPI readback: sync word = 0x%02x%02x, expected 0x1424.\n", sw[0], sw[1]);
        out("       0x0000 or 0xFFFF: check SCK (PA5 / D13), MISO (PA6 / D12), MOSI (PA7 / D11), NSS (PC9 / D10).\n");
        s_radio_ok = false;
        return;
    }
    out("[ OK ] SPI readback: sync word 0x1424\n");

    e = sx_init(&s_cfg);
    if (e) {
        out("[FAIL] init returned %d\n", e);
        s_radio_ok = false;
        return;
    }
    uint16_t err = sx_get_errors();
    uint8_t  st  = sx_get_status();
    out("[ %s ] init done, chip mode %s, device errors: ", err ? "WARN" : " OK ", sx_chip_mode_str(st));
    print_errors(err);
    out("\n");
    if (err & (1u << 5))
        out("       XOSC_START: the oscillator did not start. Try another TCXO voltage ('v') or crystal mode ('o').\n");
    if (err & (1u << 6))
        out("       PLL_LOCK: frequency synthesizer did not lock (oscillator problem, same fixes as above).\n");
    s_radio_ok = (err & ((1u << 5) | (1u << 6))) == 0;
}

static void radio_listen(void)
{
    if (s_radio_ok) sx_rx_start(0);
}

/* ------------------------------------------------------------------ console */

static void print_help(void)
{
    out("\nCommands:\n"
        "  h  help                 i  info / status / statistics\n"
        "  p  become PING initiator (also: blue button)\n"
        "  r  become RESPONDER      n  become SNIFFER (never transmits)\n"
        "  s  cycle spreading factor SF7..SF12 (all boards must match)\n"
        "  +  / -  TX power +/- 1 dB    b  bench power (%d dBm)   f  field power (+%d dBm)\n"
        "  t  cycle ping interval 2/5/10/30 s\n"
        "  c  clear statistics      x  re-run radio bring-up\n"
        "  v  cycle TCXO voltage    o  toggle TCXO / crystal mode\n\n"
        "CSV columns: CSV,ms,event,seq,peer,sf,peer_power,rssi_up,snr_up,rssi_down,snr_down\n"
        "  up = ping as heard by the responder, down = pong as heard by the initiator (dBm, dB)\n",
        BENCH_POWER, FIELD_POWER);
}

static void print_info(void)
{
    static const char *const tcxo_v[8] = { "1.6", "1.7", "1.8", "2.2", "2.4", "2.7", "3.0", "3.3" };
    uint32_t t = toa_ms(PKT_LEN);
    uint32_t hour = dc_last_hour_ms();
    out("\n%s  node 0x%04x  role %s\n", FW_VERSION, s_node, k_role_name[s_role]);
    out("radio: %s  %u.%03u MHz  SF%u  BW%u  CR4/%u  %d dBm  oscillator: %s %s V\n",
        s_radio_ok ? "OK" : "NOT READY",
        s_cfg.freq_hz / 1000000u, (s_cfg.freq_hz / 1000u) % 1000u,
        s_cfg.sf, s_cfg.bw_khz, s_cfg.cr + 4u, s_cfg.power_dbm,
        s_cfg.use_tcxo ? "TCXO" : "crystal", s_cfg.use_tcxo ? tcxo_v[s_cfg.tcxo_voltage] : "-");
    out("airtime per 16-byte packet: %u ms -> min gap after each TX %u ms\n", t, t * DC_OFF_FACTOR);
    out("airtime last 60 min: %u ms = %u.%u %% (limit 10 %%, guard 5 %%)\n",
        hour, hour / 36000u, (hour / 3600u) % 10u);
    out("ping interval %u ms\n", s_interval_ms);
    if (s_radio_ok) {
        uint8_t st = sx_get_status();
        out("chip: mode %s, last command: %s, errors: ", sx_chip_mode_str(st), sx_cmd_status_str(st));
        print_errors(sx_get_errors());
        out("\n");
    }
    uint32_t done = st_pong_ok + st_pong_lost;
    out("stats: pings sent %u, pongs %u, lost %u, PDR %u %%, crc errors %u, pings heard %u, duty-cycle holds %u\n",
        st_ping_sent, st_pong_ok, st_pong_lost, done ? (st_pong_ok * 100u) / done : 0u,
        st_crc, st_pings_heard, st_dc_hold);
}

static void set_role(role_t r)
{
    s_role = r;
    timestamp();
    out("role -> %s\n", k_role_name[r]);
    s_next_ping_ms = millis();
    if (r != ROLE_PING) radio_listen();
}

static void apply_modulation(void)
{
    if (s_radio_ok) { sx_standby(); sx_set_modulation(s_cfg.sf, s_cfg.bw_khz, s_cfg.cr); radio_listen(); }
    timestamp();
    out("SF%u, airtime %u ms per packet\n", s_cfg.sf, toa_ms(PKT_LEN));
}

static void apply_power(int dbm)
{
    if (dbm < -9) dbm = -9;
    if (dbm > 22) dbm = 22;
    s_cfg.power_dbm = (int8_t)dbm;
    if (s_radio_ok) { sx_standby(); sx_set_power(s_cfg.power_dbm); if (s_role != ROLE_PING) radio_listen(); }
    timestamp();
    out("TX power %d dBm%s\n", s_cfg.power_dbm,
        s_cfg.power_dbm > 10 ? "  (keep boards > 2 m apart, antennas fitted)" : "");
}

static void handle_console(void)
{
    int c = uart_getc();
    if (c < 0) return;
    switch (c) {
    case 'h': case '?': print_help(); break;
    case 'i': print_info(); break;
    case 'p': set_role(ROLE_PING); break;
    case 'r': set_role(ROLE_RESPONDER); break;
    case 'n': set_role(ROLE_SNIFF); break;
    case 's': s_cfg.sf = (uint8_t)(s_cfg.sf >= 12 ? 7 : s_cfg.sf + 1); apply_modulation(); break;
    case '+': apply_power(s_cfg.power_dbm + 1); break;
    case '-': apply_power(s_cfg.power_dbm - 1); break;
    case 'b': apply_power(BENCH_POWER); break;
    case 'f': apply_power(FIELD_POWER); break;
    case 't':
        s_interval_ms = s_interval_ms == 2000u ? 5000u : s_interval_ms == 5000u ? 10000u
                      : s_interval_ms == 10000u ? 30000u : 2000u;
        timestamp(); out("ping interval %u ms\n", s_interval_ms);
        break;
    case 'c':
        st_ping_sent = st_pong_ok = st_pong_lost = st_crc = st_dc_hold = st_pings_heard = 0;
        timestamp(); out("statistics cleared\n");
        break;
    case 'x': radio_bringup(); radio_listen(); break;
    case 'v':
        s_cfg.use_tcxo = true;
        s_cfg.tcxo_voltage = (uint8_t)((s_cfg.tcxo_voltage + 1u) & 7u);
        radio_bringup(); radio_listen();
        break;
    case 'o':
        s_cfg.use_tcxo = !s_cfg.use_tcxo;
        radio_bringup(); radio_listen();
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ roles */

static void on_packet(const uint8_t *p, uint8_t len, const sx_pkt_status_t *st)
{
    if (s_role == ROLE_SNIFF) {
        timestamp();
        out("SNIFF len %u rssi %d dBm snr ", len, st->rssi_dbm);
        out_q4(st->snr_q4);
        out(" dB :");
        for (uint8_t i = 0; i < len; i++) out(" %02x", p[i]);
        if (pkt_valid(p, len))
            out("  [%s #%u from 0x%04x]", p[2] == T_PING ? "PING" : "PONG", get_u32(&p[6]), get_u16(&p[4]));
        out("\n");
        out("CSV,%u,SNIFF,%u,%04x,%u,%d,%d,%d,,\n", millis(),
            pkt_valid(p, len) ? get_u32(&p[6]) : 0u, pkt_valid(p, len) ? get_u16(&p[4]) : 0u,
            s_cfg.sf, pkt_valid(p, len) ? (int8_t)p[11] : 0, st->rssi_dbm, st->snr_q4);
        return;
    }

    if (!pkt_valid(p, len)) {
        timestamp(); out("foreign packet, %u bytes, rssi %d dBm (ignored)\n", len, st->rssi_dbm);
        return;
    }

    if (s_role == ROLE_RESPONDER && p[2] == T_PING) {
        uint32_t seq = get_u32(&p[6]);
        uint16_t peer = get_u16(&p[4]);
        st_pings_heard++;
        timestamp();
        out("PING #%u from 0x%04x  rssi %d dBm  snr ", seq, peer, st->rssi_dbm);
        out_q4(st->snr_q4);
        out(" dB\n");
        out("CSV,%u,PING_RX,%u,%04x,%u,%d,%d,%d,,\n", millis(), seq, peer, s_cfg.sf,
            (int8_t)p[11], st->rssi_dbm, st->snr_q4);

        uint8_t pong[PKT_LEN];
        build_pkt(pong, T_PONG, seq, st->rssi_dbm, st->snr_q4);
        delay_ms(PONG_DELAY_MS);
        int e = radio_send(pong, PKT_LEN);
        if (e) { timestamp(); out("pong not sent (%s)\n", dc_ok() ? "radio error" : "duty-cycle guard"); }
        radio_listen();
    }
}

static void responder_or_sniffer_step(void)
{
    uint8_t buf[255], len = 0;
    sx_pkt_status_t st = {0};
    int r = sx_rx_poll(buf, &len, sizeof buf, &st);
    if (r == 1) {
        on_packet(buf, len, &st);
    } else if (r == SX_ERR_CRC) {
        st_crc++;
        timestamp(); out("CRC/header error, rssi %d dBm\n", st.rssi_dbm);
        out("CSV,%u,CRC_ERR,,,%u,,%d,%d,,\n", millis(), s_cfg.sf, st.rssi_dbm, st.snr_q4);
    }
}

static void ping_step(void)
{
    if ((int32_t)(millis() - s_next_ping_ms) < 0) return;

    if (!dc_ok()) {               /* wait for the duty-cycle guard instead of skipping */
        s_next_ping_ms = s_dc_next_ok_ms;
        return;
    }

    uint8_t pkt[PKT_LEN];
    uint32_t seq = ++s_seq;
    build_pkt(pkt, T_PING, seq, 0, 0);
    if (radio_send(pkt, PKT_LEN) != SX_OK) {
        timestamp(); out("PING #%u: transmit failed\n", seq);
        s_next_ping_ms = millis() + s_interval_ms;
        return;
    }
    st_ping_sent++;

    uint32_t wait = PONG_DELAY_MS + toa_ms(PKT_LEN) + 300u;
    sx_rx_start(wait);
    uint32_t t0 = millis();
    bool got = false;

    while ((uint32_t)(millis() - t0) < wait + 100u) {
        uint8_t buf[255], len = 0;
        sx_pkt_status_t st = {0};
        int r = sx_rx_poll(buf, &len, sizeof buf, &st);
        if (r == 0) continue;
        if (r == SX_ERR_TIMEOUT) break;
        if (r == SX_ERR_CRC) { st_crc++; continue; }
        if (!pkt_valid(buf, len) || buf[2] != T_PONG || get_u32(&buf[6]) != seq) continue;

        int16_t rssi_up = (int16_t)get_u16(&buf[12]);
        int8_t  snr_up  = (int8_t)buf[14];
        uint16_t peer   = get_u16(&buf[4]);
        st_pong_ok++;
        got = true;

        uint32_t done = st_pong_ok + st_pong_lost;
        timestamp();
        out("PING #%u -> PONG from 0x%04x   up %d dBm / ", seq, peer, rssi_up);
        out_q4(snr_up);
        out(" dB   down %d dBm / ", st.rssi_dbm);
        out_q4(st.snr_q4);
        out(" dB   SF%u %d dBm   PDR %u/%u\n", s_cfg.sf, s_cfg.power_dbm, st_pong_ok, done);
        out("CSV,%u,PONG,%u,%04x,%u,%d,%d,%d,%d,%d\n", millis(), seq, peer, s_cfg.sf,
            (int8_t)buf[11], rssi_up, snr_up, st.rssi_dbm, st.snr_q4);
        break;
    }

    if (!got) {
        st_pong_lost++;
        uint32_t done = st_pong_ok + st_pong_lost;
        timestamp();
        out("PING #%u -> no answer   PDR %u/%u\n", seq, st_pong_ok, done);
        out("CSV,%u,LOST,%u,,%u,,,,,\n", millis(), seq, s_cfg.sf);
    }

    sx_standby();
    s_next_ping_ms = millis() + s_interval_ms;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    board_init();
    delay_ms(50);
    s_node = board_node_id();

    out("\n\n=== STRATUM %s ===\n", FW_VERSION);
    out("node id 0x%04x, %u.%03u MHz, starting as RESPONDER. Type 'h' for help.\n",
        s_node, FREQ_HZ / 1000000u, (FREQ_HZ / 1000u) % 1000u);
    out("Never transmit without an antenna. TX power starts at %d dBm (bench).\n", BENCH_POWER);

    radio_bringup();
    radio_listen();
    print_help();

    bool btn_prev = false;
    uint32_t btn_t = 0;

    for (;;) {
        handle_console();

        bool b = button_pressed();
        if (b && !btn_prev && (uint32_t)(millis() - btn_t) > 200u) {
            btn_t = millis();
            set_role(s_role == ROLE_PING ? ROLE_RESPONDER : ROLE_PING);
        }
        btn_prev = b;

        if (!s_radio_ok) continue;

        if (s_role == ROLE_PING) ping_step();
        else                     responder_or_sniffer_step();
    }
}
