/***************************************************************************
 *   Copyright (C) 07/2010 by Olaf Rempel                                  *
 *   razzor@kopf-tisch.de                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; version 2 of the License,               *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>

#define F_CPU 8000000
#include <util/delay.h>

/*
 * atmega88:
 * Fuse E: 0xfa (512 words bootloader)
 * Fuse H: 0xdd (2.7V BOD)
 * Fuse L: 0xc2 (8Mhz internal RC-Osz.)
 */

/* 25ms @8MHz */
#define TIMER_RELOAD		(0xFF - 195)

/* 40 * 25ms */
#define TIMEOUT			40

#define LED_RT			(1<<PORTB4)
#define LED_GN			(1<<PORTB5)

#define TWI_ADDRESS		0x21

#define APP_END			0x1C00

#define VERSION_STRING		"TWIBOOT m88-v20"
#define SIGNATURE_BYTES		{ 0x1E, 0x93, 0x0A, 0x00 }

/* SLA+R */
#define CMD_WAIT		0x00
#define CMD_READ_VERSION	0x01
#define CMD_READ_MEMORY		0x02
/* internal mappings */
#define CMD_READ_SIGNATURE	(0x10 | CMD_READ_MEMORY)
#define CMD_READ_FLASH		(0x20 | CMD_READ_MEMORY)
#define CMD_READ_EEPROM		(0x30 | CMD_READ_MEMORY)
#define CMD_READ_PARAMETERS	(0x40 | CMD_READ_MEMORY)	/* only in APP */

/* SLA+W */
#define CMD_SWITCH_APPLICATION	CMD_READ_VERSION
#define CMD_WRITE_MEMORY	CMD_READ_MEMORY
/* internal mappings */
#define CMD_BOOT_BOOTLOADER	(0x10 | CMD_SWITCH_APPLICATION)	/* only in APP */
#define CMD_BOOT_APPLICATION	(0x20 | CMD_SWITCH_APPLICATION)
#define CMD_WRITE_SIGNATURE	(0x10 | CMD_WRITE_MEMORY)
#define CMD_WRITE_FLASH		(0x20 | CMD_WRITE_MEMORY)
#define CMD_WRITE_EEPROM	(0x30 | CMD_WRITE_MEMORY)
#define CMD_WRITE_PARAMETERS	(0x40 | CMD_WRITE_MEMORY)	/* only in APP */

/* CMD_SWITCH_APPLICATION parameter */
#define BOOTTYPE_BOOTLOADER	0x00				/* only in APP */
#define BOOTTYPE_APPLICATION	0x80

/* CMD_{READ|WRITE}_* parameter */
#define MEMTYPE_SIGNATURE	0x00
#define MEMTYPE_FLASH		0x01
#define MEMTYPE_EEPROM		0x02
#define MEMTYPE_PARAMETERS	0x03				/* only in APP */

/*
 * LED_GN blinks with 20Hz (while bootloader is running)
 * LED_RT blinks on TWI activity
 *
 * bootloader twi-protocol:
 * - abort boot timeout:
 *   SLA+W, 0x00, STO
 *
 * - show bootloader version
 *   SLA+W, 0x01, SLA+R, {16 bytes}, STO
 *
 * - start application
 *   SLA+W, 0x01, 0x80, STO
 *
 * - read signature bytes
 *   SLA+W, 0x02, 0x00, SLA+R, {4 bytes}, STO
 *
 * - read one (or more) flash bytes
 *   SLA+W, 0x02, 0x01, addrh, addrl, SLA+R, {* bytes}, STO
 *
 * - read one (or more) eeprom bytes
 *   SLA+W, 0x02, 0x02, addrh, addrl, SLA+R, {* bytes}, STO
 *
 * - write one flash page (64bytes on mega8)
 *   SLA+W, 0x02, 0x01, addrh, addrl, {64 bytes}, STO
 *
 * - write one (or more) eeprom bytes
 *   SLA+W, 0x02, 0x02, addrh, addrl, {* bytes}, STO
 */

const static uint8_t info[16] = VERSION_STRING;
const static uint8_t signature[4] = SIGNATURE_BYTES;

/* wait 40 * 25ms = 1s */
static uint8_t boot_timeout = TIMEOUT;
volatile static uint8_t cmd = CMD_WAIT;

/* flash buffer */
static uint8_t buf[SPM_PAGESIZE];
static uint16_t addr;

static void write_flash_page(void)
{
	uint16_t pagestart = addr;
	uint8_t size = SPM_PAGESIZE;
	uint8_t *p = buf;

	if (pagestart >= APP_END)
		return;

	boot_page_erase(pagestart);
	boot_spm_busy_wait();

	do {
		uint16_t data = *p++;
		data |= *p++ << 8;
		boot_page_fill(addr, data);

		addr += 2;
		size -= 2;
	} while (size);

	boot_page_write(pagestart);
	boot_spm_busy_wait();
	boot_rww_enable();
}

static uint8_t read_eeprom_byte(void)
{
	EEARL = addr;
	EEARH = (addr >> 8);
	EECR |= (1<<EERE);
	addr++;
	return EEDR;
}

static void write_eeprom_byte(uint8_t val)
{
	EEARL = addr;
	EEARH = (addr >> 8);
	EEDR = val;
	addr++;

	EECR |= (1<<EEMPE);
	EECR |= (1<<EEPE);
	eeprom_busy_wait();
}

