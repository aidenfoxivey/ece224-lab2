#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <io.h>
#include <sys/alt_alarm.h>
#include <sys/resource.h>
#include <system.h>

#include "alt_types.h"
#include "diskio.h"
#include "fatfs.h"
#include "ff.h"
#include "monitor.h"
#include "uart.h"

#include "altera_avalon_timer.h"
#include "altera_avalon_timer_regs.h"
#include <altera_up_avalon_audio.h>
#include <altera_up_avalon_audio_and_video_config.h>

/* https://www.intel.com/content/www/us/en/docs/programmable/683130/23-3/displaying-characters-on-the-lcd.html */
#define PSTR(_a) _a
#define ESC 27
#define LCD_CLEAR "[2J"
#define MIN(a,b) (((a)<(b))?(a):(b))

enum State {
	STOPPED, PAUSED, RUNNING_REGULAR, RUNNING_HALF, RUNNING_DOUBLE, RUNNING_MONO,
};

#define REGULAR_SPEED 3
#define HALF_SPEED 2
#define DOUBLE_SPEED 6
#define NUM_OF_FILES 13
#define COUNT_RELEASED 50

static alt_alarm alarm;
static unsigned long Systick = 0;
static volatile unsigned short Timer; /* 1000Hz increment timer */

static void timer_ISR(void* context, alt_32 id);
static void button_ISR(void* context, alt_32 id);
static void printLCD();
int isWav(char* filename);
static void printLCD(char *filename, int song_idx, enum State state);

static alt_u32 TimerFunction(void* context) {
	static unsigned short wTimer10ms = 0;

	(void) context;

	Systick++;
	wTimer10ms++;
	Timer++; /* Performance counter for this module */

	if (wTimer10ms == 10) {
		wTimer10ms = 0;
		ffs_DiskIOTimerproc(); /* Drive timer procedure of low level disk I/O module */
	}

	return (1);
}

static void IoInit(void) {
	uart0_init(115200);
	ffs_DiskIOInit();
	alt_alarm_start(&alarm, 1, &TimerFunction, NULL);
}

uint32_t acc_size; /* Work register for fs command */
uint16_t acc_files, acc_dirs;
FILINFO Finfo;
#if _USE_LFN
char Lfname[512];
#endif

char Line[256]; /* Console input buffer */

FATFS Fatfs[_VOLUMES]; /* File system object for each logical drive */
FIL File1, File2; /* File objects */
DIR Dir; /* Directory object */
uint8_t Buff[1024] __attribute__((aligned(4))); /* Working buffer */

enum State state = STOPPED;
int file_index = 0;
int traverse_previous = 0;
int traverse_next = 0;
uint32_t remaining_bytes_in_buff;
int i;
int mono = 0;

static FRESULT scan_files(char* path) {
	DIR dirs;
	FRESULT res;
	uint8_t i;
	char* fn;

	if ((res = f_opendir(&dirs, path)) == FR_OK) {
		i = (uint8_t) strlen(path);
		while (((res = f_readdir(&dirs, &Finfo)) == FR_OK) && Finfo.fname[0]) {
			if (_FS_RPATH && Finfo.fname[0] == '.')
				continue;
#if _USE_LFN
			fn = *Finfo.lfname ? Finfo.lfname : Finfo.fname;
#else
			fn = Finfo.fname;
#endif
			if (Finfo.fattrib & AM_DIR) {
				acc_dirs++;
				*(path + i) = '/';
				strcpy(path + i + 1, fn);
				res = scan_files(path);
				*(path + i) = '\0';
				if (res != FR_OK)
					break;
			} else {
				//      xprintf("%s/%s\n", path, fn);
				acc_files++;
				acc_size += Finfo.fsize;
			}
		}
	}

	return res;
}

//                put_rc(f_mount((uint8_t) p1, &Fatfs[p1]));

static void put_rc(FRESULT rc) {
	const char* str = "OK\0"
			"DISK_ERR\0"
			"INT_ERR\0"
			"NOT_READY\0"
			"NO_FILE\0"
			"NO_PATH\0"
			"INVALID_NAME\0"
			"DENIED\0"
			"EXIST\0"
			"INVALID_OBJECT\0"
			"WRITE_PROTECTED\0"
			"INVALID_DRIVE\0"
			"NOT_ENABLED\0"
			"NO_FILE_SYSTEM\0"
			"MKFS_ABORTED\0"
			"TIMEOUT\0"
			"LOCKED\0"
			"NOT_ENOUGH_CORE\0"
			"TOO_MANY_OPEN_FILES\0";
	FRESULT i;

	for (i = 0; i != rc && *str; i++) {
		while (*str++)
			;
	}
	xprintf("rc=%u FR_%s\n", (uint32_t) rc, str);
}

