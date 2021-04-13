#ifndef EXTERNS_H
#define EXTERNS_H

extern volatile bool allow_poweroff;
extern volatile bool update_allowed;
extern volatile bool lcd_is_on;

extern volatile int lcd_on_off_time; // 2 MSB - ON time, 2 LSB - OFF time (Both: B1 - h, B2 - m)
extern pthread_mutex_t lcd_on_off_time_lock;

extern volatile int co2_warning;
extern volatile int humd_warning_low;
extern volatile int humd_warning_high;
extern volatile int temp_warning_low;
extern volatile int temp_warning_high;
extern pthread_mutex_t warning_levels_lock;

extern volatile int co2_warning_song;
extern pthread_mutex_t co2_warning_song_lock;

// Constants
extern const int update_period_ms;
extern pthread_attr_t master_thread_attr;

#ifdef NDEBUG

#define DBPRINT(...) 

#else

#define DBPRINT(...) printf(__VA_ARGS__)

#endif

#endif /* EXTERNS_H */