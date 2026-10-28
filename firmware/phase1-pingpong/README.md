# STRATUM Phase 1 — SX1262 bring-up and range test

Firmware for **NUCLEO-U545RE-Q + Waveshare Core1262-868M**: brings the SX1262 up, runs a ping-pong link test, and logs RSSI/SNR so you get real range numbers (spec §15, test T-001).

- One firmware for every board. Each board starts as a **responder**; you make one the **pinger**, and optionally a third one the **sniffer**.
- Register-level C, CMSIS headers only. No HAL, no libc, no vendor radio stack. Every SX1262 command is written from the datasheet in `src/sx126x.c`.
- Duty cycle is enforced in firmware: after every transmission of T ms the board stays silent for 19×T, which caps it at 5 % (the CH-A sub-band limit is 10 %).

---

## 1. Before the boards arrive

### Install (Arch)

```bash
sudo pacman -S arm-none-eabi-gcc arm-none-eabi-binutils openocd picocom git make
sudo usermod -aG uucp $USER      # access to /dev/ttyACM*; log out and back in
```

Optional: `probe-rs` (AUR, or `cargo install probe-rs-tools`), or STM32CubeProgrammer for `make flash-cube`. `arm-none-eabi-newlib` is **not** needed: the firmware doesn't use a C library.

### Build

```bash
make deps     # fetches ST's CMSIS device headers + Arm CMSIS core, pinned commits
make          # -> build/stratum-phase1.elf / .bin
```

Expected output ends with a size line around 14–15 KB of `text`. `make TOOLCHAIN=clang` builds with clang + lld instead.

`prebuilt/` holds an image built with clang from this exact source, in case you want to flash before your toolchain is set up:

```bash
openocd -f interface/stlink.cfg -f target/stm32u5x.cfg -c "program prebuilt/stratum-phase1.elf verify reset exit"
```

---

## 2. Wiring

**Power everything off while wiring. Fit the antenna before the module is ever powered.**

| Core1262 pin | Nucleo Arduino pin | STM32 pin | Morpho | Notes |
|---|---|---|---|---|
| VCC | **3V3** (CN6 pin 4) | — | CN7-16 | **Never 5V** |
| GND | GND (CN6 pin 6) | — | — | |
| SCK | D13 | PA5 | CN10-11 | Shared with green LED LD2: it flickers during SPI, normal |
| MISO | D12 | PA6 | CN10-13 | |
| MOSI | D11 | PA7 | CN10-15 | |
| NSS / CS | D10 | PC9 | CN10-17 | |
| RESET / NRST | D6 | PB10 | CN10-25 | |
| BUSY | D4 | PB5 | CN10-29 | |
| DIO1 / IRQ | D5 | PB4 | CN10-27 | |
| DIO2, DIO3 | not connected | — | — | Used inside the module (RF switch, TCXO supply) |
| ANT (u.FL) | pigtail → SMA antenna | — | — | Fix the SMA end to the adapter board |

Pin names on the Core1262 may differ slightly. Match them against the module's silkscreen or the Waveshare wiki pinout. Keep SPI wires short (< 15 cm).

The console is the ST-LINK USB-C port (CN1): it powers the board, flashes it, and appears as `/dev/ttyACM0` at 115200 8N1.

---

## 3. Bring-up, step by step

### Step 1 — Flash a bare Nucleo (no radio connected)

```bash
make flash          # or: make flash-pr / make flash-cube
make term           # picocom on /dev/ttyACM0, exit with Ctrl-A Ctrl-X
```

Press the black RESET button. You should see the banner:

```
=== STRATUM phase1-pingpong 0.1 ===
node id 0x3f2a, 869.525 MHz, starting as RESPONDER. Type 'h' for help.
--- SX1262 bring-up ---
[FAIL] ...
```

A `[FAIL]` here is **expected** (no radio attached). This step proves that the toolchain, flashing and console all work.

### Step 2 — Connect the radio and read it back

Power off, wire the module (section 2), fit the antenna, power on. Expected:

```
[ OK ] reset, BUSY released
[ OK ] SPI readback: sync word 0x1424
[  OK  ] init done, chip mode STBY_RC, device errors: none
```

| You see | Meaning | Fix |
|---|---|---|
| `BUSY stays high after reset` | The chip never came out of reset | Check 3V3 + GND to the module, the NRESET wire (D6), the BUSY wire (D4) |
| `SPI readback ... 0x0000` or `0xFFFF` | No SPI traffic | Check SCK/MISO/MOSI/NSS, especially MISO ↔ MOSI swapped |
| Another value | Bits slipping | Shorter wires, common ground |
| `XOSC_START` | The oscillator didn't start | Press `v` to try the next TCXO voltage, or `o` to switch to crystal mode |
| `PLL_LOCK` | Synthesizer didn't lock | Same causes as XOSC_START |

