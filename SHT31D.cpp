#include "SHT31D.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <wiringPi.h> // For delay(1)
#include <errno.h>
#include "Logger.h"

const unsigned char cmd_measure[2]    = { 0x2C, 0x06 }; // Single shot mode, blocking
const unsigned char cmd_soft_reset[2] = { 0x30, 0xA2 };

SHT31D::SHT31D()
{
	addr_ = 0x44; // Unshifted(to the left by 1 bit for R/W bit) 7-bit I2C address for the sensor
	int fres = ioctl(i2c_fs_, I2C_SLAVE, addr_);
	if(fres < 0)
	{
		logError("SHT31D: Error acquiring bus access and/or talking to the slave", errno);
		close(i2c_fs_);
		return;
	}
}

int SHT31D::Measure()
{
	unsigned char data[6] = {0};
	
	temp_ = bad_humd_temp;
	humd_ = bad_humd_temp;
	
	// Request both temp and humidity measurements
	int fres = write(i2c_fs_, &cmd_measure, 2);
	if(fres != 2)
	{
		logError("SHT31D: Error writing to the I2C bus", errno);
		return -1;
	}
	
	fres = read(i2c_fs_, data, 6);
	if(fres != 6)
	{
		logError("SHT31D: Error reading from the I2C bus", errno);
		return -1;
	}
	
	// Check for corruption	
	if(CheckSum(data) != data[2] || CheckSum(data+3) != data[5])
	{
		logError("SHT31D: Error, recieved corrupted temperature or humidity data", 0);
		return -1;
	}
	
	unsigned short raw_t = data[0] << 8 | data[1];
	unsigned short raw_h = data[3] << 8 | data[4];

	temp_ = -45.0f + 175.0f * (float)raw_t/65535.0f; // 2^16 - 1 = 65535
	humd_ = 100.0f * (float)raw_h/65535.0f;
	return 0;
}

void SHT31D::SoftReset()
{
	write(i2c_fs_, &cmd_soft_reset, 2);
	delay(16);
}

// This is very unconvetional SRC-8 algorithm
// IDK how this works, but it works... Only Chinese programmers know it secrets!
unsigned char SHT31D::CheckSum(unsigned char* data)
{
	/*
        Polynomial: 0x31 (x8 + x5 + x4 + 1) -> NOT CORRECT!
        Initialization: 0xFF
        Final XOR: 0x00
        Example: CRC (0xBEEF) = 0x92
    */
	const unsigned char polynomial = 0x31; // -> This is not x8 + x5 + x4 + 1, its x5 + x4 + 1!
    unsigned char crc = 0xFF;
    
    crc ^= data[0];
    for ( int i = 8; i; --i )
    {
        crc = ( crc & 0x80 ) ? (crc << 1) ^ polynomial : (crc << 1);
    }
	crc ^= data[1];
    for ( int i = 8; i; --i )
    {
        crc = ( crc & 0x80 ) ? (crc << 1) ^ polynomial : (crc << 1);
    }
	
	return crc;
}









