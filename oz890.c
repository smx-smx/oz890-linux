#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <mpsse.h>
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <error.h>
#include <errno.h>

struct mpsse_context *ftdi = NULL;
uint8_t address = 0x60;

int debug_level = 0;
char *eeprom_in = NULL;

uint8_t read_register(uint8_t reg)
{
	uint8_t *data = NULL;
	uint8_t rv;
	uint8_t addr_reading = address | 1;

	Start(ftdi);
	Write(ftdi, (char *)&address, 1);
	Write(ftdi, (char *)&reg, 1);

	if(GetAck(ftdi) == ACK) {
		Start(ftdi);
		Write(ftdi, (char *)&addr_reading, 1);

		if(GetAck(ftdi) == ACK) {
			data = (uint8_t *)Read(ftdi, 1);
			if(data) {
				rv = *data;
				free(data);
			}
			SendNacks(ftdi);
			/* Read in one dummy byte, with a NACK */
			Read(ftdi, 1);
			}
	}

	Stop(ftdi);
	if (debug_level >= 2) {
		printf("Register 0x%x read 0x%x\n", reg, rv);
	}
	return rv;
}

void write_register(uint8_t reg, uint8_t data)
{
	if (debug_level >= 2) {
		printf("Writing 0x%x to register 0x%x\n", data, reg);
	}
	Start(ftdi);
	Write(ftdi, (char *)&address, 1);
	Write(ftdi, (char *)&reg, 1);
	Write(ftdi, (char *)&data, 1);

	Stop(ftdi);
}

bool is_eeprom_busy(void)
{
	uint8_t byte = read_register(0x5f); // EEPROM Control Register
	return (byte & (1 << 7)); // bit 7 = busy flag
}

void eeprom_lock(void)
{
	while (is_eeprom_busy());
	write_register(0x5f, 0x00);
}

void read_eeprom_word(uint8_t address, uint8_t *buf)
{
	if (eeprom_in) {
		// file
		FILE *f = fopen(eeprom_in, "r");
		if (!f) {
			error(1, errno, "Couldn't open %s", eeprom_in);
		}
		if (fseek(f, address, SEEK_SET)) {
			error(1, errno, "Couldn't seek %s to 0x%x", eeprom_in, address);
		}
		if (fread(buf, 1, 2, f) < 2) {
			error(1, errno, "Couldn't read %s at 0x%x", eeprom_in, address);
		}
	} else {
		// device
		while (is_eeprom_busy()); // wait for eeprom not be busy
		write_register(0x5e, address); // set eeprom address to read

		while (is_eeprom_busy());
		write_register(0x5f, 0x55); // b01010101 (or 0x55) set eeprom access & word reading mode

		while (is_eeprom_busy());
		buf[1] = read_register(0x5d); // odd addr
		while (is_eeprom_busy());
		buf[0] = read_register(0x5c); // even addr

		if (debug_level >= 1) {
			printf("EEPROM address 0x%x read 0x%02x%02x\n", address, buf[0], buf[1]);
		}

		eeprom_lock();
	}
}

const unsigned eeprom_size = 128;
uint8_t *read_eeprom(void)
{
	uint8_t *rv = malloc(eeprom_size);
	for (unsigned i = 0; i < eeprom_size; i += 2) {
		read_eeprom_word(i, rv + i);
	}
	return rv;
}

void write_eeprom_word(uint8_t address, uint8_t *buf)
{
	while (is_eeprom_busy());
	write_register(0x5f, 0x52);

	while (is_eeprom_busy()); // wait for eeprom not be busy
	write_register(0x5e, address); // set eeprom address

	while (is_eeprom_busy());
	write_register(0x5d, buf[1]);
	while (is_eeprom_busy());
	write_register(0x5c, buf[0]);

	eeprom_lock();
}

