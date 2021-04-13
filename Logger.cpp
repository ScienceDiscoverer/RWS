#include "Logger.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define SIZE     17280 // Enough for 24 hrs of readigns taken every 5 seconds
#define LOG_INTR 60    // Each 5 minutes if readings taken every 5 seconds

Reading ouroboros[SIZE];
int pos; // Current position in Ouroboros
int ints_to_log = LOG_INTR; // Intervals to log in file
FILE* rws_db;
pthread_mutex_t rws_db_lock;
pthread_mutex_t msg_log_lock;

void time2str(time_t t, char* out, bool fname);

void initLogger()
{
	pthread_mutex_init(&msg_log_lock, NULL);
	pthread_mutex_init(&rws_db_lock, NULL);
	// Critical Section Beg
	pthread_mutex_lock(&rws_db_lock);
	
	rws_db = fopen("./log/readings.rws", "a+b");
	int res = fseek(rws_db, -SIZE * sizeof(Reading), SEEK_END);
	res = fread(ouroboros, sizeof(Reading), SIZE, rws_db);
	pos = res % SIZE;
	
	pthread_mutex_unlock(&rws_db_lock);
	// Critical Section End
}

void deinitLogger()
{
	pthread_mutex_destroy(&rws_db_lock);
	pthread_mutex_destroy(&msg_log_lock);
	// Critical Section Beg
	pthread_mutex_lock(&rws_db_lock);
	
	fclose(rws_db);
	
	pthread_mutex_unlock(&rws_db_lock);
	// Critical Section End
}

void logReading(Reading rd)
{
	ouroboros[pos] = rd;
	pos = (pos + 1) % SIZE;
	
	if(pos % LOG_INTR == 0)
	{
		// Critical Section Beg
		if(pthread_mutex_trylock(&rws_db_lock) == 0)
		{
			int oft = ints_to_log - 1;
			while(oft >= 0)
			{
				Reading tmp = getReading((unsigned int)oft--);
				fwrite(&tmp, sizeof(Reading), 1, rws_db);
			}
			
			pthread_mutex_unlock(&rws_db_lock);
			// Critical Section End
			ints_to_log = LOG_INTR;
		}
		else // While file is being downloaded, just increase next logging amount to compensate
		{
			ints_to_log += LOG_INTR;
		}
	}
}

Reading getReading(unsigned int offset)
{
	assert(offset < SIZE);
	if((int)offset <= pos)
	{
		return ouroboros[pos - offset - 1];
	}
	else
	{
		return ouroboros[SIZE - offset + pos - 1];
	}
}

FILE* getDBfileAndLock()
{
	// Critical Section Beg
	pthread_mutex_lock(&rws_db_lock);
	return rws_db;
}

void unlockDBfile()
{
	pthread_mutex_unlock(&rws_db_lock);
	// Critical Section End
}

void deleteDBfile()
{
	// Critical Section Beg
	pthread_mutex_lock(&rws_db_lock);
	
	fclose(rws_db);
	remove("./log/readings.rws");
	rws_db = fopen("./log/readings.rws", "a+b");
	
	pthread_mutex_unlock(&rws_db_lock);
	// Critical Section End
}

void formDBfilename(char* buff)
{	
	time_t beg, end;
	fseek(rws_db, 0, SEEK_SET);
	fread(&beg, 4, 1, rws_db);
	fseek(rws_db, -sizeof(Reading), SEEK_END);
	fread(&end, 4, 1, rws_db);
	char begs[20], ends[20];
	time2str(beg, begs, 1);
	time2str(end, ends, 1);
	sprintf(buff, "%s__%s.rws", begs, ends);
}

void logError(const char* descript, int err_num)
{
#ifndef NO_ERR_LOGGING
	// Critical Section Beg
	pthread_mutex_lock(&msg_log_lock);
	
	FILE* err_log = fopen("./log/err.log", "a");
	char tstr[20];
	time2str(time(NULL), tstr, 0);
	fprintf(err_log, "%s: %s - Error: %s\n", tstr, descript, strerror(err_num));
	fclose(err_log);
	
	pthread_mutex_unlock(&msg_log_lock);
	// Critical Section End
#endif
}

void time2str(time_t t, char* out, bool fname)
{
	struct tm s = *localtime(&t);
	char ts = fname ? '.' : ':';
	char sep = fname ? '_' : ' ';
	sprintf(out, "%d.%02d.%02d%c%02d%c%02d%c%02d", s.tm_year + 1900, s.tm_mon + 1,
	s.tm_mday, sep, s.tm_hour, ts, s.tm_min, ts, s.tm_sec);
}