static void display_help(void) {
	xputs("dd <phy_drv#> [<sector>] - Dump sector\n"
			"di <phy_drv#> - Initialize disk\n"
			"ds <phy_drv#> - Show disk status\n"
			"bd <addr> - Dump R/W buffer\n"
			"be <addr> [<data>] ... - Edit R/W buffer\n"
			"br <phy_drv#> <sector> [<n>] - Read disk into R/W buffer\n"
			"bf <n> - Fill working buffer\n"
			"fc - Close a file\n"
			"fd <len> - Read and dump file from current fp\n"
			"fe - Seek file pointer\n"
			"fi <log drv#> - Force initialize the logical drive\n"
			"fl [<path>] - Directory listing\n"
			"fo <mode> <file> - Open a file\n"
			"fp -  (to be added by you) \n"
			"fr <len> - Read file\n"
			"fs [<path>] - Show logical drive status\n"
			"fz [<len>] - Get/Set transfer unit for fr/fw commands\n"
			"h view help (this)\n");
}

int isWav(char* filename);
static void printLCD();

static void timer_ISR(void* context, alt_32 id);
static void button_ISR(void* context, alt_32 id);

char file_names[14][20];
unsigned long sizes[14];
int names_index = 11;
int paused = 0;
int track_changed = 0;
int stopped = 1;
int count_released = 0;
int button = 0;
int done_playing = 0;

int button_pressed = 0;
int button_state = 0;

int main(void) {
	alt_irq_register(TIMER_0_IRQ, (void*) 0, timer_ISR);
	alt_irq_register(BUTTON_PIO_IRQ, (void*) 0, button_ISR);
	IOWR(BUTTON_PIO_BASE, 3, 0);
	IOWR(BUTTON_PIO_BASE, 2, 0xF);
	IOWR_ALTERA_AVALON_TIMER_STATUS(TIMER_0_BASE, 0);
	IOWR_ALTERA_AVALON_TIMER_PERIODH(TIMER_0_BASE, 0x2);
	IOWR_ALTERA_AVALON_TIMER_PERIODL(TIMER_0_BASE, 0xFFFF);
	IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x8);

	int ii;
	int fifospace;
	char *ptr, *ptr2;
	long p1, p2, p3;
	uint8_t res, b1, drv = 0;
	uint16_t w1;
	uint32_t s1, s2, cnt, blen = sizeof(Buff);
	static const uint8_t ft[] = { 0, 12, 16, 32 };
	uint32_t ofs = 0, sect = 0, blk[2];
	FATFS* fs; /* Pointer to file system object */

	alt_up_audio_dev* audio_dev;
	/* used for audio record/playback */
	unsigned int l_buf;
	unsigned int r_buf;

// open the Audio port
	audio_dev = alt_up_audio_open_dev("/dev/Audio");
	if (audio_dev == NULL)
		alt_printf("Error: could not open audio device \n");
	else
		alt_printf("Opened audio device \n");

	IoInit();

	IOWR(SEVEN_SEG_PIO_BASE, 1, 0x0007);

#if _USE_LFN
	Finfo.lfname = Lfname;
	Finfo.lfsize = sizeof(Lfname);
