#include "ILI9341.h"
#include <fcntl.h>				// Needed for SPI port
#include <sys/ioctl.h>			// Needed for SPI port
#include <linux/spi/spidev.h>	// Needed for SPI port
#include <unistd.h>			    // Needed for SPI port
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <wiringPi.h>
#include <errno.h>
#include "Logger.h"
#include "Externs.h"
#include "Buzzer.h"

#define SCR_HEIGHT    240
#define SCR_WIDTH     320

#define SCR_BPP       16
#define IMG_BPP       24

#define BURST_BS      2048 // Max 4096 (a C SPI limitation it seems)
#define BITS_PER_WORD 8

#define TOUCH_IRQ_PIN 1

#define DC_PIN        25
#define LED_PIN       22
#define RESET_PIN     24
#define BUZZ_PIN      0

// XPT2046 Touch Screen Controller
//    1       x x x       0            0              0 0
// Control   Channel    12-Bit    Differenctial    Power-Down
// B      Select     Mode      Reference        Between
//           Bits                 Mode             Conversions
#define TOUCH_X       0xD0; // CSB: 1 0 1
#define TOUCH_Y       0x90; // CSB: 0 0 1
#define TOUCH_Z1      0xB0; // CSB: 0 1 1
#define TOUCH_Z2      0xC0; // csb: 1 0 0

#define CO2_BLINKS    20
#define HT_BLINKS     6

#define BLACK_DYE     0
#define WHITE_DYE     1
#define RED_DYE       2
#define BLUE_DYE      3
#define NO_DYE        4

#define FONT_BIG      0
#define FONT_MED      1
#define FONT_SML      2

#define B(op) (unsigned char)(op)

// 16 bit pixel to 24 bit pixel convertions
#define RED(rg, gb) B(rg & 0xF8)
#define GREEN(rg, gb) B(B(rg << 5) | B(B(gb >> 3) & 0xF8)) // Clamp least G bit, to equalise it with R&B
#define BLUE(rg, gb) B(gb << 3)

#define EXTRACT_TOUCH_DATA() ((int)rtx[2] >> 3 | (int)rtx[1] << 5)

// Sends command to LCD with additional arguments Bs
#define COMMAND(cmd, ...) do { \
unsigned char dat[] = { __VA_ARGS__ }; \
spiCommand(cmd); \
spiData(dat, sizeof dat); \
} while(0)

// Wait 120 ms after going IN sleep before going OUT of sleep and vice versa
#define ENTER_SLEEP() \
spiCommand(0x10 /*Sleep IN*/); \
delay(5);

#define EXIT_SLEEP() \
spiCommand(0x11 /*Sleep OUT*/); \
delay(5);

struct Pixel
{
	unsigned char rg;
	unsigned char gb;
	unsigned char a;
};

int spi0_fs; // Descriptor for the SPI0 device filestream
int spi1_fs; // Descriptor for the SPI1 device filestream

unsigned char* frame_buffer;

struct spi_ioc_transfer spi0; // Screen SPI
struct spi_ioc_transfer spi1; // Touch SPI
const unsigned int spi0_speed = 75000000;
const unsigned int spi1_speed = 2000000;

// Theoretically scaling factor should be like this:
//const float touch_x_factor = 4096.0f/SCR_WIDTH;
//const float touch_y_factor = 4096.0f/SCR_HEIGHT;

// But in reality it's more like this (calibrated):
const float touch_x_factor = 3450.0f/SCR_HEIGHT;
const float touch_y_factor = 3416.0f/SCR_WIDTH;
const int touch_x_offset = 400;
const int touch_y_offset = 280;

#ifndef MODE_16_BIT
const int lcd_size = SCR_WIDTH * SCR_HEIGHT * IMG_BPP/8; // 230400
#else
const int lcd_size = SCR_WIDTH * SCR_HEIGHT * 2; // 153600
#endif

volatile bool box_should_be_red;
volatile bool readings_are_updating;
volatile bool poweroff_pending;

// Image resources
// Data always starts from 5th element, first 4 reserved for width/height
// Big numbers
unsigned char* big_num_w[10];
unsigned char* big_num_b[10];

// Medium numbers
unsigned char* med_num_w[10];
unsigned char* med_num_b[10];

// Small numbers
unsigned char* sml_num_w[10];
unsigned char* sml_num_b[10];

// Backgrounds
unsigned char* bg_main;
unsigned char* bg_co2;
unsigned char *bg_humd_low, *bg_humd_high;
unsigned char *bg_temp_low, *bg_temp_high;

// Misc
unsigned char* x_button;
unsigned char *dot_b, *dot_w;
unsigned char *minus_b, *minus_w;

