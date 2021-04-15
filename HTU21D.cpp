#include "HTU21D.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <wiringPi.h> // For delay(1)
#include <errno.h>
#include "Logger.h"

#define CMD_TEMP_MEASURE_HOLD    0xE3
#define CMD_HUMD_MEASURE_HOLD    0xE5
#define CMD_TEMP_MEASURE_NOHOLD  0xF3
#define CMD_HUMD_MEASURE_NOHOLD  0xF5
#define CMD_WRITE_USER_REG       0xE6
#define CMD_READ_USER_REG        0xE7
#define CMD_SOFT_RESET           0xFE

HTU21D::HTU21D()
{
	addr_ = 0x40; // Unshifted(to the left by 1 bit for R/W bit) 7-bit I2C address for the sensor
	int fres = ioctl(i2c_fs_, I2C_SLAVE, addr_);
	if(fres < 0)
	{
		logError("HTU21D: Error acquiring bus access and/or talking to the slave", errno);
		close(i2c_fs_);
		return;
	}
}

int HTU21D::Measure()
{
	unsigned char data[3] = {0};
	unsigned char cmd = 0;
	
	temp_ = bad_humd_temp;
	humd_ = bad_humd_temp;
	
	// Request temperature measurement
	cmd = CMD_TEMP_MEASURE_HOLD;
	int fres = write(i2c_fs_, &cmd, 1);
	if(fres != 1)
	{
		logError("HTU21D: Error writing to the I2C bus", errno);
		return -1;
	}
	
	fres = read(i2c_fs_, data, 3);
	if(fres != 3)
	{
		logError("HTU21D: Error reading from the I2C bus", errno);
		return -1;
	}
	
	// Check for corruption
	unsigned short raw_data = data[0] << 8 | data[1];
	if(CheckSum(raw_data, data[2]))
	{
		logError("HTU21D: Error, recieved corrupted temperature data", 0);
		return -1;
	}
	
	raw_data &= 0xFFFC; // Filter out 2 least significant status bits

	temp_ = -46.85 + 175.72f * (float)raw_data/65536.0f; // 2^16 = 65536
	
	// Request humidity measurement
	cmd = CMD_HUMD_MEASURE_HOLD;
	fres = write(i2c_fs_, &cmd, 1);
	if(fres != 1)
	{
		logError("HTU21D: Error writing to the I2C bus", errno);
		return -1;
	}
	
	fres = read(i2c_fs_, data, 3);
	if(fres != 3)
	{
		logError("HTU21D: Error reading from the I2C bus", errno);
		return -1;
	}
	
	raw_data = data[0] << 8 | data[1];
	if(CheckSum(raw_data, data[2]))
	{
		logError("HTU21D: Error, recieved corrupted humidity data", 0);
		return -1;
	}
	
	raw_data &= 0xFFFC;
	
	humd_ = -6 + 125 * (float)raw_data/65536.0f;
	return 0;
}

void HTU21D::SoftReset()
{
	unsigned char cmd = CMD_SOFT_RESET;
	write(i2c_fs_, &cmd, 1);
	delay(16);
}

// If it returns 0, then the transmission was good
// If it returns something other than 0, then the communication was corrupted
// From: http://www.nongnu.org/avr-libc/user-manual/group__util__crc.html
// POLYNOMIAL = 0x0131 = x^8 + x^5 + x^4 + 1 : http://en.wikipedia.org/wiki/Computation_of_cyclic_redundancy_checks
// Datasheet examples:
// 0xDC   ----> 0x79
// 0x683A ----> 0x7C
// 0x4E85 ----> 0x6B
unsigned char HTU21D::CheckSum(unsigned short sens_msg, unsigned char crc)
{
	int x = (int)sens_msg << 8 | crc;
	int p = 0x988000; // 0x0131 polynomial shifted to far left of 3 bytes
	for(int i = 0; i < 16; ++i)
	{
		if(x & 1 << 23 - i) // XOR data with polynomial if 1 bit is found
		{
			x ^= p;
		}
		p >>= 1; // Gradually move polynomial to the least significant 8 bits in the end of data
	}
	
	return (unsigned char)x;
}


/* Execution times in microseconds
temp: 7593
humd: 17540
tott: 25134
26.7 *C 37.1%
temp: 7596
humd: 17558
tott: 25155
26.7 *C 37.1%
temp: -992415
humd: 17549
tott: -974865
26.7 *C 37.1%
temp: 7588
humd: 17558
tott: 25147
26.7 *C 37.1%
temp: 7589
humd: 17563
tott: 25153
26.7 *C 37.1%
temp: 7579
humd: 17573
tott: 25153
26.7 *C 37.1%
temp: 7591
humd: 17563
tott: 25155
26.7 *C 37.1%
temp: 7590
humd: 17571
tott: 25162
26.7 *C 37.1%
*/