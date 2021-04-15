#include "WebServer.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/statvfs.h>
#include <math.h>
#include "Externs.h"
#include "Buzzer.h"
#include "ILI9341.h"
#include "Logger.h"

#define PORT             80

#define MAX_WEB_QUEUE    24 // Will this be enough? Seems like that

#define FILE_BUFF_SIZE   32768

#define CHART_CO2        0
#define CHART_HUMD       1
#define CHART_TEMP       2

#define SCALE_5M         0
#define SCALE_1H         1
#define SCALE_1D         2

#define MSEC_PER_UPD_5M  5000   // 60 x-divs
#define MSEC_PER_UPD_1H  120000  // 30 x-divs
#define MSEC_PER_UPD_1D  2160000 // 40 x-divs

#define TS(x)  to_string(x)
#define TSC(x) (to_string(x) + ",")

#define MSSHORT(x) (x >> 16)
#define MSBYTE0(x) (x >> 24)
#define MSBYTE1(x) ((x & 0xFF0000) >> 16)
#define MSBYTE2(x) ((x & 0xFF00) >> 8)

using namespace std;

// ch_sc_xd -> 1stMSB Active Chart, 2ndB Active Scale, LSSSHORT X divisions
// status: LSb0 -> Client ready for update
struct ClientSock
{
	ClientSock* head;
	ClientSock* tail;
	int sock;
	int ch_sc_xd;
	float x_scale;
	int status;
	int id;
};

const string head =
"HTTP/1.1 200 OK\n\
Connection: keep-alive\n\
Cache-Control: no-store\n\
Content-Type: text/html\n\
Content-Length: ";

const string update_head =
"HTTP/1.1 200 OK\n\
Connection: keep-alive\n\
Content-Type: text/event-stream\n\
Cache-Control: no-store\n\
Keep-Alive: timeout=60, max=60\n\n";

const string db_file_head =
"HTTP/1.1 200 OK\n\
Content-Type: application/octet-stream\n\
Content-Disposition: attachment; filename=\"";

const string update_ok =
"HTTP/1.1 200 OK\n\
Content-Length: 0\n\n";

const string update_404 =
"HTTP/1.1 404 Not Found\n\
Connection: close\n\
Content-Type: text/html\n\
Content-Length: 176\n\n\
<body style=\"background-color:#2b2b2b;color:#b2b2b2;font-family:'GT Pressura Mono'\">\
<h1>This isn't the Page you're looking for...</h1>\
<h2>Move along... Move along</h2></body>";

#ifndef NDEBUG
const char* debug_wrts[] = {
"WRT_WARN",
"WRT_SOUND",
"WRT_SWITCH",
"WRT_SCALE",
"WRT_LCD",
"WRT_RDINGS",
"WRT_DB_FILE",
"WRT_DEL_FILE",
"WRT_MAIN_HTML",
"WRT_DEFAULTS",
"WRT_UPDATE_HEAD",
"WRT_ERROR" };
#endif

int listen_sock;

// Pointers to the Master Web Queue that will supply Master Writer thread with updates
volatile WebUpdate* web_queue_h;
volatile WebUpdate* web_queue_t;
pthread_mutex_t web_queue_lock;
volatile ClientSock* client_socks; // List of Client connections
pthread_mutex_t client_socks_lock;
sem_t sem_empty; // Semaphore that represents empty spaces in queue
sem_t sem_full; // Semaphore that represents full spaces in queue

void loadWebPage(string* str);
void* writerThread(void* param);
void* fileWriterThread(void* sock);
void* readerThread(void* client_sock);
WebUpdate* popWebQueue(); // Users must free recieved struct themselves
void freeWebQueue();
ClientSock* addClient(int sock);
void delClient(ClientSock* to_del);
ClientSock* findClientById(int id);
void closeClientSocks();
void updStorageSpace(float* free, float* fill_circ);
string formEvent(const string& name, const string& data);
string formDataCSV(int chart, int scale);
string cht2str(int chart);
string scl2str(int scale);
string sng2str(int song);
string tim2str(int t);
string f2s(float f, bool comma);
string f2sNo0(float f);
int byte2int(int c);