int spiOpenPort(int spi_device);
int spiCommand(unsigned char cmd);
int spiData(const unsigned char* dat, int s);
void drawScrBuffer();
void hardwareReset();
void onTouchInput();
unsigned char* loadPGM(const char* path, int dye, bool alpha);
unsigned char* loadPAM(const char* path);
unsigned char* createBox(const unsigned char* orig_img);
void multiplex24(const unsigned char* img, int x, int y);
void multiplex32(const unsigned char* img, int x, int y);
void multiplexInt(int num, const unsigned char *const *font, int f_size, int x, int y);
void multiplexTempFloat(float num, const unsigned char *const *font, const unsigned char *const *smol_font,
const unsigned char* dot, const unsigned char* minus, int x, int y);
void loadNums(const char* suffix, int dye, unsigned char** dest);
void freeNums(unsigned char** targ);
void eraseX();

// External functions
void initLCD()
{
	wiringPiSetupGpio();
	
	pinMode(DC_PIN, OUTPUT);
	pinMode(LED_PIN, OUTPUT);
	pinMode(TOUCH_IRQ_PIN, INPUT);
	pinMode(RESET_PIN, OUTPUT);
	pullUpDnControl(DC_PIN, PUD_OFF);
	pullUpDnControl(LED_PIN, PUD_OFF);
	pullUpDnControl(TOUCH_IRQ_PIN, PUD_OFF);
	pullUpDnControl(RESET_PIN, PUD_OFF);
	wiringPiISR(TOUCH_IRQ_PIN, INT_EDGE_FALLING, &onTouchInput); // Setup Interrupt Service Routine for touch-screen
	
	spiOpenPort(0);
	spi0.delay_usecs = 0;
	spi0.speed_hz = spi1_speed;
	spi0.bits_per_word = BITS_PER_WORD;
	
	spiOpenPort(1);
	spi1.delay_usecs = 0;
	spi1.speed_hz = spi1_speed;
	spi1.bits_per_word = BITS_PER_WORD;
	spi1.cs_change = 0; // If 1, touch chip select stays low after transfer and IRQ gets locked by screen data!
	spi1.len = 3;
	
	// Load resourses
	frame_buffer = (unsigned char*)malloc(lcd_size);
	
	loadNums("b", WHITE_DYE, big_num_w);
	loadNums("b", BLACK_DYE, big_num_b);
	loadNums("m", WHITE_DYE, med_num_w);
	loadNums("m", BLACK_DYE, med_num_b);
	loadNums("s", WHITE_DYE, sml_num_w);
	loadNums("s", BLACK_DYE, sml_num_b);

	bg_main = loadPGM("./img/bg.pgm", NO_DYE, false);
	bg_co2 = loadPGM("./img/co2bg.pgm", RED_DYE, false);
	bg_humd_low = loadPGM("./img/rhbg.pgm", RED_DYE, false);
	bg_humd_high = loadPGM("./img/rhbg.pgm", BLUE_DYE, false);
	bg_temp_low = loadPGM("./img/tbg.pgm", BLUE_DYE, false);
	bg_temp_high = loadPGM("./img/tbg.pgm", RED_DYE, false);
	
	x_button = loadPAM("./img/x.pam");
	dot_b = loadPGM("./img/d.pgm", BLACK_DYE, true);
	dot_w = loadPGM("./img/d.pgm", WHITE_DYE, true);
	minus_b = loadPGM("./img/m.pgm", BLACK_DYE, true);
	minus_w = loadPGM("./img/m.pgm", WHITE_DYE, true);
	
	initBuzz(BUZZ_PIN);
	
	// Enable reset pin to make possible screen operation
	digitalWrite(RESET_PIN, 1);
	delay(120);
	
	// Change screen orientation to landscape mode. This command MUST be called BEFORE Soft. Reset, otherwise screen bugs out
	COMMAND(0x36, /*Memory Access Control*/ 0xE0 /*Row/Column Exchange + Column + Row Address Order*/);
	
	spiCommand(0x01 /*Software reset*/);
	delay(5); // Wait some time for the supply voltages and clock circuits to stabilise
	
#ifdef MODE_16_BIT
	COMMAND(0x3A, /*Pixel Format Set*/ 0x55 /*16 bit mode 65 536 colors*/);
#endif
	COMMAND(0xE0, /*Positive Gamma Correction*/ 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00);
	COMMAND(0xE1, /*Negative Gamma Correction*/ 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F);
	
	EXIT_SLEEP();
	spiCommand(0x29 /*Display ON*/);
	delay(130); // This delay is only cosmetic, to skip few white frames at display turning ON
	
	digitalWrite(LED_PIN, 1); // Turn backlight ON
	lcd_is_on = true;
	
	spi0.speed_hz = spi0_speed;
}

