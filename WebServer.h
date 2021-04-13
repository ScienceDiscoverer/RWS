#ifndef WEBSERVER_H
#define WEBSERVER_H

#define WRT_WARN         0
#define WRT_SOUND        1
#define WRT_SWITCH       2
#define WRT_SCALE        3
#define WRT_LCD          4
#define WRT_RDINGS       5
#define WRT_DB_FILE      6
#define WRT_DEL_FILE     7
#define WRT_MAIN_HTML    8
#define WRT_DEFAULTS     9
#define WRT_UPDATE_HEAD  10
#define WRT_ERROR        11

struct ClientSock;

struct WebUpdate
{
	WebUpdate* head;
	WebUpdate* tail;
	ClientSock* cdst; // Client Destination. Who wants this update? NULL for global update
	ClientSock* csrc; // Client who originally produced this update
	int op;             // Operation that needs to be performed by writer
	// Data
	int co2w_snd;       // MSSHORT->LSSHORT CO2 Warning level, Sound
	int ht_warn;        // MSB->LSB Humidity Low, Hum. High, Temp. Low, Temp. High
	int lcd;            // MSB->LSB ON HR:MIN, OFF HR:MIN LCD auto switch on/off times
	int ppm;
	float humd;
	float temp;
};

void* serverMain(void* param);
void putWebQueue(const WebUpdate* update); // Caller must send stack-allocated struct
void deinitWebServer();

#endif /* WEBSERVER_H */