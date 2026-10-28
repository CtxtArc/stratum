/*
 * Register-level board support (CMSIS headers only, no HAL).
 */
#include "board.h"
#include "stm32u5xx.h"

/* ---------------------------------------------------------------- time */

static volatile uint32_t s_ms;

void SysTick_Handler(void) { s_ms++; }

uint32_t millis(void) { return s_ms; }

void delay_ms(uint32_t ms)
{
    uint32_t t0 = s_ms;
    while ((uint32_t)(s_ms - t0) < ms) { __NOP(); }
}

void delay_us(uint32_t us)
{
    uint32_t t0 = DWT->CYCCNT;
    uint32_t ticks = us * (CPU_HZ / 1000000u);
    while ((uint32_t)(DWT->CYCCNT - t0) < ticks) { __NOP(); }
}

/* ---------------------------------------------------------------- gpio */

enum { MODE_IN = 0, MODE_OUT = 1, MODE_AF = 2 };
enum { PULL_NONE = 0, PULL_UP = 1, PULL_DOWN = 2 };
enum { SPEED_LOW = 0, SPEED_MED = 1, SPEED_HIGH = 2, SPEED_VHIGH = 3 };

static void pin_cfg(GPIO_TypeDef *g, uint32_t pin, uint32_t mode, uint32_t af,
                    uint32_t pull, uint32_t speed)
{
    uint32_t s2 = pin * 2u;
    g->MODER   = (g->MODER   & ~(3u << s2)) | (mode  << s2);
    g->PUPDR   = (g->PUPDR   & ~(3u << s2)) | (pull  << s2);
    g->OSPEEDR = (g->OSPEEDR & ~(3u << s2)) | (speed << s2);
    g->OTYPER &= ~(1u << pin);                               /* push-pull */
    if (mode == MODE_AF) {
        uint32_t idx = pin >> 3, s4 = (pin & 7u) * 4u;
        g->AFR[idx] = (g->AFR[idx] & ~(0xFu << s4)) | (af << s4);
    }
}

static inline void pin_write(GPIO_TypeDef *g, uint32_t pin, bool high)
{
    g->BSRR = high ? (1u << pin) : (1u << (pin + 16u));
}

static inline bool pin_read(GPIO_TypeDef *g, uint32_t pin)
{
    return (g->IDR >> pin) & 1u;
}

/* ---------------------------------------------------------------- clock */

static void clock_init(void)
{
    /* Generous flash wait states before raising the clock (safe at any VOS). */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | (2u << FLASH_ACR_LATENCY_Pos);
    while (((FLASH->ACR & FLASH_ACR_LATENCY) >> FLASH_ACR_LATENCY_Pos) != 2u) { }

    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) { }

    /* SW = 01: HSI16 as SYSCLK. AHB/APB prescalers stay at /1 (reset value). */
    RCC->CFGR1 = (RCC->CFGR1 & ~RCC_CFGR1_SW) | (1u << RCC_CFGR1_SW_Pos);
    while (((RCC->CFGR1 & RCC_CFGR1_SWS) >> RCC_CFGR1_SWS_Pos) != 1u) { }

    /* Cycle counter for delay_us(). */
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    SysTick_Config(CPU_HZ / 1000u);
}

/* ---------------------------------------------------------------- uart */

static void uart_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;

    pin_cfg(GPIOA, 9,  MODE_AF, 7, PULL_NONE, SPEED_HIGH);   /* USART1_TX */
    pin_cfg(GPIOA, 10, MODE_AF, 7, PULL_UP,   SPEED_HIGH);   /* USART1_RX */

    USART1->CR1 = 0;
    USART1->BRR = (CPU_HZ + UART_BAUD / 2u) / UART_BAUD;     /* kernel clock = PCLK2 */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void uart_putc(char c)
{
    while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) { }
    USART1->TDR = (uint8_t)c;
}

void uart_write(const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] == '\n') uart_putc('\r');
        uart_putc(s[i]);
    }
}