void deinitLCD()
{
	digitalWrite(LED_PIN, 0); // Turn backlight OFF
	spiCommand(0x28 /*Display OFF*/);
	ENTER_SLEEP();
	lcd_is_on = false;
	
	free(frame_buffer);
	
	freeNums(big_num_w);
	freeNums(big_num_b);
	freeNums(med_num_w);
	freeNums(med_num_b);
	freeNums(sml_num_w);
	freeNums(sml_num_b);

	free(bg_main);
	free(bg_co2);
	free(bg_humd_low);
	free(bg_humd_high);
	free(bg_temp_low);
	free(bg_temp_high);
	
	free(x_button);
	free(dot_b);
	free(dot_w);
	free(minus_b);
	free(minus_w);
	
	close(spi0_fs);
	close(spi1_fs);
}

void updateReadings(int ppm, float humd, float temp)
{
	// Critical Section Beg
	pthread_mutex_lock(&warning_levels_lock);
	
	int co2wh = co2_warning;
	int humdwl = humd_warning_low;
	int humdwh = humd_warning_high;
	int tempwl = temp_warning_low;
	int tempwh = temp_warning_high;
	
	pthread_mutex_unlock(&warning_levels_lock);
	// Critical Section End	
	readings_are_updating = true;
	memcpy((void*)frame_buffer, (const void*)(bg_main+4), lcd_size); // 4 first Bs are Width/Height
	
	unsigned char** co2_num = big_num_w;
	unsigned char** humd_num = med_num_w;
	unsigned char** sml_humd_num = sml_num_w;
	unsigned char** temp_num = med_num_w;
	unsigned char** sml_temp_num = sml_num_w;
	unsigned char* temp_minus = minus_w;
	unsigned char* temp_dot = dot_w;
	
	static int co2_blinks = CO2_BLINKS; // 12 seconds
	static int humd_blinks = HT_BLINKS; // 6 seconds
	static int temp_blinks = HT_BLINKS; // 6 seconds
	
	static bool co2_sound_warned;
	
	// Check reading exceeding max safe levels
	if(ppm > co2wh)
	{
		if(co2_blinks <= 0 || co2_blinks%2 == 0)
		{
			multiplex24(bg_co2, 0, 0);
			co2_num = big_num_b;
			box_should_be_red = true;
		}
		if(co2_blinks > 0)
		{
			--co2_blinks;
		}
	}
	else
	{
		co2_blinks = CO2_BLINKS;
		box_should_be_red = false;
		co2_sound_warned = false;
	}
	
	if((int)humd > humdwh)
	{
		if(humd_blinks <= 0 || humd_blinks%2 == 0)
		{
			multiplex24(bg_humd_high, 0, 122);
			humd_num = med_num_b;
			sml_humd_num = sml_num_b;
		}
		if(humd_blinks > 0)
		{
			--humd_blinks;
		}
	}
	else if((int)humd < humdwl)
	{
		if(humd_blinks <= 0 || humd_blinks%2 == 0)
		{
			multiplex24(bg_humd_low, 0, 122);
			humd_num = med_num_b;
			sml_humd_num = sml_num_b;
		}
		if(humd_blinks > 0)
		{
			--humd_blinks;
		}
	}
	else
	{
		humd_blinks = HT_BLINKS;
	}
	
	if(temp > (float)tempwh)
	{
		if(temp_blinks <= 0 || temp_blinks%2 == 0)
		{
			multiplex24(bg_temp_high, 162, 122);
			temp_num = med_num_b;
			sml_temp_num = sml_num_b;
			temp_minus = minus_b;
			temp_dot = dot_b;
		}
		if(temp_blinks > 0)
		{
			--temp_blinks;
		}
	}
	else if(temp < (float)tempwl)
	{
		if(temp_blinks <= 0 || temp_blinks%2 == 0)
		{
			multiplex24(bg_temp_low, 162, 122);
			temp_num = med_num_b;
			sml_temp_num = sml_num_b;
			temp_minus = minus_b;
			temp_dot = dot_b;
		}
		if(temp_blinks > 0)
		{
			--temp_blinks;
		}
	}
	else
	{
		temp_blinks = HT_BLINKS;
	}
	
	if(ppm < 0)
	{
		ppm = 0;
	}
	if(temp <= -100.0f || temp >= 100.0f)
	{
		temp_num = sml_temp_num;
	}
	int humd_x = 65, humd_f_size = FONT_MED;
	if((int)humd >= 100)
	{
		humd = 100.0f;
		humd_num = sml_humd_num;
		humd_f_size = FONT_SML;
		humd_x = 79;
	}
	else if((int)humd <= 0)
	{
		humd = 0.0f;
	}
	
	multiplexInt(ppm, (const unsigned char**)co2_num, FONT_BIG, 195, 99);
	multiplexInt((int)humd, (const unsigned char**)humd_num, humd_f_size, humd_x, 214);
	
	multiplexTempFloat(temp, (const unsigned char**)temp_num,
	(const unsigned char**)sml_temp_num, temp_dot, temp_minus, 285, 214);
	
	if(poweroff_pending)
	{
		multiplex32(x_button, 282, 0);
	}
	
	drawScrBuffer();
	
	if(co2_warning_song != SNG_NONE && !co2_sound_warned && co2_blinks <= 0)
	{
		// Critical Section Beg
		pthread_mutex_lock(&co2_warning_song_lock);
		
		buzzPlay(co2_warning_song);
		
		pthread_mutex_unlock(&co2_warning_song_lock);
		// Critical Section End
		co2_sound_warned = true;
	}
	
	readings_are_updating = false;
}

