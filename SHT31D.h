#ifndef SHT31D_H
#define SHT31D_H

#include "I2CTHSensor.h"

class SHT31D : public I2CTHSensor
{
public:
	SHT31D();
	~SHT31D() = default;
	int Measure();
	void SoftReset();
private:
	unsigned char CheckSum(unsigned char* data);
};

#endif /* SHT31D_H */