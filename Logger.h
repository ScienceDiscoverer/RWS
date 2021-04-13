#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

struct Reading
{
	unsigned int dt; // UNIX Timestamp (datetime)
	unsigned int rd; // Actual readings: 0000000000000 0000000 00000000 0000
};                   //                          CO2 ^  humd ^   temp ^ t/10

void initLogger();
void deinitLogger();
void logReading(Reading rd);
Reading getReading(unsigned int offset); // Returns latest data, offset gets data from the past
FILE* getDBfileAndLock();
void unlockDBfile();
void deleteDBfile();
void formDBfilename(char* buff);
void logError(const char* descript, int err_num);

#endif /* LOGGER_H */