void onLCD()
{
	lcd_is_on = true;
	EXIT_SLEEP();
	delay(120); // Must wait at least 120 ms until next ENTER_SLEEP command
	digitalWrite(LED_PIN, 1);
}

void offLCD()
{
	lcd_is_on = false;
	digitalWrite(LED_PIN, 0);
	ENTER_SLEEP();
}

// Internal functions
int spiOpenPort(int spi_device)
{
    int *spi_fs;
	static int tries = 0;
	tries = 0;

    // SPI_MODE_0 (0,0) CPOL = 0, CPHA = 0
	// Clock idle low, data is clocked in on rising edge, output data (change) on falling edge
    unsigned char spi_mode = SPI_MODE_0;
    unsigned char spi_bits_per_word = BITS_PER_WORD;
	unsigned int max_spd = spi0_speed;

    if (spi_device)
	{
    	spi_fs = &spi1_fs;
	}
    else
	{
    	spi_fs = &spi0_fs;
	}

	jmp_retry:

    if (spi_device)
	{
    	*spi_fs = open("/dev/spidev0.1", O_RDWR);
	}
    else
	{
    	*spi_fs = open("/dev/spidev0.0", O_RDWR);
	}

    if (*spi_fs < 0)
    {
        char msg[80];
		sprintf(msg, "ILI9341: Error - Could not open SPI%d device [try %d]", spi_device, tries);
		logError(msg, errno);
		delay(1000);
		++tries;
		goto jmp_retry;
        //return -1;
    }
	
	int res = 0;
    res += ioctl(*spi_fs, SPI_IOC_WR_MODE, &spi_mode);
    res += ioctl(*spi_fs, SPI_IOC_RD_MODE, &spi_mode);
    res += ioctl(*spi_fs, SPI_IOC_WR_BITS_PER_WORD, &spi_bits_per_word);
    res += ioctl(*spi_fs, SPI_IOC_RD_BITS_PER_WORD, &spi_bits_per_word);
    res += ioctl(*spi_fs, SPI_IOC_WR_MAX_SPEED_HZ, &max_spd);
    res += ioctl(*spi_fs, SPI_IOC_RD_MAX_SPEED_HZ, &max_spd);
	
	if(res < 0)
	{
		logError("ILI9341: Something went wrong with SPI%d initialisation\n", errno);
        return -1;
	}
	
    return 0;
}

int spiCommand(unsigned char cmd)
{
	digitalWrite(DC_PIN, 0);
	spi0.cs_change = 0;
	spi0.len = 1;
	spi0.tx_buf = (unsigned long)&cmd;
	spi0.rx_buf = 0;		
	return ioctl(spi0_fs, SPI_IOC_MESSAGE(1), &spi0);
}

int spiData(const unsigned char* dat, int s)
{
	digitalWrite(DC_PIN, 1);
	spi0.cs_change = 0;
	spi0.len = s;
	spi0.tx_buf = (unsigned long)dat;
	spi0.rx_buf = 0;		
	return ioctl(spi0_fs, SPI_IOC_MESSAGE(1), &spi0);
}

void drawScrBuffer()
{
	spiCommand(0x2C /*Memory Write*/);
	spi0.cs_change = 1;
	spi0.len = BURST_BS;
	spi0.rx_buf = 0;
	digitalWrite(DC_PIN, 1); // Switch to data mode
	for(int i = 0; i < lcd_size; i+=BURST_BS)
	{
		spi0.tx_buf = (unsigned long)&frame_buffer[i];		
		ioctl(spi0_fs, SPI_IOC_MESSAGE(1), &spi0);
	}
	spiCommand(0x00 /*NOP - No Operation*/);
}

void hardwareReset()
{
	digitalWrite(RESET_PIN, 1);
	delay(120);
	digitalWrite(RESET_PIN, 0);
	delay(120);
	digitalWrite(RESET_PIN, 1);
	delay(120);
}

