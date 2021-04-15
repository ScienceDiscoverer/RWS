#ifndef HTU21D_H
#define HTU21D_H

#include "I2CTHSensor.h"

class HTU21D : public I2CTHSensor
{
public:
	HTU21D();
	~HTU21D() = default;
	int Measure();
	void SoftReset();
private:
	unsigned char CheckSum(unsigned short sens_msg, unsigned char crc);
};

#endif /* HTU21D_H */