void* serverMain(void* param)
{
	struct sockaddr_in addr;
	int addr_len = sizeof(addr);
	
	// Init locks and semaphores
	pthread_mutex_init(&web_queue_lock, NULL);
	pthread_mutex_init(&client_socks_lock, NULL);

	sem_init(&sem_empty, 0, MAX_WEB_QUEUE);
	sem_init(&sem_full, 0, 0);
	initLogger();
	
	listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if(listen_sock == 0)
	{
		perror("Error creating socket");
		return NULL;
	}
	
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(PORT);
	
	memset(addr.sin_zero, 0, sizeof(addr.sin_zero));
	
	int ret = bind(listen_sock, (struct sockaddr*)&addr, addr_len);
	if(ret < 0)
	{
		perror("Error binding socket");
		return NULL;
	}
	
	ret = listen(listen_sock, 10);
	if(ret < 0)
	{
		perror("Error setting up server as listеner");
		return NULL;
	}
	
	pthread_t wthd;
	pthread_create(&wthd, &master_thread_attr, writerThread, NULL);

	while(1)
	{		
		DBPRINT("Listener Thread. Waiting for clients...\n\n");
		int new_client = accept(listen_sock, (struct sockaddr*)&addr, (socklen_t*)&addr_len);
		if(new_client < 0)
		{
			perror("Error accepting client connection into new socket");
			return NULL;
		}
		
		int enable = 1;
		setsockopt(new_client, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(int));
#ifndef NDEBUG
		int a = (int)ntohl(addr.sin_addr.s_addr);
		printf("Client ADDR: %d.%d.%d.%d:%d\n", MSBYTE0(a), MSBYTE1(a), MSBYTE2(a), a & 0xFF, ntohs(addr.sin_port));
#endif
		pthread_t rthd;
		pthread_create(&rthd, &master_thread_attr, readerThread, (void*)addClient(new_client));
	}
	
	return NULL;
}

void loadWebPage(string* str)
{
	FILE* f = fopen("./web/main.html", "rb");
	char c = 0;
	while(fread(&c, 1, 1, f))
	{
		str->push_back(c);
	}
	fclose(f);
}