// Screen was pressed WARNING: this runs in WiringPi ISR, thread sefety must be enforced!
// BUT! Considering that this ISR will be invoked only very rarely, I replaced costly hardware locks with
// simple custom software boolean locks and just blocked LCD updates while screen is pressed.
void onTouchInput()
{
//  Response:
//  0111 1111  1111 1000 => Only 12 bits are data, skip 1 most sig. bit and 3 least sig. bits

	if(digitalRead(TOUCH_IRQ_PIN))
	{
		return;
	}
	while(readings_are_updating)
	{
		delay(10);
	}
	update_allowed = false; // Block any LCD updates while touch screen input is processed
	
	unsigned char rtx[3] = {0};
	int x = 0, y = 0;
#ifdef CALC_TOUCH_PRESSURE
	int x_raw = 0, z1 = 0, z2 = 0;;
	float prs = 0.0f; // Pressure
#endif
	spi1.tx_buf = (unsigned long)rtx;
	spi1.rx_buf = (unsigned long)rtx;
	
	bool x_button_drawn = false;
	
	while(!digitalRead(TOUCH_IRQ_PIN))
	{
		rtx[1] = rtx[2] = 0;
		rtx[0] = TOUCH_X;
		ioctl(spi1_fs, SPI_IOC_MESSAGE(1), &spi1);
#ifdef CALC_TOUCH_PRESSURE
		x_raw = EXTRACT_TOUCH_DATA();
		y = (x_raw - touch_x_offset)/touch_x_factor; // Touch Scr. X = Screen Y
#else
		y = (EXTRACT_TOUCH_DATA() - touch_x_offset)/touch_x_factor;
#endif
		
		rtx[1] = rtx[2] = 0;
		rtx[0] = TOUCH_Y;
		ioctl(spi1_fs, SPI_IOC_MESSAGE(1), &spi1);
		x = (EXTRACT_TOUCH_DATA() - touch_y_offset)/touch_y_factor;  // Touch Scr. Y = Screen X
		
#ifdef CALC_TOUCH_PRESSURE
		rtx[1] = rtx[2] = 0;
		rtx[0] = TOUCH_Z1;
		ioctl(spi1_fs, SPI_IOC_MESSAGE(1), &spi1);
		z1 = EXTRACT_TOUCH_DATA();
		
		rtx[1] = rtx[2] = 0;
		rtx[0] = TOUCH_Z2;
		ioctl(spi1_fs, SPI_IOC_MESSAGE(1), &spi1);
		z2 = EXTRACT_TOUCH_DATA();
		
		prs = 1000.0f * (float)x_raw/4096.0f * ((float)z2/z1 - 1.0f);
#endif		
		
		if(lcd_is_on && x >= 282 && y <= 37 && !x_button_drawn)
		{
			x_button_drawn = true;
			multiplex32(x_button, 282, 0);
			drawScrBuffer();
		}
		else if(lcd_is_on && x < 282 && y > 37 && x_button_drawn)
		{
			x_button_drawn = false;
			eraseX();
		}
		
		delay(100); // 10 Hz touch screen sampling rate
	}
	
	if(lcd_is_on && x >= 282 && y <= 37) // Top right corner, X-icon
	{
		if(poweroff_pending)
		{
			poweroff_pending = false;
			allow_poweroff = true;
			raise(SIGINT);
		}
		else
		{
			poweroff_pending = true;
			multiplex32(x_button, 282, 0);
			drawScrBuffer();
		}
	}
	else
	{
		if(poweroff_pending)
		{
			poweroff_pending = false;
			eraseX();
			update_allowed = true;
			delay(120);
			return;
		}
		
		if(lcd_is_on)
		{
			lcd_is_on = false;
			digitalWrite(LED_PIN, 0);
			eraseX();
			ENTER_SLEEP();
			delay(120); // Must wait at least 120 ms until next EXIT_SLEEP command
		}
		else
		{
			lcd_is_on = true;
			EXIT_SLEEP();
			delay(120); // Must wait at least 120 ms until next ENTER_SLEEP command
			digitalWrite(LED_PIN, 1);
		}
	}
	
	delay(80); // Wait some time to not mess up 2 LCD redraw commands together
	update_allowed = true;
}

