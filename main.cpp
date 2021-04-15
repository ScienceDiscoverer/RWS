#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include "Externs.h"
#include "HTU21D.h"
#include "SHT31D.h"
#include "CO2.h"
#include "ILI9341.h"
#include "WebServer.h"
#include "Buzzer.h"
#include "Logger.h"

#define TH_MSR_DUR  50000000 // Tempreture + Humidity mesuring duration
#define CO2_MSR_DUR 30000000 // CO2 mesuring duration
#define NSEC_PASSED(ts0, ts1, tns0, tns1) ((ts1 - ts0) * 1000000000 + (tns1 - tns0))

// Extern variables
volatile bool allow_poweroff;
volatile bool update_allowed = true;
volatile bool lcd_is_on;

volatile int lcd_on_off_time;
pthread_mutex_t lcd_on_off_time_lock;

volatile int co2_warning = 1000; // ASHRAE 2006 / EN 13779:2008 / WHO 1990
volatile int humd_warning_low = 30;
volatile int humd_warning_high = 50;
volatile int temp_warning_low = 20;
volatile int temp_warning_high = 27;
pthread_mutex_t warning_levels_lock;

volatile int co2_warning_song = SNG_BEEP;
pthread_mutex_t co2_warning_song_lock;

const int update_period_ms = 1000;
pthread_attr_t master_thread_attr; // Will be used to create all threads

HTU21D thsens1;
SHT31D thsens2;
MHZ19B co2sens;

void sigCatcher(int signum);
void initLocks();
void destroyLocks();
void loadConfig();
void saveConfig();

int main()
{	
	// Set up signal handlers
	signal(SIGINT, sigCatcher);
	signal(SIGTERM, sigCatcher);
	signal(SIGQUIT, sigCatcher);
	
	// Prepare master thread attributes and mutex locks
	pthread_attr_init(&master_thread_attr);
	pthread_attr_setdetachstate(&master_thread_attr, PTHREAD_CREATE_DETACHED);
	initLocks();
	
	loadConfig();
	
	pthread_t servthd;
	pthread_create(&servthd, &master_thread_attr, serverMain, NULL);
	
	initLCD();
	
	int msec = 0;
	int ppm = co2sens.GetPPM();
	float humd = 0.0f, temp = 0.0f;
	
	unsigned int upd_period_ns = update_period_ms * 1000000;
	struct timespec beg, end; // No Hobbits live here!
	struct timespec th, co, th_aw, end_wait = {0}, th_wait = {0}, co_wait = {0};
	
	struct tm t;
	time_t sec;
	
	while(1)
	{		
		clock_gettime(CLOCK_MONOTONIC, &beg);
		int th1_res = thsens1.Measure();
		int th2_res = thsens2.Measure();
		float h1, h2, t1, t2;
		h1 = thsens1.GetHumd();
		h2 = thsens2.GetHumd();
		t1 = thsens1.GetTemp();
		t2 = thsens2.GetTemp();
		
		if(th1_res == 0 && th2_res == 0)
		{
			humd = roundf((h1 + h2)/2.0f * 10.0f)/10.0f;
			temp = roundf((t1 + t2)/2.0f * 10.0f)/10.0f;
		}
		else if(th1_res < 0 && th2_res == 0)
		{
			humd = h2;
			temp = t2;
		}
		else if(th1_res == 0 && th2_res < 0)
		{
			humd = h1;
			temp = t1;
		}
		
		clock_gettime(CLOCK_MONOTONIC, &th);
		th_wait.tv_nsec = TH_MSR_DUR - NSEC_PASSED(beg.tv_sec, th.tv_sec, beg.tv_nsec, th.tv_nsec);
		nanosleep(&th_wait, NULL);
		clock_gettime(CLOCK_MONOTONIC, &th_aw);
		
		// CO2 sensor measures every 5 seconds no matter how often it's probed
		if(msec >= 5000)
		{
			ppm = co2sens.GetPPM();
			msec = 0;
		}
		
		clock_gettime(CLOCK_MONOTONIC, &co);
		co_wait.tv_nsec = CO2_MSR_DUR - NSEC_PASSED(th_aw.tv_sec, co.tv_sec, th_aw.tv_nsec, co.tv_nsec);
		nanosleep(&co_wait, NULL);
		
		WebUpdate upd;
		memset(&upd, 0, sizeof(WebUpdate));
		upd.op = WRT_RDINGS;
		upd.ppm = ppm;
		upd.humd = humd;
		upd.temp = temp;
		putWebQueue(&upd);
		
		if(lcd_is_on && update_allowed)
		{
			updateReadings(ppm, humd, temp);
		}
		
		// Critical Section Beg
		pthread_mutex_lock(&lcd_on_off_time_lock);
		
		int onh = lcd_on_off_time >> 24;
		int onm = (lcd_on_off_time & 0xFF0000) >> 16;
		int offh = (lcd_on_off_time & 0xFF00) >> 8;
		int offm = lcd_on_off_time & 0xFF;
		
		pthread_mutex_unlock(&lcd_on_off_time_lock);
		// Critical Section End
		
		if(onh == offh && onm == offm)
		{
			goto loopend;
		}
		
		sec = time(NULL);
		t = *localtime(&sec);
		if(!lcd_is_on && t.tm_hour == onh && t.tm_min == onm)
		{
			onLCD();
		}
		else if(lcd_is_on && t.tm_hour == offh && t.tm_min == offm)
		{
			offLCD();
		}
		
		loopend:
		msec += update_period_ms;
		clock_gettime(CLOCK_MONOTONIC, &end);
		
		// This operation takes from 4000 to 1000 nano seconds, which is fine and shouldn't cause much desync
		end_wait.tv_nsec = upd_period_ns - NSEC_PASSED(beg.tv_sec, end.tv_sec, beg.tv_nsec, end.tv_nsec);
		nanosleep(&end_wait, NULL);
	}
	
	sigCatcher(0);
	
	return 0;
}