void* writerThread(void* param)
{	
	DBPRINT("Master Writer Thread UP!\n");
	
	string source;
	loadWebPage(&source);
	
	float fil_text = 0.0f;
	float fil = 0.0f;
	
	unsigned int msec = 0;
	int ppm;
	float humd, temp;
	
	while(1)
	{
		WebUpdate* upd = popWebQueue(); // Semaphore blocks here if Queue is empty
		DBPRINT("Master Writer update operation: %s!\n", debug_wrts[upd->op]);
		string update;

		switch(upd->op)
		{
		case WRT_WARN:
		{
			int cw = MSSHORT(upd->co2w_snd);
			int hwl = MSBYTE0(upd->ht_warn);
			int hwh = MSBYTE1(upd->ht_warn);
			int twl = byte2int(MSBYTE2(upd->ht_warn));
			int twh = byte2int(upd->ht_warn & 0xFF);
			update = formEvent("warnings", TSC(cw) + TSC(hwl) + TSC(hwh) + TSC(twl) + TS(twh));
			// Critical Section Beg
			pthread_mutex_lock(&warning_levels_lock);
			
			co2_warning = cw;
			humd_warning_low = hwl;
			humd_warning_high = hwh;
			temp_warning_low = twl;
			temp_warning_high = twh;
			
			pthread_mutex_unlock(&warning_levels_lock);
			// Critical Section End
		}
			break;
		case WRT_SOUND:
			update = formEvent("sound", sng2str(upd->co2w_snd & 0xFFFF));
			// Critical Section Beg
			pthread_mutex_lock(&co2_warning_song_lock);
			
			co2_warning_song = upd->co2w_snd & 0xFFFF;
			
			pthread_mutex_unlock(&co2_warning_song_lock);
			// Critical Section End
			break;
		case WRT_SWITCH:
		case WRT_SCALE:
		{			
			int active_chart = MSBYTE0(upd->cdst->ch_sc_xd);
			int active_scale = MSBYTE1(upd->cdst->ch_sc_xd);
			int x_divs = upd->cdst->ch_sc_xd & 0xFFFF;
			if(upd->op == WRT_SWITCH)
			{
				update += formEvent("chart_switch", cht2str(active_chart));
			}
			else
			{
				update += formEvent("chart_scale", scl2str(active_scale));
			}
			
			update += formEvent("data", formDataCSV(active_chart, active_scale));
			update += formEvent("chart_vars", TSC(x_divs) + f2sNo0(upd->cdst->x_scale));
		}
			break;
		case WRT_LCD:
			update = formEvent("lcd_times", tim2str(upd->lcd));
			// Critical Section Beg
			pthread_mutex_lock(&lcd_on_off_time_lock);
			
			lcd_on_off_time = upd->lcd;
			
			pthread_mutex_unlock(&lcd_on_off_time_lock);
			// Critical Section End
			break;
		case WRT_RDINGS:
		{
			ppm = upd->ppm;
			humd = upd->humd;
			temp = upd->temp;
			update += formEvent("readings", TSC(ppm) + f2s(humd, 1) + f2s(temp, 0));
			
			if(msec % 5000 == 0)
			{
				int tf = (int)(temp * 10.0f) - (int)temp * 10;
				unsigned int rd_pack = ppm << 19 | (int)humd << 12 | (int)temp << 4 | tf;
				
				Reading rd;
				rd.dt = (unsigned int)time(NULL);
				rd.rd = rd_pack;
				logReading(rd);
			}
		}
			break;
		case WRT_MAIN_HTML:
			update = head + to_string(source.size()) + "\nKeep-Alive: timeout=60, max=160\n\n" + source;
			break;
		case WRT_DEFAULTS:
		{
			// Critical Section Beg
			pthread_mutex_lock(&warning_levels_lock);
			
			int cw = co2_warning;
			int hwl = humd_warning_low;
			int hwh = humd_warning_high;
			int twl = temp_warning_low;
			int twh = temp_warning_high;
			
			pthread_mutex_unlock(&warning_levels_lock);
			pthread_mutex_lock(&co2_warning_song_lock);
			
			int song = co2_warning_song;
			
			pthread_mutex_unlock(&co2_warning_song_lock);
			pthread_mutex_lock(&lcd_on_off_time_lock);
			
			int lcd_times = lcd_on_off_time;
			
			pthread_mutex_unlock(&lcd_on_off_time_lock);
			// Critical Section End
			updStorageSpace(&fil_text, &fil);
			int active_chart = MSBYTE0(upd->cdst->ch_sc_xd);
			int active_scale = MSBYTE1(upd->cdst->ch_sc_xd);
			int x_divs = upd->cdst->ch_sc_xd & 0xFFFF;
			
			update += formEvent("warnings", TSC(cw) + TSC(hwl) + TSC(hwh) + TSC(twl) + TS(twh));
			update += formEvent("storage", f2sNo0(fil) + "," + f2s(fil_text, 0));
			update += formEvent("sound", sng2str(song));
			update += formEvent("chart_switch", cht2str(active_chart));
			update += formEvent("chart_scale", scl2str(active_scale));
			update += formEvent("data", formDataCSV(active_chart, active_scale));
			update += formEvent("chart_vars", TSC(x_divs) + f2sNo0(upd->cdst->x_scale));
			update += formEvent("lcd_times", tim2str(lcd_times));
		}
			break;
		case WRT_UPDATE_HEAD:
			update = update_head;
			break;
		case WRT_ERROR:
			update = update_404;
			break;
		case WRT_DB_FILE:
		case WRT_DEL_FILE:
			break;
		default:
			free(upd);
			continue;
		}
		
		// Critical Section Beg
		pthread_mutex_lock(&client_socks_lock);
		
		ClientSock* tmp = (ClientSock*)client_socks;
		while(tmp != NULL)
		{
			
			switch(upd->op)
			{
			// Ensure that all clients get appropriate chart updates
			case WRT_RDINGS:
			{
				if(!(tmp->status & 0x1))
				{
					break;
				}
				
				int active_chart = MSBYTE0(tmp->ch_sc_xd);
				int active_scale = MSBYTE1(tmp->ch_sc_xd);
				float current_data_val;
				switch(active_chart)
				{
				case CHART_CO2:
					current_data_val = (float)ppm;
					break;
				case CHART_HUMD:
					current_data_val = humd;
					break;
				case CHART_TEMP:
				default:
					current_data_val = temp;
					break;
				}
				
				unsigned int msec_upd;
				switch(active_scale)
				{
				case SCALE_5M:
					msec_upd = MSEC_PER_UPD_5M;
					break;
				case SCALE_1H:
					msec_upd = MSEC_PER_UPD_1H;
					break;
				case SCALE_1D:
				default:
					msec_upd = MSEC_PER_UPD_1D;
					break;
				}
				
				if(msec % msec_upd == 0)
				{
					DBPRINT("Master Writer writing DATA_UPD to THD %d...\n", tmp->sock);
					string data_update = formEvent("data_upd", f2sNo0(current_data_val));
					write(tmp->sock, data_update.c_str(), data_update.size());
				}
				
				if(msec % 60000 == 0)
				{
					DBPRINT("Master Writer writing STORAGE to THD %d...\n", tmp->sock);
					updStorageSpace(&fil_text, &fil);
					update += formEvent("storage", f2sNo0(fil) + "," + f2s(fil_text, 0));
				}
				
				DBPRINT("Master Writer writing RDINGZ to THD %d...\n", tmp->sock);
				write(tmp->sock, update.c_str(), update.size());
			}
				break;
			// Global program settings
			case WRT_WARN:
			case WRT_SOUND:
			case WRT_LCD:
				if(tmp->status & 0x1)
				{
					DBPRINT("Master Writer writing WARN/SOUND/LCD to THD %d...\n", tmp->sock);
					write(tmp->sock, update.c_str(), update.size());
				}
				else if(upd->csrc == tmp)
				{
					DBPRINT("Master Writer writing OK to THD %d...\n", tmp->sock);
					write(tmp->sock, update_ok.c_str(), update_ok.size());
				}
				break;
			// Local client settings
			case WRT_SWITCH:
			case WRT_SCALE:
				if(upd->cdst == tmp)
				{
					DBPRINT("Master Writer writing SWITCH/SCALE to THD %d...\n", tmp->sock);
					write(tmp->sock, update.c_str(), update.size());
				}
				else if(upd->csrc == tmp)
				{
					DBPRINT("Master Writer writing OK to THD %d...\n", tmp->sock);
					write(tmp->sock, update_ok.c_str(), update_ok.size());
				}
				break;
			// Database file operations
			case WRT_DB_FILE:
				if(upd->cdst == tmp)
				{
					DBPRINT("Master Writer Spawns File Writer...\n");
					pthread_t fwthd;
					pthread_create(&fwthd, &master_thread_attr, fileWriterThread, (void*)tmp->sock);
				}
				break;
			case WRT_DEL_FILE:
				if(upd->cdst == tmp)
				{
					DBPRINT("Master Writer writing OK to THD %d...\n", tmp->sock);
					write(tmp->sock, update_ok.c_str(), update_ok.size());					
					deleteDBfile();
					DBPRINT("Master Writer removed readings DB file...\n");
					
					updStorageSpace(&fil_text, &fil);
					ClientSock* tmp2 = (ClientSock*)client_socks;
					while(tmp2 != NULL)
					{
						if(tmp2->status & 0x1)
						{
							string stor_upd = formEvent("storage", f2sNo0(fil) + "," + f2s(fil_text, 0));
							write(tmp2->sock, stor_upd.c_str(), stor_upd.size());
						}
						tmp2 = tmp2->tail;
					}
					DBPRINT("Master Writer updated all storage spaces...\n");
				}
				break;
			// Page setup
			case WRT_UPDATE_HEAD:
			case WRT_DEFAULTS:
			case WRT_MAIN_HTML:
			case WRT_ERROR:
				if(upd->cdst == tmp)
				{
					DBPRINT("Master Writer writing UPD_HEAD/DEFS/MAIN_HTML/ERR to THD %d...\n", tmp->sock);
					write(tmp->sock, update.c_str(), update.size());
				}
				break;
			default:
				break;
			}
			
			tmp = tmp->tail;
		}		
		
		pthread_mutex_unlock(&client_socks_lock);
		// Citical Section End
		if(msec >= MSEC_PER_UPD_1D) // Reset counter to avoid potential overflow issues
		{
			msec = 0;
		}
		if(upd->op == WRT_RDINGS) // Other operations must not advance update counter!
		{
			msec += update_period_ms;
		}
		
		free(upd);
	}
}

