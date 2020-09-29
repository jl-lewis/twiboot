CC	:= avr-gcc
LD	:= avr-ld
OBJCOPY	:= avr-objcopy
OBJDUMP	:= avr-objdump
SIZE	:= avr-size

TARGET = twiboot
SOURCE = $(wildcard *.c)

# select MCU
MCU = atmega328p

AVRDUDE_PROG := -c usbasp -b 115200 -P usb
#AVRDUDE_PROG := -c avr910 -b 115200 -P /dev/ttyUSB0
#AVRDUDE_PROG := -c dragon_isp -P usb

# ---------------------------------------------------------------------------

ifeq ($(MCU), atmega8)
# atmega8:
# Fuse L: 0x84 (8Mhz internal RC-Osz., 2.7V BOD)
# Fuse H: 0xda (512 words bootloader)
AVRDUDE_MCU=m8
AVRDUDE_FUSES=lfuse:w:0x84:m hfuse:w:0xda:m

BOOTLOADER_START=0x1C00
endif

ifeq ($(MCU), atmega88)
# atmega88:
# Fuse L: 0xc2 (8Mhz internal RC-Osz.)
# Fuse H: 0xdd (2.7V BOD)
# Fuse E: 0xfa (512 words bootloader)
AVRDUDE_MCU=m88
AVRDUDE_FUSES=lfuse:w:0xc2:m hfuse:w:0xdd:m efuse:w:0xfa:m

BOOTLOADER_START=0x1C00
endif

ifeq ($(MCU), atmega168)
# atmega168:
# Fuse L: 0xc2 (8Mhz internal RC-Osz.)
# Fuse H: 0xdd (2.7V BOD)
# Fuse E: 0xfa (512 words bootloader)
AVRDUDE_MCU=m168 -F
AVRDUDE_FUSES=lfuse:w:0xc2:m hfuse:w:0xdd:m efuse:w:0xfa:m

BOOTLOADER_START=0x3C00
endif

ifeq ($(MCU), atmega328p)
# atmega328p:
# Fuse L: 0xc2 (8Mhz internal RC-Osz.)
# Fuse H: 0xdc (512 words bootloader)
# Fuse E: 0xfd (2.7V BOD)
AVRDUDE_MCU=m328p -F
#AVRDUDE_MCU=m328p
AVRDUDE_FUSES=lock:w:0xFF:m lfuse:w:0xc2:m hfuse:w:0xdc:m efuse:w:0xfd:m 
#AVRDUDE_FUSES=lfuse:w:0xd2:m hfuse:w:0xdc:m efuse:w:0xfd:m
AVRDUDE_LOCK=lock:w:0xCF:m

BOOTLOADER_START=0x7C00
endif

# ---------------------------------------------------------------------------

CFLAGS = -pipe -g -Os -mmcu=$(MCU) -Wall -fdata-sections -ffunction-sections
CFLAGS += -Wa,-adhlns=$(*F).lst -DBOOTLOADER_START=$(BOOTLOADER_START)
LDFLAGS = -Wl,-Map,$(@:.elf=.map),--cref,--relax,--gc-sections,--section-start=.text=$(BOOTLOADER_START)
LDFLAGS += -nostartfiles

# ---------------------------------------------------------------------------

$(TARGET): $(TARGET).elf
	@$(SIZE) -B -x --mcu=$(MCU) $<

$(TARGET).elf: $(SOURCE:.c=.o)
	@echo " Linking file:  $@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@$(OBJDUMP) -h -S $@ > $(@:.elf=.lss)
	@$(OBJCOPY) -j .text -j .data -O ihex $@ $(@:.elf=.hex)
	@$(OBJCOPY) -j .text -j .data -O binary $@ $(@:.elf=.bin)

%.o: %.c $(MAKEFILE_LIST)
	@echo " Building file: $<"
	@$(CC) $(CFLAGS) -o $@ -c $<

clean:
	rm -rf $(SOURCE:.c=.o) $(SOURCE:.c=.lst) $(addprefix $(TARGET), .elf .map .lss .hex .bin)

fuses:
	avrdude -v $(AVRDUDE_PROG) -p $(AVRDUDE_MCU) -e -u $(patsubst %,-U %, $(AVRDUDE_FUSES))	

install: $(TARGET).elf
	avrdude -v $(AVRDUDE_PROG) -p $(AVRDUDE_MCU) -D -U flash:w:$(<:.elf=.hex) -D -u -Ulock:w:0xCF:m