#endif
	/* DI: Disk Initialize */
	xprintf("rc=%d\n", (uint16_t) disk_initialize((uint8_t) 0));

	/* FI: File Initialize */
	put_rc(f_mount((uint8_t) 0, &Fatfs[0]));

	/* FL: Load file names into array */
	while (*ptr == ' ')
		ptr++;

	res = f_opendir(&Dir, ptr);

	if (res) {
		put_rc(res);
	}

	p1 = s1 = s2 = 0;
	int index = 0;

	for (;;) {
		res = f_readdir(&Dir, &Finfo);

		if ((res != FR_OK) || !Finfo.fname[0])
			break;

		if (Finfo.fattrib & AM_DIR) {
			s2++;
		} else {
			s1++;
			p1 += Finfo.fsize;
		}

		if (isWav(&(Finfo.fname[0]))) {
			sizes[index] = Finfo.fsize;
			strcpy(&file_names[index], &(Finfo.fname[0]));
			index++;
		}
	}

	// init the LCD 
	printLCD(file_names[0], 0, state);

	for (int i = 0; i < 14; i++) {
		printf("File Index: %d\tFile Name: %s\n", i, file_names[i]);
	}

	printf("Cannoli\n");

	int speed_conversion = 4;
	
	while (1) {
		/* If the previous button is pressed */
		if (traverse_previous == 1) {
			state = STOPPED;
			if (file_index == 0) {
				file_index = 13;
			} else {
				file_index--;
			}

			printLCD(file_names[file_index], file_index, state);
			printf("File Index: %d\tFile Name: %s\n", file_index,
					file_names[file_index]);
			traverse_previous = 0;
		}

		/* If the next button is pressed */
		if (traverse_next == 1) {
			state = STOPPED;
			if (file_index == 13) {
				file_index = 0;
			} else {
				file_index++;
			}
			printLCD(file_names[file_index], file_index, state);
			printf("File Index: %d\tFile Name: %s\n", file_index,
					file_names[file_index]);
			traverse_next = 0;
		}
		// STOPPED, PAUSED, RUNNING_REGULAR, RUNNING_HALF, RUNNING_DOUBLE, RUNNING_MONO,
		if (state == RUNNING_REGULAR || state == RUNNING_DOUBLE || state == RUNNING_HALF || state == RUNNING_MONO) {
			speed_conversion = 4; /* Set speed conversion */
			mono = 0;
			if (state == RUNNING_DOUBLE) speed_conversion = 6;
			if (state == RUNNING_HALF) speed_conversion = 2;
			if (state == RUNNING_MONO) mono = 1;
			p1 = sizes[file_index]; /* Get the size of the file */
			
			/* Open the file */
			while (*ptr == ' ')
				ptr++;
			
			put_rc(f_open(&File1, file_names[file_index], 1));
		}
		
		while (p1 > 0 && (state == RUNNING_REGULAR || state == RUNNING_DOUBLE || state == RUNNING_HALF || state == RUNNING_MONO)) {
            // if remaining file size is greater than buffer, subtract and continue
			if ((uint32_t) p1 >= blen) 
			{
				cnt = blen;
				p1 -= blen;
			} 
			else 
			{
				cnt = p1;
				p1 = 0;
			}

			/* Read from file pointer */
			res = f_read(&File1, Buff, cnt, &s2);
			if (res != FR_OK) {
				put_rc(res); // output a read error if a read error occurs
				break;
			}

			remaining_bytes_in_buff = s2;

			while (remaining_bytes_in_buff > 0) {
				uint32_t min_bytes_to_write;

				/* Space in the right FIFO */
				uint32_t right_space = (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT)) * speed_conversion;
				/* Space of bytes in the left FIFO */
				uint32_t left_space = (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_LEFT)) * speed_conversion;

				/* Calculate minimum space available in both FIFO */
				min_bytes_to_write = MIN(left_space, right_space);

                /* don't write more bytes than is left in the buffer */
				if (min_bytes_to_write > remaining_bytes_in_buff) {
					min_bytes_to_write = remaining_bytes_in_buff;
				}

				/* Get the position of the next byte to be written to the FIFO in the buffer */
				uint32_t start_position_buff = s2 - remaining_bytes_in_buff;

				/* Loop from the next byte to be written to in the buffer to the minimum number of bytes available */
				for (i = start_position_buff; i < start_position_buff + min_bytes_to_write; i += speed_conversion) {
					/* Copy buffer data to right buffer and left buffer */
					memcpy(&l_buf, Buff + i, 2);
					memcpy(&r_buf, Buff + i + 2, 2);

					/**
					 * if mono, then write `l_buf` to both right and left FIFOs
					 * otherwise, write `l_buf` to left FIFO and `r_buf` to right FIFO
					 */
					if (mono) {
						alt_up_audio_write_fifo(audio_dev, &(l_buf), 1, ALT_UP_AUDIO_LEFT);
						alt_up_audio_write_fifo(audio_dev, &(l_buf), 1, ALT_UP_AUDIO_RIGHT);
					} else {
						alt_up_audio_write_fifo(audio_dev, &(l_buf), 1, ALT_UP_AUDIO_LEFT);
						alt_up_audio_write_fifo(audio_dev, &(r_buf), 1, ALT_UP_AUDIO_RIGHT);
					}
				}

				/* Update the number of remaining bytes in buffer */
				remaining_bytes_in_buff -= min_bytes_to_write;
			}
		}
		
		state = STOPPED;
	}

	return (0);
}