void* fileWriterThread(void* sock)
{
	DBPRINT("File Writer writing DATA FILE to THD %d...\n", (int)sock);
	FILE* f = getDBfileAndLock();
	fseek(f, 0L, SEEK_END);
	size_t fs = ftell(f);
	fseek(f, 0L, SEEK_SET);
	
	char fname[45];
	formDBfilename(fname);
	string fhead = db_file_head + string(fname) + "\"\nContent-Length: " + to_string(fs) + "\n\n";
	write((int)sock, fhead.c_str(), fhead.size());
	
	fseek(f, 0, SEEK_SET);
	char buff[FILE_BUFF_SIZE];
	while(1)
	{
		size_t rd = fread(buff, 1, FILE_BUFF_SIZE, f);
		int res;
		if(rd == 0) // No more file
		{
			break;
		}
		else if(rd < FILE_BUFF_SIZE) // Last pice of file left
		{
			res = write((int)sock, buff, rd);
			break;
		}
		else // Full file segment
		{
			res = write((int)sock, buff, FILE_BUFF_SIZE);
		}
		
		if(res < 0)
		{
			break;
		}
	}
	unlockDBfile();
	DBPRINT("File Writer finished successfully...\n");
	return NULL;
}

void* readerThread(void* client_sock)
{	
	ClientSock* s = (ClientSock*)client_sock;
	int res;
	
	//logError("Test loggin from reader thread", 1);
	
	// Critical Section Beg
	pthread_mutex_lock(&client_socks_lock);
	
	int sock = s->sock;
	// Load default Client values of client specific data
	s->ch_sc_xd = CHART_CO2 << 24 | SCALE_5M << 16 | 60;
	s->x_scale = 0.083333f;
	
	pthread_mutex_unlock(&client_socks_lock);
	// Critical Section End
	
	DBPRINT("Client Reader Thread UP! Socket ID: %d!\n", sock);
	
	char buff[2048];
	while(1)
	{
		DBPRINT("Client Reader Thread %d waiting for client input...\n", sock);
		res = read(sock, buff, 2048);
		if(res < 0)
		{
			perror("Client Reader Thread %d Read failed... Client Thread terminating!");
			delClient(s);
			return NULL; // Will this even work? IDK...
		}
		else if(res == 0) // Client closed connection, terminate thread!
		{
			DBPRINT("Client Reader Thread %d received empty request... Terminating!\n", sock);
			delClient(s);
			return NULL; // No, THIS will work!
		}
		
		string req = buff;// Request
		DBPRINT("CTHD%d REQ: %s\n\n",sock, req.substr(0, 60).c_str());
		req = req.substr(4, req.find("HTTP")-5);
		
		WebUpdate wupd;
		memset(&wupd, 0, sizeof(WebUpdate));
		wupd.cdst = s;
		wupd.csrc = s;

		size_t qmark = req.find('?');
		if(!req.compare("/"))
		{
			wupd.op = WRT_MAIN_HTML;
		}
		else if(!req.substr(0, 4).compare("/upd"))
		{			
			WebUpdate defs;
			memset(&defs, 0, sizeof(WebUpdate));
			defs.op = WRT_UPDATE_HEAD;
			defs.cdst = s;
			putWebQueue(&defs);
			
			int id = atoi(req.substr(8).c_str());
			// Critical Section Beg
			pthread_mutex_lock(&client_socks_lock);
			
			s->status |= 0x1; // This client is ready to recive updates
			s->id = id;
			
			pthread_mutex_unlock(&client_socks_lock);
			// Critical Section End
			
			wupd.op = WRT_DEFAULTS;
		}
		else if(!req.compare("/download"))
		{
			wupd.op = WRT_DB_FILE;
		}
		else if(!req.compare("/delete"))
		{
			wupd.op = WRT_DEL_FILE;
		}
		else if(qmark != string::npos) // Param changes came
		{
			req = req.substr(qmark+1);
			if(req.find("cw=") != string::npos) // Change Warnings levels
			{
				size_t amp = req.find('&');
				int co2_w = atoi(req.substr(3, amp-3).c_str());
				size_t p_amp = amp;
				amp = req.find('&', amp+1);
				int hwl = atoi(req.substr(p_amp+5, amp-p_amp-5).c_str()) & 0xFF;
				p_amp = amp;
				amp = req.find('&', amp+1);
				int hwh = atoi(req.substr(p_amp+5, amp-p_amp-5).c_str()) & 0xFF;
				p_amp = amp;
				amp = req.find('&', amp+1);
				int twl = atoi(req.substr(p_amp+5, amp-p_amp-5).c_str()) & 0xFF;
				p_amp = amp;
				amp = req.find('&', amp+1);
				int twh = atoi(req.substr(p_amp+5).c_str()) & 0xFF;
				
				wupd.op = WRT_WARN;
				wupd.co2w_snd = co2_w << 16;
				wupd.ht_warn = hwl << 24 | hwh << 16 | twl << 8 | twh;
				wupd.cdst = NULL; // This is global update
			}
			else if(req.find("cs=") != string::npos) // Change Sound
			{
				wupd.op = WRT_SOUND;
				wupd.co2w_snd = atoi(req.substr(3).c_str());
				wupd.cdst = NULL;
			}
			else if(req.find("sw=") != string::npos) // Chart Switch
			{				
				wupd.op = WRT_SWITCH;
				int id = atoi(req.substr(req.find('&')+4).c_str());
				ClientSock* mc = findClientById(id); // Main Client
				wupd.cdst = mc;
				// Critical Section Beg
				pthread_mutex_lock(&client_socks_lock);
				
				mc->ch_sc_xd = atoi(req.substr(3).c_str()) << 24 | mc->ch_sc_xd & 0xFFFFFF;
				
				pthread_mutex_unlock(&client_socks_lock);
				// Critical Section End
			}
			else if(req.find("sc=") != string::npos) // Chart Scale
			{
				int ascale = atoi(req.substr(3).c_str());
				int x_divs;
				float x_scale;
				switch(ascale)
				{
				case SCALE_5M:
					x_divs = 60; // Total number of x line coordinates (seconds, minues, etc.)
					x_scale = 0.083333f; // Multiplier of x coordinate index (i of the loop * x_scale)
					break;
				case SCALE_1H:
					x_divs = 30;
					x_scale = 2.0f;
					break;
				case SCALE_1D:
					x_divs = 40;
					x_scale = 0.6;
					break;
				default:
					break;
				}
				
				wupd.op = WRT_SCALE;
				int id = atoi(req.substr(req.find('&')+4).c_str());
				ClientSock* mc = findClientById(id);
				wupd.cdst = mc;
				// Critical Section Beg
				pthread_mutex_lock(&client_socks_lock);
				
				mc->ch_sc_xd = ascale << 16 | mc->ch_sc_xd & 0xFF00FFFF;
				mc->ch_sc_xd = x_divs | mc->ch_sc_xd & 0xFFFF0000;
				mc->x_scale = x_scale;
				
				pthread_mutex_unlock(&client_socks_lock);
				// Critical Section End
			}
			else if(req.find("lo=") != string::npos) // LCD ON/OFF times
			{
				int onh = atoi(req.substr(3, 2).c_str());
				int onm = atoi(req.substr(6, 2).c_str());
				int offh = atoi(req.substr(12, 2).c_str());
				int offm =  atoi(req.substr(15, 2).c_str());

				wupd.op = WRT_LCD;
				wupd.lcd = onh << 24 | onm << 16 | offh << 8 | offm;
				wupd.cdst = NULL;
			}
		}
		else
		{
			wupd.op = WRT_ERROR;
			DBPRINT("\nCTHD%d: ERRRQ: %s\n\n", sock, req.c_str());
		}
		
		putWebQueue(&wupd);		
	}
	
	DBPRINT("It's almost impossible but, Client Thread %d is out of the loop, terminating...\n", sock);
	return NULL;
}

