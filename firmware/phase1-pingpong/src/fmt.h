/* Tiny printf to the console: %d %i %u %x %X %c %s %%, width, '0' and '-' flags, 'l' ignored. */
#ifndef FMT_H
#define FMT_H

#include <stdint.h>

/* All integer arguments are 32-bit on Cortex-M; no format attribute because uint32_t is
 * 'unsigned long' with arm-none-eabi-gcc and would trigger -Wformat noise on every %u. */
void out(const char *fmt, ...);

/* Print a quarter-dB value (e.g. SX1262 SNR) as "-7.25". */
void out_q4(int32_t q4);

#endif
