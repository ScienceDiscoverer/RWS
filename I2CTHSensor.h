#ifndef I2CTHSENSOR_H
#define I2CTHSENSOR_H

const float bad_value_f = 0.0f;

class I2CTHSensor
{
public:
	I2CTHSensor();
	~I2CTHSensor();
	virtual void Measure() = 0;
	virtual void SoftReset() = 0;
	float GetTemp() const { return temp_; }
	float GetHumd() const { return humd_; }
	void DeInit();
	
protected:
	// Data
	int addr_;
	int i2c_fs_;
	float temp_;
	float humd_;
};

#endif /* I2CTHSENSOR_H */