unsigned char* loadPGM(const char* path, int dye, bool alpha)
{
	FILE* img = fopen(path, "rb");
	
	char wa[4] = {0}, ha[4] = {0}; // Max width/height -> 9999
	fseek(img, 3, SEEK_SET);
	char c;
	for(int i = 0; i < 5; ++i) // i < 5 not 4, to catch 0x20 for w with 4 digits
	{
		fread((void*)&c, 1, 1, img);
		if(c == 0x20)
		{
			int* tmp = (int*)wa;
			*tmp <<= (4 - i) * 8; // This will effectively shift all arr. elements to its end
			break;
		}
		wa[i] = B(c & 0x0F); // For example: 0x37 -> 0x07 aka char to number
	}
	for(int i = 0; i < 5; ++i)
	{
		fread((void*)&c, 1, 1, img);
		if(c == 0x0A)
		{
			int* tmp = (int*)ha;
			*tmp <<= (4 - i) * 8; // Direction of shift must be reversed cos memory holds/reads ints in little endian...
			break;                // ...while char array holds decimal numbers in big endian mode!
		}
		ha[i] = B(c & 0x0F);
	}
	
	int w = wa[0] * 1000 + wa[1] * 100 + wa[2] * 10 + wa[3];
	int h = ha[0] * 1000 + ha[1] * 100 + ha[2] * 10 + ha[3];
	
	c = 0;
	while(c != 0x0A)
	{
		fread((void*)&c, 1, 1, img);
	}
	
	const int mp = alpha ? 3 : 2; // Multiplier
	// Screen memory accepts pixels in 2 B form -> 5 red 6 green 5 blue bits
	int size = w * h * mp + 4; // First 4 Bs reserved for width/height, so increse size by 4
	
	unsigned char* dst_img = (unsigned char*)malloc(size);
	unsigned short* wp = (unsigned short*)dst_img;
	unsigned short* hp = (unsigned short*)(dst_img+2);
	*wp = (unsigned short)w;
	*hp = (unsigned short)h;

	unsigned char gp; // Grey Pixel
	for(int i = 4; i < size; i+=mp) // Data starts from 5th element
	{
		fread((void*)&gp, 1, 1, img);
		switch(dye)
		{
		case BLACK_DYE:
			if(!alpha)
			{
				unsigned char inverse = B(0xFF - gp);
				dst_img[i] = B(B(inverse & 0xF8) | B(inverse >> 5));
				dst_img[i+1] = B(B(B(inverse & 0xFC) << 3) | B(inverse >> 3));
			}
			else
			{
				dst_img[i] = 0x00;
				dst_img[i+1] = 0x00;
				dst_img[i+2] = gp;
			}
			break;
		case RED_DYE:
			if(!alpha)
			{
				dst_img[i] = 0x00;
				dst_img[i+1] = B(gp >> 3);
			}
			else
			{
				dst_img[i] = 0x00;
				dst_img[i+1] = 0x1F;
				dst_img[i+2] = gp;
			}
			break;
		case BLUE_DYE:
			if(!alpha)
			{
				dst_img[i] = B(gp & 0xF8);
				dst_img[i+1] = 0x00;
			}
			else
			{
				dst_img[i] = 0xF1;
				dst_img[i+1] = 0x00;
				dst_img[i+2] = gp;
			}
			break;
		case WHITE_DYE:
		case NO_DYE:
		default:
			if(!alpha)
			{
				dst_img[i] = B(B(gp & 0xF8) | B(gp >> 5));
				dst_img[i+1] = B(B(B(gp & 0xFC) << 3) | B(gp >> 3));
			}
			else
			{
				dst_img[i] = 0xFF;
				dst_img[i+1] = 0xFF;
				dst_img[i+2] = gp;
			}
			break;
		}
	}
	
	fclose(img);
	return dst_img;
}

unsigned char* loadPAM(const char* path)
{
	FILE* img = fopen(path, "rb");
	
	char wa[4] = {0}, ha[4] = {0};
	fseek(img, 0x09, SEEK_SET);
	char c;
	for(int i = 0; i < 5; ++i)
	{
		fread((void*)&c, 1, 1, img);
		if(c == 0x0A)
		{
			int* tmp = (int*)wa;
			*tmp <<= (4 - i) * 8;
			break;
		}
		wa[i] = B(c & 0x0F);
	}
	fseek(img, 0x13, SEEK_SET);
	for(int i = 0; i < 5; ++i)
	{
		fread((void*)&c, 1, 1, img);
		if(c == 0x0A)
		{
			int* tmp = (int*)ha;
			*tmp <<= (4 - i) * 8;
			break;
		}
		ha[i] = B(c & 0x0F);
	}
	
	int w = wa[0] * 1000 + wa[1] * 100 + wa[2] * 10 + wa[3];
	int h = ha[0] * 1000 + ha[1] * 100 + ha[2] * 10 + ha[3];
	
	c = 0;
	int lf_count = 0;
	while(lf_count < 4)
	{
		fread((void*)&c, 1, 1, img);
		lf_count += c == 0x0A ? 1 : 0;
	}
	
	int size = w * h * 3 + 4;
	
	unsigned char* dst_img = (unsigned char*)malloc(size);
	unsigned short* wp = (unsigned short*)dst_img;
	unsigned short* hp = (unsigned short*)(dst_img+2);
	*wp = (unsigned short)w;
	*hp = (unsigned short)h;
	
	unsigned char pix[4];
	for(int i = 4; i < size; i+=3)
	{
		fread((void*)pix, 1, 4, img);
		dst_img[i] = B(B(pix[2] & 0xF8) | B(pix[1] >> 5));
		dst_img[i+1] = B(B(B(pix[1] & 0xFC) << 3) | B(pix[0] >> 3));
		dst_img[i+2] = pix[3]; // Alpha Channel
	}
	
	fclose(img);	
	return dst_img;
}

