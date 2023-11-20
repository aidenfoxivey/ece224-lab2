#include <_types/_uint8_t.h>
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

#include <altera_up_avalon_audio.h>
#include <altera_up_avalon_audio_and_video_config.h>

/* https://www.intel.com/content/www/us/en/docs/programmable/683130/23-3/displaying-characters-on-the-lcd.html */
#define PSTR(_a) _a
#define ESCAPE 26
#define LCD_CLEAR "[2J"
#define REGULAR_SPEED 3
#define HALF_SPEED 2
#define DOUBLE_SPEED 6

static alt_alarm alarm;
static unsigned long Systick = 0;
static volatile unsigned short Timer; /* 1000Hz increment timer */

static void timer_ISR(void* context, alt_32 id);
static void button_ISR(void* context, alt_32 id);
static void printLCD();
int isWav(char* filename);

static alt_u32 TimerFunction(void* context)
{
    static unsigned short wTimer10ms = 0;

    (void)context;

    Systick++;
    wTimer10ms++;
    Timer++; /* Performance counter for this module */

    if (wTimer10ms == 10) {
        wTimer10ms = 0;
        ffs_DiskIOTimerproc(); /* Drive timer procedure of low level disk I/O module */
    }

    return (1);
}

static void IoInit(void)
{
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

static FRESULT scan_files(char* path)
{
    DIR dirs;
    FRESULT res;
    uint8_t i;
    char* fn;

    if ((res = f_opendir(&dirs, path)) == FR_OK) {
        i = (uint8_t)strlen(path);
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

static void put_rc(FRESULT rc)
{
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
    xprintf("rc=%u FR_%s\n", (uint32_t)rc, str);
}

static void display_help(void)
{
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
static void TestLCD();

static void timer_ISR(void* context, alt_32 id);
static void button_ISR(void* context, alt_32 id);

char names[14][20];
unsigned long sizes[14];
int xx;
int names_index = 11;
int paused = 0;
int track_changed = 0;
int stopped = 1;
int count_released = 0;
int button = 0;
int done_playing = 0;
int state = 3;

int main(void)
{
    alt_irq_register(TIMER_0_IRQ, (void*)0, timer_ISR);
    alt_irq_register(BUTTON_PIO_IRQ, (void*)0, button_ISR);
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

    xputs(PSTR("FatFs module test monitor\n"));
    xputs(_USE_LFN ? "LFN Enabled" : "LFN Disabled");
    xprintf(", Code page: %u\n", _CODE_PAGE);

    display_help();

#if _USE_LFN
    Finfo.lfname = Lfname;
    Finfo.lfsize = sizeof(Lfname);
#endif

    for (;;) {

        get_line(Line, sizeof(Line));

        ptr = Line;
        switch (*ptr++) {

        case 'm': /* System memroy/register controls */
            switch (*ptr++) {
            case 'd': /* md <address> [<count>] - Dump memory */
                if (!xatoi(&ptr, &p1))
                    break;
                if (!xatoi(&ptr, &p2))
                    p2 = 128;
                for (ptr = (char*)p1; p2 >= 16; ptr += 16, p2 -= 16)
                    put_dump((uint8_t*)ptr, (uint32_t)ptr, 16);
                if (p2)
                    put_dump((uint8_t*)ptr, (uint32_t)ptr, p2);
                break;
            }
            break;

        case 'd': /* Disk I/O layer controls */
            switch (*ptr++) {
            case 'd': /* dd [<drv> [<lba>]] - Dump secrtor */
                if (!xatoi(&ptr, &p1)) {
                    p1 = drv;
                } else {
                    if (!xatoi(&ptr, &p2))
                        p2 = sect;
                }
                drv = (uint8_t)p1;
                sect = p2 + 1;
                res = disk_read((uint8_t)p1, Buff, p2, 1);
                if (res) {
                    xprintf("rc=%d\n", (uint16_t)res);
                    break;
                }
                xprintf("D:%lu S:%lu\n", p1, p2);
                for (ptr = (char*)Buff, ofs = 0; ofs < 0x200; ptr += 16, ofs += 16)
                    put_dump((uint8_t*)ptr, ofs, 16);
                break;

            case 'i': /* di <drv> - Initialize disk */
                if (!xatoi(&ptr, &p1))
                    break;
                xprintf("rc=%d\n", (uint16_t)disk_initialize((uint8_t)p1));
                break;

            case 's': /* ds <drv> - Show disk status */
                if (!xatoi(&ptr, &p1))
                    break;
                if (disk_ioctl((uint8_t)p1, GET_SECTOR_COUNT, &p2) == RES_OK) {
                    xprintf("Drive size: %lu sectors\n", p2);
                }
                if (disk_ioctl((uint8_t)p1, GET_SECTOR_SIZE, &w1) == RES_OK) {
                    xprintf("Sector size: %u bytes\n", w1);
                }
                if (disk_ioctl((uint8_t)p1, GET_BLOCK_SIZE, &p2) == RES_OK) {
                    xprintf("Block size: %lu sectors\n", p2);
                }
                if (disk_ioctl((uint8_t)p1, MMC_GET_TYPE, &b1) == RES_OK) {
                    xprintf("MMC/SDC type: %u\n", b1);
                }
                if (disk_ioctl((uint8_t)p1, MMC_GET_CSD, Buff) == RES_OK) {
                    xputs("CSD:\n");
                    put_dump(Buff, 0, 16);
                }
                if (disk_ioctl((uint8_t)p1, MMC_GET_CID, Buff) == RES_OK) {
                    xputs("CID:\n");
                    put_dump(Buff, 0, 16);
                }
                if (disk_ioctl((uint8_t)p1, MMC_GET_OCR, Buff) == RES_OK) {
                    xputs("OCR:\n");
                    put_dump(Buff, 0, 4);
                }
                if (disk_ioctl((uint8_t)p1, MMC_GET_SDSTAT, Buff) == RES_OK) {
                    xputs("SD Status:\n");
                    for (s1 = 0; s1 < 64; s1 += 16)
                        put_dump(Buff + s1, s1, 16);
                }
                break;

            case 'c': /* Disk ioctl */
                switch (*ptr++) {
                case 's': /* dcs <drv> - CTRL_SYNC */
                    if (!xatoi(&ptr, &p1))
                        break;
                    xprintf("rc=%d\n", disk_ioctl((uint8_t)p1, CTRL_SYNC, 0));
                    break;
                case 'e': /* dce <drv> <start> <end> - CTRL_ERASE_SECTOR */
                    if (!xatoi(&ptr, &p1) || !xatoi(&ptr, (long*)&blk[0]) || !xatoi(&ptr, (long*)&blk[1]))
                        break;
                    xprintf("rc=%d\n", disk_ioctl((uint8_t)p1, CTRL_ERASE_SECTOR, blk));
                    break;
                }
                break;
            }
            break; // end of Disk Controls //

        case 'b': /* Buffer controls */
            switch (*ptr++) {
            case 'd': /* bd <addr> - Dump R/W buffer */
                if (!xatoi(&ptr, &p1))
                    break;
                for (ptr = (char*)&Buff[p1], ofs = p1, cnt = 32; cnt; cnt--, ptr += 16, ofs += 16)
                    put_dump((uint8_t*)ptr, ofs, 16);
                break;

            case 'r': /* br <drv> <lba> [<num>] - Read disk into R/W buffer */
                if (!xatoi(&ptr, &p1))
                    break;
                if (!xatoi(&ptr, &p2))
                    break;
                if (!xatoi(&ptr, &p3))
                    p3 = 1;
                xprintf("rc=%u\n", (uint16_t)disk_read((uint8_t)p1, Buff, p2, p3));
                break;

            case 'f': /* bf <val> - Fill working buffer */
                if (!xatoi(&ptr, &p1))
                    break;
                memset(Buff, (uint8_t)p1, sizeof(Buff));
                break;
            }
            break; // end of Buffer Controls //

        case 'f': /* FatFS API controls */
            switch (*ptr++) {

            case 'c': /* fc - Close a file */
                put_rc(f_close(&File1));
                break;

            case 'd': /* fd <len> - read and dump file from current fp */
                if (!xatoi(&ptr, &p1))
                    break;
                ofs = File1.fptr;
                while (p1) {
                    if ((uint32_t)p1 >= 16) {
                        cnt = 16;
                        p1 -= 16;
                    } else {
                        cnt = p1;
                        p1 = 0;
                    }
                    res = f_read(&File1, Buff, cnt, &cnt);
                    if (res != FR_OK) {
                        put_rc(res);
                        break;
                    }
                    if (!cnt)
                        break;

                    put_dump(Buff, ofs, cnt);
                    ofs += 16;
                }
                break;

            case 'e': /* fe - Seek file pointer */
                if (!xatoi(&ptr, &p1))
                    break;
                res = f_lseek(&File1, p1);
                put_rc(res);
                if (res == FR_OK)
                    xprintf("fptr=%lu(0x%lX)\n", File1.fptr, File1.fptr);
                break;

            case 'i': /* fi <vol> - Force initialized the logical drive */
                if (!xatoi(&ptr, &p1))
                    break;
                put_rc(f_mount((uint8_t)p1, &Fatfs[p1]));
                break;

            case 'l': /* fl [<path>] - Directory listing */
                while (*ptr == ' ')
                    ptr++;
                res = f_opendir(&Dir, ptr);
                if (res) // if res in non-zero there is an error; print the error.
                {
                    put_rc(res);
                    break;
                }
                p1 = s1 = s2 = 0; // otherwise initialize the pointers and proceed.
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
                    xprintf("%c%c%c%c%c %u/%02u/%02u %02u:%02u %9lu  %s",
                        (Finfo.fattrib & AM_DIR) ? 'D' : '-',
                        (Finfo.fattrib & AM_RDO) ? 'R' : '-',
                        (Finfo.fattrib & AM_HID) ? 'H' : '-',
                        (Finfo.fattrib & AM_SYS) ? 'S' : '-',
                        (Finfo.fattrib & AM_ARC) ? 'A' : '-',
                        (Finfo.fdate >> 9) + 1980, (Finfo.fdate >> 5) & 15, Finfo.fdate & 31,
                        (Finfo.ftime >> 11), (Finfo.ftime >> 5) & 63, Finfo.fsize, &(Finfo.fname[0]));
#if _USE_LFN
                    for (p2 = strlen(Finfo.fname); p2 < 14; p2++)
                        xputc(' ');
                    xprintf("%s\n", Lfname);
#else
                    xputc('\n');
#endif
                }
                xprintf("%4u File(s),%10lu bytes total\n%4u Dir(s)", s1, p1, s2);
                res = f_getfree(ptr, (uint32_t*)&p1, &fs);
                if (res == FR_OK)
                    xprintf(", %10lu bytes free\n", p1 * fs->csize * 512);
                else
                    put_rc(res);
                break;

            case 'o': /* fo <mode> <file> - Open a file */
                if (!xatoi(&ptr, &p1))
                    break;
                while (*ptr == ' ')
                    ptr++;
                put_rc(f_open(&File1, ptr, (uint8_t)p1));
                break;

            case 'p': /* fp <len> - read and play file from current fp */
                if (res = f_opendir(&Dir, ptr)) {
                    put_rc(res);
                    break;
                }

                p1 = s1 = s2 = 0;
                xx = 0;

                /* traverse through files and count tracks */
                while (1) {
                    if ((res = f_readdir(&Dir, &Finfo)) != FR_OK)
                        break;

                    if (Finfo.fattrib & AM_DIR) {
                        s2++;
                    } else {
                        s1++;
                        p1 += Finfo.fsize;
                    }

                    if (isWav(Finfo.fname)) {
                        sizes[xx] = Finfo.size;
                        strcpy(&names[xx], Finfo.fname);
                        x++;
                    }
                }

                while (1) {
                    if (track_changed) {
                        track_changed = 0;
                    }
                    if (done_playing) {
                        stopped = 1;
                        done_playing = 0;
                    }
                    if (stopped) {
                        state = 3;
                        while (stopped) {
                            if (previous != names_index) {
                                TestLCD();
                                previous = names_index;
                            }
                        }
                    }
                    state = 1;
                    put_rc(f_open(&File1, names[names_index], (uint8_t)1));
                    p1 = sizes[names_index];
                    printLCD();
                    ofs = File1.fptr;

                    /* fseek over *.wav header */
                    f_lseek(&File1, 44);
                    p1 -= 44;

                    /* Variable declaration */
                    int i;
                    int switch_0, switch_1;
                    int speed_conversion = REGULAR_SPEED;
                    int mono = 0;
                    uint32_t remaining_bytes_in_buff;

                    switch_0 = IORD(SWITCH_PIO_BASE, 0) & 0x1;
                    switch_1 = IORD(SWITCH_PIO_BASE, 0) & 0x2;

                    if (switch_0 && switch_1) {
                        /* stereo and normal speed */
                        printf("Normal Speed -- STEREO\n");
                        speed_conversion = REGULAR_SPEED;
                        mono = 0;
                    }
                    if (!switch_0 && switch_1) {
                        /* stereo and half speed */
                        printf("Half Speed -- STEREO\n");
                        speed_conversion = HALF_SPEED;
                        mono = 0;
                    }
                    if (switch_0 && !switch_1) {
                        /* stereo and double speed */
                        printf("Double Speed -- STEREO\n");
                        speed_conversion = DOUBLE_SPEED;
                        mono = 0;
                    }
                    if (!switch_0 && !switch_1) {
                        /* mono and normal speed */
                        printf("Normal Speed -- MONO\n");
                        speed_conversion = REGULAR_SPEED;
                        mono = 1;
                    }

                    while (p1 > 0) {
                        if ((uint32_t)p1 >= blen) {
                            cnt = blen;
                            p1 -= blen;
                        } else {
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

                            if (min_bytes_to_write > remaining_bytes_in_buff) {
                                min_bytes_to_write = remaining_bytes_in_buff;
                            }

                            /* Get the position of the next byte to be written to the FIFO in the buffer */
                            uint32_t start_position_buff = s2 - remaining_bytes_in_buff;

                            /* Loop from the next byte to be written to in the buffer to the minimum number of bytes available */
                            for (int i = start_position_buff; i < start_position_buff + min_bytes_to_write; i += speed_conversion) {
                                /* Copy buffer data to right buffer and left buffer */
                                memcpy(&l_buf, Buff + i, 2);
                                memcpy(&r_buf, Buff + i + 2, 2);

                                /**
                                 * if mono, then write `l_buf` to both right and left FIFO'Ss
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
                }

                xprintf("done\n");
                break;
            case 'r': /* fr <len> - read file */
                if (!xatoi(&ptr, &p1))
                    break;
                p2 = 0;
                Timer = 0;
                while (p1) {
                    if ((uint32_t)p1 >= blen) {
                        cnt = blen;
                        p1 -= blen;
                    } else {
                        cnt = p1;
                        p1 = 0;
                    }
                    res = f_read(&File1, Buff, cnt, &s2);
                    if (res != FR_OK) {
                        put_rc(res); // output a read error if a read error occurs
                        break;
                    }
                    p2 += s2; // increment p2 by the s2 referenced value
                    if (cnt != s2) // error if cnt does not equal s2 referenced value ???
                        break;
                }
                xprintf("%lu bytes read with %lu kB/sec.\n", p2, Timer ? (p2 / Timer) : 0);
                break;

            case 's': /* fs [<path>] - Show volume status */
                res = f_getfree(ptr, (uint32_t*)&p2, &fs);
                if (res) {
                    put_rc(res);
                    break;
                }
                xprintf("FAT type = FAT%u\nBytes/Cluster = %lu\nNumber of FATs = %u\n"
                        "Root DIR entries = %u\nSectors/FAT = %lu\nNumber of clusters = %lu\n"
                        "FAT start (lba) = %lu\nDIR start (lba,clustor) = %lu\nData start (lba) = %lu\n\n...",
                    ft[fs->fs_type & 3], (uint32_t)fs->csize * 512, fs->n_fats,
                    fs->n_rootdir, fs->fsize, (uint32_t)fs->n_fatent - 2, fs->fatbase, fs->dirbase, fs->database);
                acc_size = acc_files = acc_dirs = 0;
                res = scan_files(ptr);
                if (res) {
                    put_rc(res);
                    break;
                }
                xprintf("\r%u files, %lu bytes.\n%u folders.\n"
                        "%lu KB total disk space.\n%lu KB available.\n",
                    acc_files, acc_size, acc_dirs, (fs->n_fatent - 2) * (fs->csize / 2), p2 * (fs->csize / 2));
                break;

            case 'z': /* fz [<rw size>] - Change R/W length for fr/fw/fx command */
                if (xatoi(&ptr, &p1) && p1 >= 1 && p1 <= sizeof(Buff))
                    blen = p1;
                xprintf("blen=%u\n", blen);
                break;
            }
            break; // end of FatFS API controls //

        case 'h':
            display_help();
            break;
        }
    }

    /*
     * This return here make no sense.
     * But to prevent the compiler warning:
     * "return type of 'main' is not 'int'
     * we use an int as return :-)
     */
    return (0);
}

static void printLCD()
{
    FILE* lcd = fopen("/dev/lcd_display", "w");
    if (lcd = NULL) {
        return;
    }

    char text[16];

    for (int i = 0; i < 13; i++) {
        text[i] = names[names_index][i];
    }

    /* clear the LCD */
    fprintf(lcd, "%c%s", ESC, LCD_CLEAR);

    fprintf(lcd, "%i %s\n", names_index, text);
    fprintf(lcd, "FORTNITE");
}

int isWav(char* filename)
{
    int x = (int)strlen(filename);
    if (filename[x - 4] == '.' && filename[x - 3] == 'W' && filename[x - 2] == 'A' && filename[x - 1] == 'V')
        return 1;
    else {
        return 0;
    }
}

static void button_ISR(void* context, alt_32 id)
{
    IOWR(BUTTON_PIO_BASE, 3, 0);
    IOWR(BUTTON_PIO_BASE, 2, 0);
    IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
}

/* FIX THIS !!!!! FUBAR */
static void timer_ISR(void* context, alt_32 id)
{
    int current_value = IORD(BUTTON_PIO_BASE, 0);
    IOWR(LED_PIO_BASE, 0, 0x1);
    if (button == 0 && current_value != 0xF) {
        button = 1;
        if (current_value == 0xE) {
            track_changed = 1;
            if (names_index == 13)
                names_index = 0;
            else
                names_index++;
        }
        if (current_value == 0x7) {
            track_changed = 1;
            if (names_index == 0)
                names_index = 13;
            else
                names_index--;
        }
        if (current_value == 0xD) {
            if (paused)
                paused = 0;
            else
                paused = 1;
        }
        if (current_value == 0xB) {
            if (stopped)
                stopped = 0;
            else
                stopped = 1;
            if (paused)
                paused = 0;
        }
    }
    if (current_value == 0xF) {
        count_released++;
        if (count_released < 20)
            IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
    } else {
        count_released = 0;
        IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
    }
    if (count_released == 20) {

        IOWR(LED_PIO_BASE, 0, 0x0);
        count_released = 0;
        button = 0;
        IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x8);
        IOWR(BUTTON_PIO_BASE, 2, 0xF);
    }
}