void sigCatcher(int signum)
{	
	logError("SIGINT/SIGTERM/SIGQUIT catched, Deiniting", 0);
	saveConfig();
	pthread_attr_destroy(&master_thread_attr);
	destroyLocks();
	deinitWebServer();
	deinitLCD();
	thsens1.DeInit();
	thsens2.DeInit();
	co2sens.DeInit();
	
	if(allow_poweroff)
	{
		logError("Powering off Raspberry Pi", 0);
		deinitLogger();
		system("poweroff");
		exit(0);
	}
	else
	{
		logError("Exiting normally", 0);
		deinitLogger();
		exit(0);
	}
	sleep(10);
}

void initLocks()
{
	pthread_mutex_init(&lcd_on_off_time_lock, NULL);
	pthread_mutex_init(&warning_levels_lock, NULL);
	pthread_mutex_init(&co2_warning_song_lock, NULL);
}

void destroyLocks()
{
	pthread_mutex_destroy(&lcd_on_off_time_lock);
	pthread_mutex_destroy(&warning_levels_lock);
	pthread_mutex_destroy(&co2_warning_song_lock);
}

// Only Main Thread will be present on config load, so no need to lock
void loadConfig()
{
	FILE* f = fopen("./log/config.cfg", "r");
	if(f == NULL)
	{
		return;
	}
	
	char c[140];
	fread(c, 1, 140, f);
	
	fclose(f);
	
	for(int i = 0; i < 140; ++i)
	{
		if(c[i] == '\n')
		{
			c[i] = 0;
		}
	}
	
	lcd_on_off_time = atoi(c+12) << 24 | atoi(c+27) << 16 | atoi(c+43) << 8 | atoi(c+59);
	co2_warning = atoi(c+68);
	humd_warning_low = atoi(c+82);
	humd_warning_high = atoi(c+95);
	temp_warning_low = atoi(c+108);
	temp_warning_high = atoi(c+121);
	co2_warning_song = atoi(c+136);
}