int uart_getc(void)
{
    uint32_t isr = USART1->ISR;
    if (isr & USART_ISR_ORE) USART1->ICR = USART_ICR_ORECF;
    if (isr & USART_ISR_RXNE_RXFNE) return (int)(USART1->RDR & 0xFFu);
    return -1;
}

/* ---------------------------------------------------------------- spi */

static void spi_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;

    pin_cfg(GPIOA, 5, MODE_AF, 5, PULL_NONE, SPEED_VHIGH);   /* SCK  */
    pin_cfg(GPIOA, 6, MODE_AF, 5, PULL_NONE, SPEED_VHIGH);   /* MISO */
    pin_cfg(GPIOA, 7, MODE_AF, 5, PULL_NONE, SPEED_VHIGH);   /* MOSI */

    SPI1->CR1  = 0;
    /* MBR = 1 -> fPCLK/4 = 4 MHz ; DSIZE = 7 -> 8-bit frames ; FTHLV = 0 (1 frame) */
    SPI1->CFG1 = (1u << SPI_CFG1_MBR_Pos) | (7u << SPI_CFG1_DSIZE_Pos);
    /* Master, software NSS, mode 0 (CPOL=0, CPHA=0), MSB first, keep pins driven when idle */
    SPI1->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_AFCNTR;
    SPI1->CR1  = SPI_CR1_SSI;
}

void spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    if (n == 0) return;
    SPI1->CR2  = (uint32_t)n << SPI_CR2_TSIZE_Pos;
    SPI1->CR1 |= SPI_CR1_SPE;
    SPI1->CR1 |= SPI_CR1_CSTART;

    for (uint16_t i = 0; i < n; i++) {
        while (!(SPI1->SR & SPI_SR_TXP)) { }
        *(volatile uint8_t *)&SPI1->TXDR = tx ? tx[i] : 0x00u;
        while (!(SPI1->SR & SPI_SR_RXP)) { }
        uint8_t b = *(volatile uint8_t *)&SPI1->RXDR;
        if (rx) rx[i] = b;
    }

    while (!(SPI1->SR & SPI_SR_EOT)) { }
    SPI1->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;
    SPI1->CR1 &= ~SPI_CR1_SPE;
}

/* ---------------------------------------------------------------- radio gpio */

void rf_nss(bool high)   { pin_write(GPIOC, 9,  high); }
void rf_reset(bool high) { pin_write(GPIOB, 10, high); }
bool rf_busy(void)       { return pin_read(GPIOB, 5); }
bool rf_dio1(void)       { return pin_read(GPIOB, 4); }

bool button_pressed(void) { return pin_read(GPIOC, 13); }  /* B1 drives PC13 high */

uint16_t board_node_id(void)
{
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
    uint32_t h = 2166136261u;                                /* FNV-1a over 12 bytes */
    for (int w = 0; w < 3; w++) {
        uint32_t v = uid[w];
        for (int b = 0; b < 4; b++) { h ^= (v >> (8 * b)) & 0xFFu; h *= 16777619u; }
    }
    return (uint16_t)(h ^ (h >> 16));
}

/* ---------------------------------------------------------------- init */

void board_init(void)
{
    clock_init();

    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN | RCC_AHB2ENR1_GPIOBEN | RCC_AHB2ENR1_GPIOCEN;
    (void)RCC->AHB2ENR1;

    uart_init();
    spi_init();

    /* Set output levels before switching the pins to output: NSS idle high, NRESET released. */
    pin_write(GPIOC, 9, true);
    pin_write(GPIOB, 10, true);
    pin_cfg(GPIOC, 9,  MODE_OUT, 0, PULL_NONE, SPEED_HIGH);   /* NSS    */
    pin_cfg(GPIOB, 10, MODE_OUT, 0, PULL_NONE, SPEED_LOW);    /* NRESET */
    pin_cfg(GPIOB, 5,  MODE_IN,  0, PULL_NONE, SPEED_LOW);    /* BUSY   */
    pin_cfg(GPIOB, 4,  MODE_IN,  0, PULL_DOWN, SPEED_LOW);    /* DIO1   */
    pin_cfg(GPIOC, 13, MODE_IN,  0, PULL_DOWN, SPEED_LOW);    /* B1     */
}