void putWebQueue(const WebUpdate* update)
{
	WebUpdate* tmp = (WebUpdate*)malloc(sizeof(WebUpdate));
	memcpy(tmp, update, sizeof(WebUpdate));
	tmp->head = NULL;
	
	// Critical Section Beg
	sem_wait(&sem_empty);
	pthread_mutex_lock(&web_queue_lock);
	
	if(web_queue_h != NULL)
	{
		web_queue_h->head = tmp;
	}
	else // First Queue element
	{
		web_queue_t = (volatile WebUpdate*)tmp;
	}
	tmp->tail = (WebUpdate*)web_queue_h;
	web_queue_h = tmp;
	
	pthread_mutex_unlock(&web_queue_lock);
	sem_post(&sem_full);
	// Critical Section End
}

WebUpdate* popWebQueue()
{
	// Critical Section Beg
	sem_wait(&sem_full);
	pthread_mutex_lock(&web_queue_lock);
	
	WebUpdate* tmp = (WebUpdate*)web_queue_t;
	if(web_queue_t->head != NULL) // It's not only element in Queue
	{
		web_queue_t->head->tail = NULL;
		web_queue_t = web_queue_t->head;
	}
	else // It's only element left in Queue
	{
		web_queue_h = NULL;
		web_queue_t = NULL;
	}
	
	pthread_mutex_unlock(&web_queue_lock);
	sem_post(&sem_empty);
	// Critical Section End
	tmp->head = NULL;
	return tmp;
}

