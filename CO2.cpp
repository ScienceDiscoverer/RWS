#include "CO2.h"
#include <stdio.h>
#include <unistd.h>	  // For UART
#include <fcntl.h>    // For UART
#include <termios.h>  // For UART
#include <errno.h>
#include "Logger.h"

MHZ19B::MHZ19B() : uart_fs_(-1)
{
	// Setting up UART Filestream
	uart_fs_ = open("/dev/serial0", O_RDWR | O_NOCTTY); // blocking read/write mode
	if(uart_fs_ == -1)
	{
		logError("Error opening UART. Some other process might be using it", errno);
		return;
	}
	
	struct termios options;
	tcgetattr(uart_fs_, &options);
	options.c_cflag = B9600 | CS8 | CLOCAL | CREAD;
	options.c_iflag = IGNPAR;
	options.c_oflag = 0;
	options.c_lflag = 0;
	
	tcflush(uart_fs_, TCIFLUSH);
	tcsetattr(uart_fs_, TCSANOW, &options);
}

MHZ19B::~MHZ19B()
{
	DeInit();
}

int MHZ19B::GetPPM()
{
	int count = write(uart_fs_, (void*)tx_buff_, 9);	
	if(count < 0)
	{
		logError("MHZ19B: Error sending TX buffer", errno);
		return 0;
	}
	
	count = read(uart_fs_, (void*)rx_buff_, 8);
	
	if(count != 8)
	{
		logError("MHZ19B: Error receiving 8 data bytes", errno);
		return 0;
	}
	
	char check_sum;
	count = read(uart_fs_, (void*)&check_sum, 1);
	if(count != 1)
	{
		logError("MHZ19B: Error receiving checksum byte", errno);
		return 0;
	}
	
	rx_buff_[8] = check_sum;
	
	if(!(rx_buff_[0] == 0xFF && rx_buff_[1] == 0x86 && rx_buff_[8] == CheckSum(rx_buff_)))
	{
		logError("MHZ19B: Corrupted/wrong response! Checksum failed", 0);
		//PrintBuff(rx_buff_);
		return 0;
	}
	
	return rx_buff_[2] << 8 | rx_buff_[3];
}

void MHZ19B::DeInit()
{
	close(uart_fs_);
}

unsigned char MHZ19B::CheckSum(unsigned char* buff) // Checking check-sum
{
	char csum;
	for(int i = 1; i < 8; ++i)
	{
		csum += buff[i];
	}
	
	csum = 0xFF - csum;
	csum += 1;
	return csum;
}

void MHZ19B::PrintBuff(unsigned char* buff)
{
	for(int i = 0; i < 9; ++i)
	{
		printf("%02X ", buff[i]);
	}
	printf("\n");
}