unsigned char* createBox(const unsigned char* orig_img)
{
	int w = *(unsigned short*)orig_img;
	int h = *(unsigned short*)(orig_img+2);
	int s = w * h * 2 + 4;
	unsigned char* black_box = (unsigned char*)malloc(s);
	memset((void*)black_box, 0, s);
	unsigned short* wp = (unsigned short*)black_box;
	unsigned short* hp = (unsigned short*)(black_box+2);
	*wp = (unsigned short)w;
	*hp = (unsigned short)h;
	
	if(box_should_be_red)
	{
		for(int i = 5; i < s; i+=2)
		{
			black_box[i] = 0x1F;
		}
	}
	return black_box;
}

// Replaces pixels of screen buffer at position x/y with img pixels
void multiplex24(const unsigned char* img, int x, int y)
{
	if(x < 0 || y < 0)
	{
		return;
	}
	
	const int fbw = SCR_WIDTH;
	const int fbh = SCR_HEIGHT;
	const int w = *(unsigned short*)img;
	const int h = *(unsigned short*)(img+2);
	
	// Data starts from 5th element, first 4 reserved for width/height
	unsigned short* im = (unsigned short*)(img+4);
	unsigned short* fb = (unsigned short*)frame_buffer;
	
	for(int i = y, ii = 0; i < fbh && ii < h; ++i, ++ii)
	{
		for(int j = x, jj = 0; j < fbw && jj < w; ++j, ++jj)
		{
			fb[j + i*fbw] = im[jj + ii*w];
		}
	}
}

void multiplex32(const unsigned char* img, int x, int y)
{
	if(x < 0 || y < 0)
	{
		return;
	}
	
	const int fbw = SCR_WIDTH;
	const int fbh = SCR_HEIGHT;
	const int w = *(unsigned short*)img;
	const int h = *(unsigned short*)(img+2);	
	
	// Data starts from 5th element, first 4 reserved for width/height
	Pixel* im = (Pixel*)(img+4);
	unsigned short* fb = (unsigned short*)frame_buffer;
	
	for(int i = y, ii = 0; i < fbh && ii < h; ++i, ++ii)
	{
		for(int j = x, jj = 0; j < fbw && jj < w; ++j, ++jj)
		{
			unsigned short* fb_p = fb + j + i*fbw; // Frame Buffer Pixel			
			Pixel* im_p = im + jj + ii*w; // Image Pixel
			unsigned char* fb_pb = (unsigned char*)fb_p; // Frame Buffer Pixel Bs
			
			if(im_p->a == 0xFF) // This is solid pixel, just replace it
			{
				*fb_p = *(unsigned short*)im_p;
				continue;
			}
			else if(!im_p->a) // This is empty pixel, skip it
			{
				continue;
			}
			
			// Alpha Blending Routine
			float scale = im_p->a/255.0f;
			
			unsigned char r = B(B(RED(fb_pb[0], fb_pb[1]) * (1.0f - scale)) + B(RED(im_p->rg, im_p->gb) * scale));
			unsigned char g = B(B(GREEN(fb_pb[0], fb_pb[1]) * (1.0f - scale)) + B(GREEN(im_p->rg, im_p->gb) * scale));
			unsigned char b = B(B(BLUE(fb_pb[0], fb_pb[1]) * (1.0f - scale)) + B(BLUE(im_p->rg, im_p->gb) * scale));
			
			fb_pb[0] = B(B(r & 0xF8) | B(g >> 5));
			fb_pb[1] = B(B(B(g & 0xFC) << 3) | B(b >> 3));
		}
	}
}

void multiplexInt(int num, const unsigned char *const *font, int f_size, int x, int y)
{
	int w_max; // Width Max
	switch(f_size)
	{
	case FONT_SML:
		w_max = 24;
		break;
	case FONT_MED:
		w_max = 44;
		break;
	case FONT_BIG:
	default:
		w_max = 55;
		break;
	}
	
	char snum[42];
	sprintf(snum, "%d", num);
	int size = strlen(snum) - 1;
	for(int i = size; i >= 0; --i)
	{
		int w, h;
		h = *(unsigned short*)(font[snum[i]&0x0F]+2);
		if(i < size)
		{
			w = *(unsigned short*)font[snum[i]&0x0F];
			x -= w + 4 + (w_max-w)/2 + (w_max-w)%2;
		}
		multiplex32(font[snum[i]&0x0F], x, y-h);
		if(i < size)
		{
			x -= (w_max-w)/2;
		}
	}
}