Default settings are TCXO at 1.8 V on DIO3 and the RF switch on DIO2 (what Waveshare's Core1262 uses), with the LDO regulator. Type `i` at any time for the full status.

### Step 3 — Bench link test (two boards)

Boards 1–2 m apart, antennas vertical and parallel, power at **0 dBm** (the boot default, so the receivers aren't overloaded).

1. Open a terminal on each board (`make term PORT=/dev/ttyACM0`, then `PORT=/dev/ttyACM1` in a second window).
2. On board A, press the **blue button** or type `p` to make it the pinger.
3. Board A prints one line every 5 s:

```
[    12.345] PING #3 -> PONG from 0x8c11   up -31 dBm / 9.50 dB   down -30 dBm / 9.75 dB   SF9 0 dBm   PDR 3/3
```

- **up**: how board B heard the ping. **down**: how board A heard the pong.
- At 1–2 m and 0 dBm, expect about −25 to −45 dBm with SNR +8 to +12 dB, and **PDR 100 %**.
- Board B prints `PING #3 from 0x...` for each packet it answers.

That's test **T-001 (bench)**.

### Step 4 — Sniffer (third board)

Type `n` on the third board. It logs every packet on the channel in hex with RSSI/SNR and never transmits. Leave it running during every test from now on: when a ping is lost, it tells you whether the ping or the pong went missing.

### Step 5 — Outdoor range test

1. On **both** boards: `f` (field power, +22 dBm) and the same SF. Change it with `s`: SF7 → … → SF12 → SF7.
2. Log the pinger's console to a file:

   ```bash
   picocom -b 115200 /dev/ttyACM0 --logfile run-sf9.log
   grep '^CSV' run-sf9.log > run-sf9.csv
   ```

3. The responder can run from a USB power bank on the ST-LINK port. Some power banks switch off at low current (~40 mA); use one with an "always on / low current" mode.
4. Walk away in steps (100 m, 250 m, 500 m, 1 km, 2 km, …), staying ~1 min at each step, and note distance and time. Hold the antennas vertical at the same height.
5. Repeat at SF7 and SF12. SF12 transmits for ~1.3 s per packet, so the duty-cycle guard spaces pings ~25 s apart; set the interval to 30 s with `t`.

CSV columns:

```
CSV,ms,event,seq,peer,sf,peer_power,rssi_up,snr_up,rssi_down,snr_down
```

`event` is one of `PONG`, `LOST`, `PING_RX` (responder side), `SNIFF` or `CRC_ERR`. SNR values in the CSV are in **quarter dB** (divide by 4).

The link limit is roughly where SNR reaches the floor for the SF: about −7.5 dB at SF7, −12.5 dB at SF9 and −20 dB at SF12.

---

## 4. Console commands

| Key | Action |
|---|---|
| `h` | Help |
| `i` | Info: radio config, airtime, duty cycle over the last hour, chip status, statistics |
| `p` / blue button | Become pinger (button toggles pinger ↔ responder) |
| `r` | Become responder (default) |
| `n` | Become sniffer (never transmits) |
| `s` | Cycle SF7 … SF12; all boards must use the same SF |
| `+` / `-` | TX power ±1 dB (−9 … +22 dBm) |
| `b` / `f` | Bench power (0 dBm) / field power (+22 dBm) |
| `t` | Ping interval 2 / 5 / 10 / 30 s (the duty-cycle guard can stretch it) |
| `c` | Clear statistics |
| `x` | Re-run the radio bring-up |
| `v` / `o` | Cycle TCXO voltage / toggle TCXO ↔ crystal (oscillator troubleshooting) |

---

## 5. Radio settings

| Parameter | Value | Why |
|---|---|---|
| Frequency | 869.525 MHz | CH-A of the spec: the EU sub-band with 500 mW ERP and 10 % duty cycle |
| Bandwidth | 125 kHz | Fits inside the 869.40–869.65 MHz sub-band |
| SF | 9 (7–12 selectable) | Spec default |
| CR | 4/5 | |
| Preamble | 8 symbols | |
| Header / CRC | Explicit / on | |
| Sync word | 0x1424 (private) | Keeps clear of public LoRaWAN traffic |
| Packet | 16 bytes | Airtime: SF7 52 ms, SF9 165 ms, SF12 1.32 s |
| PA | High-power PA, +22 dBm config, ramp 200 µs, OCP 140 mA | |
| RX | Boosted gain (reg 0x08AC = 0x96) | ~+1–2 dB sensitivity for ~0.7 mA more RX current |
| Errata | §15.2 TX clamp (0x08D8), §15.4 IQ polarity (0x0736) applied | |

Test packet layout (16 bytes, little-endian): `'S' 'T' type(1=ping,2=pong) version(1) node_id(2) seq(4) sf(1) tx_power(1) rssi_up(2, pong only) snr_up_q4(1, pong only) reserved(1)`.

---

## 6. Code map

| File | What it does |
|---|---|
| `src/board.c/.h` | Clock (HSI16, 16 MHz), GPIO, USART1 console, SPI1 (4 MHz, mode 0), SysTick, DWT µs delay, chip-UID node id |
| `src/sx126x.c/.h` | SX1262 driver: reset, self-test, init sequence, TX, RX, packet status, device errors, time-on-air |
| `src/main.c` | Roles (pinger / responder / sniffer), duty-cycle guard, console, CSV logging |
| `src/fmt.c` | Tiny printf for the console |
| `src/startup.c` | Vector table, `.data`/`.bss` init, `memcpy`/`memset` and ARM EABI helpers |
| `ld/stm32u545re.ld` | Linker script: 512 KB flash, 192 KB SRAM1 |
| `scripts/fetch-cmsis.sh` | Fetches the two header-only dependencies at pinned commits |

Verified before delivery (without hardware): it builds with no warnings, the vector table is correct, and the frequency-register and time-on-air math match an independent reference (including Semtech's 41.22 ms for 10 bytes at SF7/125 kHz). What still needs the real boards: SPI timing, the Core1262's actual TCXO voltage, and RF performance.

---

## 7. Safety and rules

- **Never transmit without an antenna**: reflected power at +22 dBm can damage the SX1262 PA.
- Use bench power (0 dBm) whenever boards are within a couple of metres of each other.
- +22 dBm with a 2 dBi antenna is about 24 dBm EIRP (~22 dBm ERP), inside the 27 dBm ERP (500 mW) allowed on 869.4–869.65 MHz.
- The firmware keeps each node at ≤ 5 % airtime; `i` shows the last hour's total.
