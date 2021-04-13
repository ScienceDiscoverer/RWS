#ifndef CO2_H
#define CO2_H

class MHZ19B
{
public:
	MHZ19B();
	~MHZ19B();
	int GetPPM();
	void DeInit();
	// Calibration functions can be added here
	
private:
	unsigned char CheckSum(unsigned char* buff);
	void PrintBuff(unsigned char* buff);

	// Data
	const unsigned char tx_buff_[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
	unsigned char rx_buff_[9] = {};
	int uart_fs_;
};

#endif /* CO2_H */