ISR(TWI_vect)
{
	static uint8_t bcnt;
	uint8_t data;
	uint8_t ack = (1<<TWEA);

	switch (TWSR & 0xF8) {
	/* SLA+W received, ACK returned -> receive data and ACK */
	case 0x60:
		bcnt = 0;
 		PORTB |= LED_RT;
		TWCR |= (1<<TWINT) | (1<<TWEA);
		break;

	/* prev. SLA+W, data received, ACK returned -> receive data and ACK */
	case 0x80:
		data = TWDR;
		switch (bcnt) {
		case 0:
			switch (data) {
			case CMD_SWITCH_APPLICATION:
			case CMD_WRITE_MEMORY:
				bcnt++;
				/* no break */

			case CMD_WAIT:
				/* abort countdown */
				boot_timeout = 0;
				break;

			default:
				/* boot app now */
				data = CMD_BOOT_APPLICATION;
				ack = (0<<TWEA);
				break;
			}
			cmd = data;
			break;

		case 1:
			switch (cmd) {
			case CMD_SWITCH_APPLICATION:
				if (data == BOOTTYPE_APPLICATION) {
					cmd = CMD_BOOT_APPLICATION;
				}
				ack = (0<<TWEA);
				break;

			case CMD_WRITE_MEMORY:
				bcnt++;
				if (data == MEMTYPE_SIGNATURE) {
					cmd = CMD_WRITE_SIGNATURE;

				} else if (data == MEMTYPE_FLASH) {
					cmd = CMD_WRITE_FLASH;

				} else if (data == MEMTYPE_EEPROM) {
					cmd = CMD_WRITE_EEPROM;

				} else {
					ack = (0<<TWEA);
				}
				break;
			}
			break;

		case 2:
		case 3:
			addr <<= 8;
			addr |= data;
			bcnt++;
			break;

		default:
			switch (cmd) {
			case CMD_WRITE_FLASH:
				buf[bcnt -4] = data;
				if (bcnt < sizeof(buf) +3) {
					bcnt++;
				} else {
					write_flash_page();
					ack = (0<<TWEA);
				}
				break;

			case CMD_WRITE_EEPROM:
				write_eeprom_byte(data);
				bcnt++;
				break;
			}
			break;
		}

		if (ack == 0x00)
			bcnt = 0;

		TWCR |= (1<<TWINT) | ack;
		break;

	/* SLA+R received, ACK returned -> send data */
	case 0xA8:
		bcnt = 0;
		PORTB |= LED_RT;

	/* prev. SLA+R, data sent, ACK returned -> send data */
	case 0xB8:
		switch (cmd) {
		case CMD_READ_VERSION:
			data = info[bcnt++];
			bcnt %= sizeof(info);
			break;

		case CMD_READ_SIGNATURE:
			data = signature[bcnt++];
			bcnt %= sizeof(signature);
			break;

		case CMD_READ_FLASH:
			data = pgm_read_byte_near(addr++);
			break;

		case CMD_READ_EEPROM:
			data = read_eeprom_byte();
			break;

		default:
			data = 0xFF;
			break;
		}

		TWDR = data;
		TWCR |= (1<<TWINT) | (1<<TWEA);
		break;

	/* STOP or repeated START */
	case 0xA0:
	/* data sent, NACK returned */
	case 0xC0:
		PORTB &= ~LED_RT;
		TWCR |= (1<<TWINT) | (1<<TWEA);
		break;

	/* illegal state -> reset hardware */
	case 0xF8:
		TWCR |= (1<<TWINT) | (1<<TWSTO) | (1<<TWEA);
		break;
	}
}

ISR(TIMER0_OVF_vect)
{
	/* restart timer */
	TCNT0 = TIMER_RELOAD;

	/* blink LED while running */
	PORTB ^= LED_GN;

	/* count down for app-boot */
	if (boot_timeout > 1)
		boot_timeout--;

	/* trigger app-boot */
	else if (boot_timeout == 1)
		cmd = CMD_BOOT_APPLICATION;
}

static void (*jump_to_app)(void) = 0x0000;

/*
 * For newer devices (mega88) the watchdog timer remains active even after a
 * system reset. So disable it as soon as possible.
 * automagically called on startup
 */
void disable_wdt_timer(void) __attribute__((naked, section(".init3")));
void disable_wdt_timer(void)
{
	MCUSR = 0;
	WDTCSR = (1<<WDCE) | (1<<WDE);
	WDTCSR = (0<<WDE);
}

int main(void)
{
	DDRB = LED_GN | LED_RT;
	PORTB = LED_GN;

	/* move interrupt-vectors to bootloader */
	MCUCR = (1<<IVCE);
	MCUCR = (1<<IVSEL);

	/* timer0: running with F_CPU/1024 */
	TCCR0B = (1<<CS02) | (1<<CS00);

	/* enable timer0 OVF interrupt */
	TIMSK0 = (1<<TOIE0);

	/* TWI init: set address, auto ACKs with interrupts */
	TWAR = (TWI_ADDRESS<<1);
	TWCR = (1<<TWEA) | (1<<TWEN) | (1<<TWIE);

	sei();
	while (cmd != CMD_BOOT_APPLICATION);
	cli();

	/* Disable TWI but keep address! */
	TWCR = 0x00;

	/* disable timer0 */
	TIMSK0 = 0x00;
	TCCR0B = 0x00;

	/* move interrupt vectors back to application */
	MCUCR = (1<<IVCE);
	MCUCR = (0<<IVSEL);

	PORTB = 0x00;

	uint8_t i;
	for (i = 0; i < 10; i++)
		_delay_ms(10);

	jump_to_app();
	return 0;
}