void saveConfig()
{
	FILE* f = fopen("./log/config.cfg", "w");
	
	// Critical Section Beg
	pthread_mutex_lock(&lcd_on_off_time_lock);
	
	int onh = lcd_on_off_time >> 24;
	int onm = (lcd_on_off_time & 0xFF0000) >> 16;
	int offh = (lcd_on_off_time & 0xFF00) >> 8;
	int offm = lcd_on_off_time & 0xFF;
	
	pthread_mutex_unlock(&lcd_on_off_time_lock);
	// Critical Section End
	
	fprintf(f, "lcd_on_hrs= %02d\nlcd_on_min= %02d\nlcd_off_hrs= %02d\nlcd_off_min= %02d\n",
	onh, onm, offh, offm);
	
	// Critical Section Beg
	pthread_mutex_lock(&warning_levels_lock);
	
	fprintf(f, "wco2= %04d\nwhumd_l= %03d\nwhumd_h= %03d\nwtemp_l= %03d\nwtemp_h= %03d\n",
	co2_warning, humd_warning_low, humd_warning_high, temp_warning_low, temp_warning_high);
	
	pthread_mutex_unlock(&warning_levels_lock);
	pthread_mutex_lock(&co2_warning_song_lock);
	
	fprintf(f, "wco2_song= %02d\n", co2_warning_song);
	
	pthread_mutex_unlock(&co2_warning_song_lock);
	// Critical Section End
	
	fclose(f);
}

