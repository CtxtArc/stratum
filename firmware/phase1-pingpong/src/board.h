/*
 * STRATUM Phase 1 — board support for NUCLEO-U545RE-Q (MB1841) + Waveshare Core1262-868M
 *
 * Pin map (see README.md for the wiring table):
 *   USART1  PA9 TX / PA10 RX  -> ST-LINK virtual COM port (115200 8N1)
 *   SPI1    PA5 SCK  (Arduino D13, shared with green LED LD2 — it flickers during SPI, that's normal)
 *           PA6 MISO (D12)
 *           PA7 MOSI (D11)
 *   PC9  -> SX1262 NSS    (D10, GPIO output)
 *   PB10 -> SX1262 NRESET (D6,  GPIO output)
 *   PB5  <- SX1262 BUSY   (D4,  GPIO input)
 *   PB4  <- SX1262 DIO1   (D5,  GPIO input)
 *   PC13 <- user button B1 (blue)
 *
 * Clock: HSI16 -> SYSCLK = HCLK = PCLK2 = 16 MHz. SPI1 at 4 MHz.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include <stdbool.h>

#define CPU_HZ        16000000u
#define UART_BAUD     115200u

void     board_init(void);

uint32_t millis(void);
void     delay_ms(uint32_t ms);
void     delay_us(uint32_t us);

/* Console (ST-LINK VCP) */
void     uart_putc(char c);
void     uart_write(const char *s, uint32_t n);
int      uart_getc(void);               /* -1 when nothing received */

/* Radio interface */
void     rf_nss(bool high);
void     rf_reset(bool high);
bool     rf_busy(void);
bool     rf_dio1(void);
void     spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t n);

/* Misc */
bool     button_pressed(void);           /* raw level, true = pressed */
uint16_t board_node_id(void);            /* 16-bit hash of the 96-bit chip UID */

#endif