void write_eeprom(char *filename)
{
	// read the file
	FILE *f = fopen(filename, "r");
	if (!f) {
		error(1, errno, "Couldn't open %s", filename);
	}
	if (fseek(f, 0, SEEK_END)) {
		error(1, errno, "Couldn't seek %s", filename);
	}
	if (128 != ftell(f)) {	// a little sanity check
		error(1, 0, "%s is not 128 bytes long", filename);
	}
	rewind(f);
	uint8_t contents[eeprom_size];
	if (fread(contents, 1, eeprom_size, f) < eeprom_size) {
		error(1, errno, "Couldn't read %d bytes from %s", eeprom_size, filename);
	}

	uint8_t password[2];
	// get password
	read_eeprom_word(0x7a, password);
	// grab eeprom
	while (is_eeprom_busy());
	write_register(0x5f, 0x50);
	// enter password
	write_register(0x69, password[0]);
	write_register(0x6a, password[1]);
	// check if it's correct
	uint8_t auth_status = read_register(0x6f);
	// let eeprom go
	eeprom_lock();

	if (debug_level >= 2) {
		if (auth_status & (1 << 7)) {
			printf("PWD_FAIL\n");
		}
		if (auth_status & (1 << 6)) {
			printf("PWD_OK\n");
		}
		if (auth_status & (1 << 5)) {
			printf("PWD_BUSY\n");
		}
	}
	if (!(auth_status & (1 << 6))) {
		error(1, 0, "Authentication failed");
	}
	// auth success
	// erase the eeprom
	while (is_eeprom_busy());
	write_register(0x5f, 0x53);
	// write eeprom back
	for (unsigned i = 0; i < eeprom_size; i += 2) {
		if (debug_level >= 1) {
			printf("Writing 0x%02x%02x to 0x%x EEPROM address\n", contents[i], contents[i + 1], i);
		}
		write_eeprom_word(i, contents + i);
	}
}

double adc2mv(int16_t sample)
{
	return 1.22 * sample;
}

unsigned read_cell_voltage(unsigned cell)
{
	assert(cell < 13);
	uint8_t lo = read_register(0x32 + cell * 2);
	uint8_t hi = read_register(0x33 + cell * 2);
	unsigned rv = ((unsigned)hi << 5) + (lo >> 3);
	return rv;
}

// in 100s of µOhms
uint8_t read_sense_resistor(void)
{
	uint8_t tmp[2];
	read_eeprom_word(0x34, tmp);
	return tmp[0] ? tmp[0] : 25;
}

double read_current(void)
{
	uint8_t lo = read_register(0x54);
	uint8_t hi = read_register(0x55);
	unsigned voltage_raw = ((unsigned)hi << 8) + lo;	// in 7.63µVs
	double voltage_V = voltage_raw * 7.63 / 1000000;
	double sense_Ohm = read_sense_resistor() / 10000.0;
	return voltage_V / sense_Ohm;
}

void print_help(char *name)
{
	fprintf(stderr, "Usage: %s [-c] [-d] [-e file] [-F] [-f] [-h] [-o file] [-v] [-w file]\n\n"
		"Options:\n"
		"	-c		display current\n"
		"	-d		debug output; use multiple times to increase verbosity\n"
		"	-e file		work on the eeprom dump instead of a real device\n"
		"	-F		force operating on an unknown device\n"
		"	-f		display and fix flags\n"
		"	-h		display this help\n"
		"	-o file		read the eeprom to the file\n"
		"	-v		display voltages\n"
		"	-w file		write the file into the eeprom\n",
		name);
}