void freeWebQueue()
{
	// Critical Section Beg
	pthread_mutex_lock(&web_queue_lock);
	
	if(web_queue_h == NULL)
	{
		pthread_mutex_unlock(&web_queue_lock);
		return;
	}
	
	WebUpdate* tmp = (WebUpdate*)web_queue_h;
	while(tmp != NULL)
	{
		WebUpdate* to_del = tmp;
		tmp = tmp->tail;
		free(to_del);
	}
	
	pthread_mutex_unlock(&web_queue_lock);
	// Critical Section End
}

void deinitWebServer()
{
	freeWebQueue();
	close(listen_sock);
	closeClientSocks();	
	pthread_mutex_destroy(&web_queue_lock);
	pthread_mutex_destroy(&client_socks_lock);
	sem_destroy(&sem_empty);
	sem_destroy(&sem_full);
}

ClientSock* addClient(int sock)
{
	ClientSock* tmp = (ClientSock*)malloc(sizeof(ClientSock));
	memset(tmp, 0, sizeof(ClientSock));
	tmp->sock = sock;
	// Critical Section Beg
	pthread_mutex_lock(&client_socks_lock);
	
	if(client_socks != NULL)
	{
		client_socks->head = tmp;
	}
	tmp->tail = (ClientSock*)client_socks;
	client_socks = (volatile ClientSock*)tmp;
	
	pthread_mutex_unlock(&client_socks_lock);
	// Critical Section End
	
	return tmp;
}

