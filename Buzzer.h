#ifndef BUZZER_H
#define BUZZER_H

#define SNG_NONE           0
#define SNG_BEEP           1
#define SNG_SOS            2
#define SNG_CO2            3
#define SNG_IMPERIAL_MARCH 4
#define SNG_STAR_TREK      5
#define SNG_DOOM           6

void initBuzz(int buzz_pin);
void buzzPlay(int song); // Non blocking sound playback

#endif /* BUZZER_H */