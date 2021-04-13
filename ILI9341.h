#ifndef ILI9341_H
#define ILI9341_H

#define MODE_16_BIT
//#define CALC_TOUCH_PRESSURE

void initLCD();
void deinitLCD();
void updateReadings(int ppm, float humd, float temp);
void onLCD();
void offLCD();

#endif /* ILI9341_H */