void delClient(ClientSock* to_del)
{
	// Critical Section Beg
	pthread_mutex_lock(&client_socks_lock);
#ifndef NDEBUG
	printf("Deleting Client %d|E:%08X|H:%08X|T:%08X|CS:%08X|\n", to_del->sock, (unsigned int)to_del,
	(unsigned int)to_del->head, (unsigned int)to_del->tail, (unsigned int)client_socks);
#endif
	
	if(to_del->head != NULL && to_del->tail != NULL) // Located in the middle of a List
	{
		to_del->head->tail = to_del->tail;
		to_del->tail->head = to_del->head;
	}
	else if(to_del->head != NULL) // Located in the end of a List
	{
		to_del->head->tail = NULL;
	}
	else if(to_del->tail != NULL) // Located in the beginning of a List
	{
		to_del->tail->head = NULL;
		client_socks = (volatile ClientSock*)to_del->tail;
	}
	else // One and only element in a List
	{
		client_socks = NULL;
	}
	pthread_mutex_unlock(&client_socks_lock);
	// Critical Section End
	close(to_del->sock);
	free(to_del);
	DBPRINT("Deleted!\n");
}

ClientSock* findClientById(int id)
{
	// Critical Section Beg
	pthread_mutex_lock(&client_socks_lock);
	
	ClientSock* tmp = (ClientSock*)client_socks;
	while(tmp != NULL)
	{
		if(tmp->id == id)
		{
			break;
		}
		tmp = tmp->tail;
	}
	
	pthread_mutex_unlock(&client_socks_lock);
	// Critical Section End
	
	return tmp;
}

void closeClientSocks()
{
	// Critical Section Beg
	pthread_mutex_lock(&client_socks_lock);
	
	if(client_socks == NULL)
	{
		pthread_mutex_unlock(&client_socks_lock);
		return;
	}
	
	ClientSock* tmp = (ClientSock*)client_socks;
	while(tmp != NULL)
	{
		ClientSock* to_del = tmp;
		tmp = tmp->tail;
		close(to_del->sock);
		free(to_del);
	}
	
	pthread_mutex_unlock(&client_socks_lock);
	// Critical Section End
}