void multiplexTempFloat(float num, const unsigned char *const *font, const unsigned char *const *smol_font,
const unsigned char* dot, const unsigned char* minus, int x, int y)
{
	int w_max = num >= 100.0f ? 24 : 44;
	
	char snum[42];
	sprintf(snum, "%-3.1f", num);
	int size = strlen(snum) - 1;
	
	int h = *(unsigned short*)(smol_font[snum[size]&0x0F]+2);
	multiplex32(smol_font[snum[size]&0x0F], x, y-h);
	
	int w = *(unsigned short*)dot;
	h = *(unsigned short*)(dot+2);
	x -= w + 7;
	multiplex32(dot, x, y-h);
	x -= 3;
	size -= 2;
	
	if(num > 0.0f)
	{
		for(int i = size; i >= 0; --i)
		{
			w = *(unsigned short*)font[snum[i]&0x0F];
			h = *(unsigned short*)(font[snum[i]&0x0F]+2);
			x -= w + 4 + (w_max-w)/2 + (w_max-w)%2;
			multiplex32(font[snum[i]&0x0F], x, y-h);
			x -= (w_max-w)/2;
		}
	}
	else
	{
		for(int i = size; i >= 1; --i)
		{
			w = *(unsigned short*)font[snum[i]&0x0F];
			h = *(unsigned short*)(font[snum[i]&0x0F]+2);
			x -= w + 4 + (w_max-w)/2 + (w_max-w)%2;
			multiplex32(font[snum[i]&0x0F], x, y-h);
			x -= (w_max-w)/2;
		}
		
		w = *(unsigned short*)minus;
		h = *(unsigned short*)(minus+2);
		multiplex32(minus, x-w-2, y-32-h);
	}
}

void loadNums(const char* suffix, int dye, unsigned char** dest)
{
	char fn[14] = { '.', '/', 'i', 'm', 'g', '/', '\0', suffix[0], '.', 'p', 'g', 'm', '\0' };
	for(int i = 0; i < 10; ++i)
	{
		fn[6] = (char)((char)i | (char)0x30);
		dest[i] = loadPGM(fn, dye, true);		
	}
}

void freeNums(unsigned char** targ)
{
	for(int i = 0; i < 10; ++i)
	{
		free(targ[i]);
	}
}

void eraseX() // Erase X button by overwriting with black box
{
	unsigned char* black_box = createBox(x_button);
	multiplex24(black_box, 282, 0);
	drawScrBuffer();
	free(black_box);
}

/* Save Screenshot /////////////////////////////
FILE* out = fopen("post_x.ppm", "wb");
fprintf(out, "P6\n320 240\n255\n");
for(int i = 0; i < lcd_size; i+=2)
{
	unsigned char rgb[3];
	rgb[0] = BLUE(frame_buffer[i], frame_buffer[i+1]);
	rgb[1] = GREEN(frame_buffer[i], frame_buffer[i+1]);
	rgb[2] = RED(frame_buffer[i], frame_buffer[i+1]);
	fwrite(rgb, 1, 3, out);
}
fclose(out);
/////////////////////////////////////////////*/

// Usefull commands
//COMMAND(0x55, /*Write Content Adaptive Brightness Control*/ 0x00 /*Off*/);
//COMMAND(0x55, /*Write Content Adaptive Brightness Control*/ 0x01 /*User Interface Image*/);
//COMMAND(0x55, /*Write Content Adaptive Brightness Control*/ 0x10 /*Still Picture*/);
//COMMAND(0x55, /*Write Content Adaptive Brightness Control*/ 0x11 /*Moving Image*/);
//spiCommand(0x29 /*Screen ON*/);
//spiCommand(0x28 /*Screen OFF*/);
//EXIT_SLEEP();
//ENTER_SLEEP();
//digitalWrite(LED_PIN, 1); // Turn backlight ON
//digitalWrite(LED_PIN, 0); // Turn backlight OFF
//spiCommand(0x38 /*Idle Mode OFF (8 colors)*/);
//spiCommand(0x39 /*Idle Mode ON* (8 colors)*/);
//COMMAND(0x53, /*Write CTRL Display*/ 0x24 /*Brightness CTRL ON, Dimming OFF, Backlight CTRL ON*/); !NOTWORKING
//COMMAND(0x53, /*Write CTRL Display*/ 0x2C /*Brightness CTRL ON, Dimming ON, Backlight CTRL ON*/); !NOTWORKING
//spiCommand(0x51 /*Write Display Brightness*/); !NOTWORKING
//spiCommand(0x51 /*Write Display Brightness*/); !NOTWORKING
//COMMAND(0x36, /*Memory Access Control*/ 0x80 /*Row Address Order Vertical Flip*/);
//COMMAND(0x36, /*Memory Access Control*/ 0x40 /*Column Address Order Horisontal Flip*/);
//COMMAND(0x36, /*Memory Access Control*/ 0x20 /*Row/Column Exchange*/);
//COMMAND(0x36, /*Memory Access Control*/ 0xC0 /*Column + Row Address Order 180 deg. turn*/);
//COMMAND(0x36, /*Memory Access Control*/ 0xE0 /*Row/Column Exchange + Column + Row Address Order*/);
//COMMAND(0x36, /*Memory Access Control*/ 0x60 /*Row/Column Exchange + Column Address Order*/);
//COMMAND(0x36, /*Memory Access Control*/ 0xA0 /*Row/Column Exchange + Row Address Order (flip vertical)*/);
//COMMAND(0x36, /*Memory Access Control*/ 0x00 /*DEFAULT*/);