int main(int argc, char *argv[])
{
	int retval = EXIT_SUCCESS;

	int opt;

	char *eeprom_out = NULL;
	char *eeprom_file = NULL;
	bool read_current_ = false;
	bool read_flags = false;
	bool read_voltages = false;
	bool force = false;

	while ((opt = getopt(argc, argv, "cde:Ffho:vw:")) != -1) {
		switch (opt) {
		case 'c':
			read_current_ = true;
			break;
		case 'd':
			debug_level++;
			break;
		case 'e':
			eeprom_in = strdup(optarg);
			break;
		case 'F':
			force = true;
			break;
		case 'f':
			read_flags = true;
			break;
		case 'h':
			print_help(argv[0]);
			return EXIT_FAILURE;
		case 'o':
			eeprom_out = strdup(optarg);
			break;
		case 'v':
			read_voltages = true;
			break;
		case 'w':
			eeprom_file = strdup(optarg);
			break;
		default:
			print_help(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!eeprom_in) {
		if((ftdi = MPSSE(I2C, FOUR_HUNDRED_KHZ, MSB)) != NULL && ftdi->open)
		{
			if (debug_level >= 1)
				printf("%s initialized at %dHz (I2C)\n", GetDescription(ftdi), GetClock(ftdi));
			uint8_t chip_id = read_register(0);
			if (chip_id != 2) {
				fprintf(stderr, "Unknown chip: %x\n", chip_id);
				if (!force) goto out;
			} else {
				printf("OZ890 rev C detected.\n");
			}
		} else {
			fprintf(stderr, "Failed to initialize MPSSE: %s\n", ErrorString(ftdi));
			return -1;
		}
	}
	if (eeprom_file) {
		write_eeprom(eeprom_file);
	}
	if (read_flags) {
		uint8_t tmp[2];
		read_eeprom_word(0x32, tmp);
		bool software_mode = !(tmp[0] & 1);
		if (software_mode)
			puts("Software mode.");
		else {
			printf("Hardware mode. Bleeding is %s.\n",
					(tmp[0] & 2) ? "enabled" : "disabled");
		}
		uint8_t softsleep = read_register(0x14);
		if (softsleep & 2)
			puts("Woken up by short circuit.");
		if (softsleep & 0x10)
			puts("Device is in low power state.");
		uint8_t shutdown = read_register(0x15);
		if (shutdown & 0x10) {
			printf("Battery is unbalanced (permanent failure flag). Clearing...\n");
			write_register(0x15, 0x10);
		}
		if (shutdown & 0x8)
			printf("MOSFET failure detected.\n");
		if (shutdown & 0x4)
			printf("Voltage High Permanent Failure.\n");
		if (shutdown & 0x2)
			printf("Voltage Low Permanent Failure.\n");
		if (shutdown & 0x1)
			puts("Shut down by a software request.");
		uint8_t check_yes = read_register(0x1c);
		if (check_yes & 1)
			puts("Undervoltage detected.");
		if (check_yes & 2)
			puts("Cell voltage is extremely low (permanent failure flag)!");
		if (check_yes & 4)
			puts("Cell voltage is extremely high (permanent failure flag)!");
		if (check_yes & 8)
			puts("MOSFET failure (permanent failure flag)!");
		if (check_yes & 0x10)
			puts("Cells are unbalanced (permanent failure flag)!");
		if (check_yes & 0x20)
			puts("Overvoltage detected.");
		if (check_yes & 0x40)
			puts("Temperature is too low.");
		if (check_yes & 0x80)
			puts("Temperature is too high!");
		if (software_mode) {
			uint8_t fet_enable = read_register(0x1e);
			if ((fet_enable & 1) == 0)
				printf("Discharge MOSFET is disabled by software.\n");
			if ((fet_enable & 2) == 0)
				printf("Charge MOSFET is disabled by software.\n");
			if ((fet_enable & 4) == 0)
				printf("Precharge MOSFET is disabled by software.\n");
		}
		uint8_t cd_state = read_register(0x20);
		if (cd_state & 8) {
			puts("Battery is charging.");
		} else {
			if (debug_level) puts("Battery is not charging.");
		}
		if (cd_state & 4) {
			puts("Battery is discharging.");
		} else {
			if (debug_level) puts("Battery is not discharging.");
		}
	}
	if (read_voltages) {
		if (!eeprom_in) {
			for (int cell = 0; cell < 13; ++cell) {
				unsigned voltage = read_cell_voltage(cell);
				printf("Cell %d: %lfmV\n", cell, adc2mv(voltage));
			}
		}
		uint8_t tmp[2];
		read_eeprom_word(0x4a, tmp);
		uint16_t ovt = adc2mv((tmp[0] >> 3) | (tmp[1] << 5));
		printf("OV Threshold: %umV\n", ovt);

		read_eeprom_word(0x4c, tmp);
		uint16_t ovr = adc2mv((tmp[0] >> 3) | (tmp[1] << 5));
		printf("OV Release: %umV\n", ovr);

		read_eeprom_word(0x4e, tmp);
		uint16_t uvt = adc2mv((tmp[0] >> 3) | (tmp[1] << 5));
		printf("UV Threshold: %umV\n", uvt);

		read_eeprom_word(0x50, tmp);
		uint16_t uvr = adc2mv((tmp[0] >> 3) | (tmp[1] << 5));
		printf("UV Release: %umV\n", uvr);
	}
	if (read_current_) {
		printf("Current: %lfA\n", read_current());
	}
	if (eeprom_out) {
		FILE *f = fopen(eeprom_out, "wb");
		uint8_t *buf = read_eeprom();
		fwrite(buf, eeprom_size, 1, f);
		fclose(f);
		free(buf);
	}

out:
	Close(ftdi);

	return retval;
}
