# STRATUM Phase 1 — ping-pong firmware for NUCLEO-U545RE-Q + Core1262-868M
#
#   make deps          fetch CMSIS headers (once)
#   make               build with arm-none-eabi-gcc  (default)
#   make TOOLCHAIN=clang   build with clang + lld instead
#   make flash         flash with OpenOCD (ST-LINK)
#   make flash-pr      flash with probe-rs
#   make flash-cube    flash with STM32CubeProgrammer CLI
#   make term          open the serial console (picocom, 115200)

TOOLCHAIN ?= gcc
PORT      ?= /dev/ttyACM0
PROBE     ?=                       # e.g. PROBE="--probe 0483:374e:SERIAL" with several boards

BUILD   := build
TARGET  := $(BUILD)/stratum-phase1
SRCS    := src/startup.c src/board.c src/sx126x.c src/fmt.c src/main.c
OBJS    := $(SRCS:src/%.c=$(BUILD)/%.o)

CMSIS_DEVICE := third_party/cmsis-device-u5/Include
CMSIS_CORE   := third_party/cmsis_core/CMSIS/Core/Include

COMMON  := -std=c11 -O2 -g3 -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
           -Wall -Wextra -Wshadow -Wundef -Wstrict-prototypes \
           -DSTM32U545xx -I$(CMSIS_DEVICE) -I$(CMSIS_CORE)

ifeq ($(TOOLCHAIN),clang)
  CC      := clang
  OBJCOPY := llvm-objcopy
  SIZE    := llvm-size
  ARCH    := --target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
  CFLAGS  := $(ARCH) $(COMMON)
  LDFLAGS := $(ARCH) -nostdlib -fuse-ld=lld -Wl,--gc-sections -Wl,-Map=$(TARGET).map -T ld/stm32u545re.ld
  LDLIBS  :=
else
  CC      := arm-none-eabi-gcc
  OBJCOPY := arm-none-eabi-objcopy
  SIZE    := arm-none-eabi-size
  ARCH    := -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
  CFLAGS  := $(ARCH) $(COMMON) -fno-tree-loop-distribute-patterns
  LDFLAGS := $(ARCH) -nostdlib -Wl,--gc-sections -Wl,-Map=$(TARGET).map -T ld/stm32u545re.ld
  LDLIBS  := -lgcc
endif

.PHONY: all deps clean flash flash-pr flash-cube term

all: $(TARGET).elf $(TARGET).bin
	@$(SIZE) $(TARGET).elf

deps:
	./scripts/fetch-cmsis.sh

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	@test -d $(CMSIS_DEVICE) || { echo "CMSIS headers missing: run 'make deps' first"; exit 1; }
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) ld/stm32u545re.ld
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD):
	mkdir -p $(BUILD)

flash: $(TARGET).elf
	openocd -f interface/stlink.cfg -f target/stm32u5x.cfg -c "program $< verify reset exit"

flash-pr: $(TARGET).elf
	probe-rs download --chip STM32U545RETxQ $(PROBE) $<
	probe-rs reset --chip STM32U545RETxQ $(PROBE)

flash-cube: $(TARGET).elf
	STM32_Programmer_CLI -c port=SWD -w $< -v -rst

term:
	picocom -b 115200 $(PORT)

clean:
	rm -rf $(BUILD)