/*printf("NSEC PASSED: %d\n", (end.tv_sec - beg.tv_sec) * 1000000000 + (end.tv_nsec - beg.tv_nsec));
fprintf(f, "%lu,", (th_aw.tv_sec - beg.tv_sec) * 1000000000 + (th_aw.tv_nsec - beg.tv_nsec));
fprintf(f, "%lu,", (co_aw.tv_sec - th_aw.tv_sec) * 1000000000 + (co_aw.tv_nsec - th_aw.tv_nsec));
fprintf(f, "%lu,", (t3.tv_sec - co_aw.tv_sec) * 1000000000 + (t3.tv_nsec - co_aw.tv_nsec));
fprintf(f, "%lu,", (t4.tv_sec - t3.tv_sec) * 1000000000 + (t4.tv_nsec - t3.tv_nsec));
fprintf(f, "%lu,", (t5.tv_sec - t4.tv_sec) * 1000000000 + (t5.tv_nsec - t4.tv_nsec));
fprintf(f, "%lu,", (end.tv_sec - t5.tv_sec) * 1000000000 + (end.tv_nsec - t5.tv_nsec));
fprintf(f, "%lu\n", (end.tv_sec - beg.tv_sec) * 1000000000 + (end.tv_nsec - beg.tv_nsec));*/
//printf("%-d ppm HTU: %-3.1f *C %-3.1f%% SHT: %-3.1f *C %-3.1f%%\n", ppm, t1, h1, t2, h2);
//printf("h1 %.3f t1 %.3f h2 %.3f t2 %.3f avgh %.3f avgt %.3f\n", h1, t1, h2, t2, humd, temp);
/* Measurements comparisons
1116 ppm HTU: 26.4 *C 37.8% SHT: 26.4 *C 37.5%
1116 ppm HTU: 26.4 *C 37.9% SHT: 26.5 *C 37.5%
1116 ppm HTU: 26.4 *C 37.9% SHT: 26.4 *C 37.6%
1116 ppm HTU: 26.4 *C 37.9% SHT: 26.4 *C 37.6%
1116 ppm HTU: 26.4 *C 38.0% SHT: 26.5 *C 37.6%
1114 ppm HTU: 26.4 *C 38.0% SHT: 26.5 *C 37.7%
1114 ppm HTU: 26.5 *C 38.1% SHT: 26.4 *C 37.8%
1114 ppm HTU: 26.5 *C 38.2% SHT: 26.4 *C 37.9%
1114 ppm HTU: 26.5 *C 38.3% SHT: 26.5 *C 37.7%
1114 ppm HTU: 26.5 *C 38.4% SHT: 26.4 *C 37.7%
1112 ppm HTU: 26.5 *C 38.5% SHT: 26.4 *C 37.7%
1112 ppm HTU: 26.5 *C 38.6% SHT: 26.5 *C 37.8%
1112 ppm HTU: 26.5 *C 38.7% SHT: 26.4 *C 37.9%
1112 ppm HTU: 26.5 *C 38.8% SHT: 26.5 *C 38.0%
1112 ppm HTU: 26.5 *C 38.9% SHT: 26.5 *C 38.0%
1111 ppm HTU: 26.6 *C 38.9% SHT: 26.5 *C 38.0%
1111 ppm HTU: 26.6 *C 38.9% SHT: 26.5 *C 38.0%
1111 ppm HTU: 26.6 *C 38.8% SHT: 26.5 *C 37.9%
1111 ppm HTU: 26.6 *C 38.7% SHT: 26.5 *C 37.9%
1111 ppm HTU: 26.6 *C 38.7% SHT: 26.5 *C 37.9%
1117 ppm HTU: 26.6 *C 38.6% SHT: 26.5 *C 37.9%
1117 ppm HTU: 26.6 *C 38.6% SHT: 26.5 *C 37.9%
1117 ppm HTU: 26.6 *C 38.6% SHT: 26.5 *C 37.8%
1117 ppm HTU: 26.6 *C 38.6% SHT: 26.4 *C 37.8%
1117 ppm HTU: 26.6 *C 38.5% SHT: 26.5 *C 37.8%
1119 ppm HTU: 26.6 *C 38.5% SHT: 26.5 *C 37.8%
1119 ppm HTU: 26.6 *C 38.5% SHT: 26.5 *C 37.8%
1119 ppm HTU: 26.6 *C 38.4% SHT: 26.5 *C 38.1%
1119 ppm HTU: 26.6 *C 38.4% SHT: 26.5 *C 38.2%
1119 ppm HTU: 26.6 *C 38.3% SHT: 26.5 *C 38.1%
1122 ppm HTU: 26.6 *C 38.2% SHT: 26.5 *C 38.0%
1122 ppm HTU: 26.6 *C 38.2% SHT: 26.5 *C 37.9%
1122 ppm HTU: 26.6 *C 38.1% SHT: 26.5 *C 37.8%
1122 ppm HTU: 26.6 *C 38.1% SHT: 26.5 *C 37.8%
1122 ppm HTU: 26.6 *C 38.2% SHT: 26.5 *C 37.8%
1124 ppm HTU: 26.6 *C 38.2% SHT: 26.5 *C 38.4%
1124 ppm HTU: 26.6 *C 38.2% SHT: 26.5 *C 39.2%
1124 ppm HTU: 26.6 *C 38.3% SHT: 26.5 *C 39.1%
1124 ppm HTU: 26.6 *C 38.4% SHT: 26.6 *C 38.8%
1124 ppm HTU: 26.6 *C 38.5% SHT: 26.5 *C 38.6%
1122 ppm HTU: 26.6 *C 38.5% SHT: 26.6 *C 38.5%
1122 ppm HTU: 26.6 *C 38.5% SHT: 26.6 *C 38.2%
1122 ppm HTU: 26.6 *C 38.5% SHT: 26.5 *C 38.0%
1122 ppm HTU: 26.6 *C 38.5% SHT: 26.5 *C 37.9%
1122 ppm HTU: 26.6 *C 38.4% SHT: 26.6 *C 37.9% */