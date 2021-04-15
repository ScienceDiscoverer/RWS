#include "I2CTHSensor.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <errno.h>
#include "Logger.h"

I2CTHSensor::I2CTHSensor() : i2c_fs_(-1), temp_(bad_humd_temp), humd_(bad_humd_temp)
{
	i2c_fs_ = open("/dev/i2c-1", O_RDWR);
	if(i2c_fs_ < 0)
	{
		logError("Error opening i2c bus filestream...", errno);
		return;
	}
}

I2CTHSensor::~I2CTHSensor()
{
	DeInit();
}

void I2CTHSensor::DeInit()
{
	close(i2c_fs_);
}

extern "C" void __cxa_pure_virtual() { while (1); }