void updStorageSpace(float* free, float* fill_circ)
{
	struct statvfs vfs;
	statvfs("./log", &vfs);
	double free_d = (double)vfs.f_bsize * vfs.f_bavail/1000000000.0;
	*free = ceil(free_d * 10.0)/10.0;
	double max = (double)vfs.f_frsize * vfs.f_blocks/1000000000.0;
	max = ceil(max * 10.0f)/10.0f;
	// Percentage of free space left on Pi, from 0.0f to 2.0f
	*fill_circ = (1.0f - *free/max) * 2.0f;
}

string formEvent(const string& name, const string& data)
{
	return string("event: ") + name + "\n" + "data: " + data + "\n\n";
}

string formDataCSV(int chart, int scale)
{
	int max_off, step;
	
	switch(scale)
	{
	case SCALE_5M:
		max_off = 60;
		step = 1;
		break;
	case SCALE_1H:
		max_off = 720;   // 3600 s / 5 s
		step = 24;       // 120 s / 5 s for 30 X-aix divisions
		break;
	case SCALE_1D:
		max_off = 17280; // 86400 s / 5 s
		step = 432;      // 2160 s / 5 s for 40 X-aix divisions
		break;
	default:
		break;
	}
	
	string res;
	for(int i = max_off-1; i >= 0; i -= step)
	{
		Reading r = getReading(i);
		switch(chart)
		{
		case CHART_CO2:
			res += TSC(r.rd >> 19);
			break;
		case CHART_HUMD:
			res += TSC((r.rd & 0x7F000) >> 12);
			break;
		case CHART_TEMP:
			res += TS((r.rd & 0xFF0) >> 4) + "." + TSC(r.rd & 0xF);
			break;
		default:
			break;
		}
	}
	res.pop_back();
	return res;
}

string cht2str(int chart)
{
	switch(chart)
	{
	case CHART_CO2:
		return string("co2_chart");
	case CHART_HUMD:
		return string("humd_chart");
	case CHART_TEMP:
		return string("temp_chart");
	default:
		return string("err_chart");
	}
}

string scl2str(int scale)
{
	switch(scale)
	{
	case SCALE_5M:
		return string("5m");
	case SCALE_1H:
		return string("1h");
	case SCALE_1D:
		return string("1d");
	default:
		return string("err_scale");
	}
}

string sng2str(int song)
{
	switch(song)
	{
	case SNG_NONE:
		return string("none");
	case SNG_BEEP:
		return string("beep");
	case SNG_SOS:
		return string("sos");
	case SNG_CO2:
		return string("co2");
	case SNG_IMPERIAL_MARCH:
		return string("imperial_march");
	case SNG_STAR_TREK:
		return string("star_trek");
	case SNG_DOOM:
		return string("doom");
	default:
		return string("err_song");
	}
}

string tim2str(int t)
{
	char time[48];
	sprintf(time, "%02d:%02d,%02d:%02d", MSBYTE0(t), MSBYTE1(t), MSBYTE2(t), t & 0xFF);
	return string(time);
}

string f2s(float f, bool comma)
{
	char buff[10];
	sprintf(buff, "%-3.1f", f);
	string com = comma ? "," : "";
	return string(buff) + com;
}

string f2sNo0(float f)
{
	ostringstream oss;
	oss << setprecision(6) << noshowpoint << f;
	return oss.str();
}

int byte2int(int c)
{
	if(c & 0x80) // "Negative" bit is set, this is negative byte
	{
		c |= 0xFFFFFF00;
	}
	return c;
}

/*/ THIS PRINTS OUT ENTIRE CLIENT LINKED-LIST QUEUE
ClientSock* tmp = (ClientSock*)client_socks;
while(tmp != NULL)
{
	printf("<----H%08X|%d_%08X|%08XT---->", tmp->head, tmp->sock, tmp, tmp->tail);
	tmp = tmp->tail;
}
printf("\n");
/*/	

/*/ FAKE SENSORS FOR TESTING
void* debugSensorThread(void* param)
{
	while(1)
	{
		WebUpdate wupd;
		memset(&wupd, 0, sizeof(WebUpdate));
		wupd.op = WRT_RDINGS;
		wupd.ppm = 200 + rand() % 2000;
		wupd.humd = 5.0f + rand() % 90 + (rand() % 9)/10.0f;
		wupd.temp = 12.0f + rand() % 32 + (rand() % 9)/10.0f;
		
		putWebQueue(&wupd);
		
		sleep(1);
	}
}
/*/