int isWav(char* filename) {
	int x = (int) strlen(filename);
	if (filename[x - 4] == '.' && filename[x - 3] == 'W'
			&& filename[x - 2] == 'A' && filename[x - 1] == 'V')
		return 1;
	else {
		return 0;
	}
}

static void button_ISR(void* context, alt_32 id) {
	/* Writein any value to `edgecapture` clears all bits to 0 */
	IOWR(BUTTON_PIO_BASE, 3, 0);

	/* Disable button interrupts */
	IOWR(BUTTON_PIO_BASE, 2, 0);

	/**
	 * Write 0b0101 to Timer 0 control registers
	 * the first bit triggers the interrupt
	 * the third bit starts the timer (counting down) 
	 */
	IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
}

static void timer_ISR(void* context, alt_32 id) {
	int current_value = IORD(BUTTON_PIO_BASE, 0);
	if (button_state == 0 && current_value != 0xF) {
		button_pressed = current_value;
		button_state = 1;
	}
	if (current_value == 0xF) {
		count_released++;
        /* modified count_released from 20 to 50 to increase the debounce time */
		if (count_released < COUNT_RELEASED)
			IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
	} else {
		count_released = 0;
		IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
	}
	if (count_released == COUNT_RELEASED) {
		IOWR(LED_PIO_BASE, 0, 0x0);
		count_released = 0;
		button_state = 0;
		IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x8);
		IOWR(BUTTON_PIO_BASE, 2, 0xF);

		if (button_pressed == 7) { /* PREVIOUS BUTTON */
			traverse_previous = 1;
			button_pressed = 0;
		} else if (button_pressed == 11) { /* STOP BUTTON */
			state = STOPPED;
			button_pressed = 0;
		} else if (button_pressed == 13) { /* PAUSE BUTTON */
            /* if state is paused or stopped, then read switches and set the value */
			if (state == PAUSED || state == STOPPED) {
                /* switch reading to a certain value */
				int switch_0 = IORD(SWITCH_PIO_BASE, 0) & 0x1;
				int switch_1 = IORD(SWITCH_PIO_BASE, 0) & 0x2;

				if (switch_0 && switch_1) state = RUNNING_REGULAR;
				if (switch_0 && !switch_1) state = RUNNING_HALF;
				if (!switch_0 && switch_1) state = RUNNING_DOUBLE;
				if (!switch_0 && !switch_1) state = RUNNING_MONO;

            /* if state is running at any speed, then set to stopped */
			} else if (state == RUNNING_REGULAR || state == RUNNING_DOUBLE
					|| state == RUNNING_HALF) {
				state = PAUSED;
			}
			button_pressed = 0;
		} else if (button_pressed == 14) { /* NEXT BUTTON */
			traverse_next = 1;
			button_pressed = 0;
		}
		printLCD(file_names[file_index], file_index, state);
	}
}

static void printLCD(char *filename, int song_idx, enum State state) {
	FILE* lcd = fopen("/dev/lcd_display", "w");
	fprintf(lcd, "%c%s", ESC, LCD_CLEAR);

	fprintf(lcd, "%i %s", song_idx, filename);

	switch (state) {
		case STOPPED:
		fprintf(lcd, "\nSTOPPED");
		break;
		case PAUSED:
		fprintf(lcd, "\nPAUSED");
		break;
		case RUNNING_REGULAR:
		fprintf(lcd, "\nPBACK-NORM SPD");
		break;
		case RUNNING_HALF:
		fprintf(lcd, "\nPBACK–HALF SPD");
		break;
		case RUNNING_DOUBLE:
		fprintf(lcd, "\nPBACK–DBL SPD");
		break;
		case RUNNING_MONO:
		fprintf(lcd, "\nPBACK–MONO");
		break;
	}

	fclose(lcd);
}
