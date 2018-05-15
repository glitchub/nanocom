// Nanocom serial terminal

// This is free and unencumbered software released into the public domain.
//
// Anyone is free to copy, modify, publish, use, compile, sell, or distribute
// this software, either in source code form or as a compiled binary, for any
// purpose, commercial or non-commercial, and by any means.
//
// In jurisdictions that recognize copyright laws, the author or authors of
// this software dedicate any and all copyright interest in the software to the
// public domain. We make this dedication for the benefit of the public at
// large and to the detriment of our heirs and successors. We intend this
// dedication to be an overt act of relinquishment in perpetuity of all present
// and future rights to this software under copyright law.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// For more information, please refer to <http://unlicense.org/>

#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

// magic characters
#define lf 10
#define cr 13
#define fs 28           // aka "^\", escape char, different from telnet's

#define console 0       // console device
struct termios term;    // original console state
int cursor=3;           // cursor state, bit 0 == cr'd, bit 1 == lf'd

// write to stderr, expects console in cooked mode!
#define warn(...) fprintf(stderr, __VA_ARGS__)

// put console in raw mode
static void raw(void)
{
    struct termios t;
    memset(&t,0,sizeof(t));
    t.c_cflag=term.c_cflag;
    t.c_iflag=term.c_iflag;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cc[VMIN]=1;
    t.c_cc[VTIME]=0;
    tcsetattr(console,TCSANOW,&t);
    cursor=3; // probably true
}

// restore console to original cooked mode
static void cooked()
{
    tcsetattr(console,TCSANOW,&term);
    if (cursor != 3) warn("\n");
    cursor=3;
}

// restore console and die
#define die(...) cooked(), warn(__VA_ARGS__), exit(1)

#define usage() die("\
Usage:\n\
\n\
    nanocom [options] serial-device\n\
\n\
where:\n\
\n\
    -f file - tee received serial data to specified file\n\
    -k      - enable keylock\n\
    -l      - ENTER key sends LF \n\
    -n      - use existing stty config, do not force 115200 N-8-1\n\
    -r      - try to reconnect to serial device if it returns an error\n\
    -s      - BS key sends DEL \n\
    -t      - enable timestamps (use twice to enable dates)\n\
    -x      - show unprintable chars as hex (use twice to show all chars as hex)\n\
\n\
Will attempt to resolve a partial device name such as 'ttyS0' or 'USB0' to an\n\
appropriate serial device such as '/dev/ttyS0' or '/dev/tty.USB0'.\n\
")

// get character from file descriptor
static int get(int fd, char *s, size_t n)
{
    int x;
    while ((x=read(fd, s, n)) < 0 && errno == EINTR);
    return x;
}

// put string to file descriptor
static void put(int fd, char *s, size_t n)
{
    while((write(fd, s, n) < 0) && (errno == EAGAIN || errno == EINTR));
}

// write a timestamp if mode==1, with date if mode==2
static void stamp(int mode)
{
    if (mode)
    {
        char ts[40];
        struct timeval t;
        struct tm *l;

        gettimeofday(&t,NULL);
        l=localtime(&t.tv_sec);
        sprintf(ts+strftime(ts, sizeof(ts)-10, (mode==1)?"[%H:%M:%S":"[%Y/%m/%d %H:%M:%S", l),".%.3d] ", (int)t.tv_usec/1000);
        put(console, ts, strlen(ts));
    }
}

// open given serial device and configure it
int open_serial(char *device, int native)
{
    struct termios io;

    int f = open(device, O_RDWR|O_NOCTTY|O_NONBLOCK);
    if (f <= 0) return f;

    // setup serial port
    tcgetattr(f, &io);
    io.c_oflag = 0;
    io.c_lflag = 0;
    io.c_cc[VMIN]=1;
    io.c_cc[VTIME]=0;
    if (!native)
    {
        // force 115200 N81
        io.c_cflag = CS8 | CLOCAL | CREAD;
        io.c_iflag = IGNPAR;
        cfsetspeed(&io, 115200);
    }
    if (tcsetattr(f, TCSANOW, &io)) die ("Unable to configure %s: %s\n", device, strerror(errno));
    return f;
}

int main(int argc, char *argv[])
{
    int serial;             // serial port handle
    char *device_name;      // serial device name
    char *teefile=NULL;     // tee file name
    int tee=-1;             // tee file descriptor, if >=0
    int keylock=0;          // lock keyboard
    int reconnect=0;        // reconnect serial after failure
    int showhex=0;          // show chars as hex
    int enterislf=0;        // send lf for enter
    int bsisdel=0;          // send del for bs
    int native=0;           // default, set 115200 N81
    int timestamp=0;        // show timestamp

    tcgetattr(console,&term);                               // get this first so die() will work

    while (1) switch (getopt(argc,argv,":f:klnrstx"))
    {
        case 'f': teefile=optarg; break;
        case 'k': keylock^=1; break;
        case 'l': enterislf^=1; break;
        case 'n': native=1; break;
        case 'r': reconnect=1; break;
        case 's': bsisdel^=1; break;
        case 't': timestamp++; break;
        case 'x': showhex=(showhex+1)%3; break;

        case ':':            // missing
        case '?': usage();   // or invalid options
        case -1: goto optx;  // no more options
    } optx:

    if (optind >= argc) usage();

    if (teefile && (tee=open(teefile, O_CREAT|O_WRONLY|O_APPEND))<0) die("Could not open tee file '%s': %s\n", teefile, strerror(errno));

    // hunt for device
    device_name=malloc(strlen(argv[optind]+32));
    char *paths[] = {"%s","/dev/%s", "/dev/cu%s", "/dev/tty%s", "/dev/cu.%s", "/dev/tty.%s", NULL };
    for (char**p=paths;;p++)
    {
        if (!*p) die("Could not open '%s'\n", argv[optind]);
        sprintf(device_name, *p, argv[optind]);
        serial = open_serial(device_name, native);
        if (serial > 0) break;
    }
    warn("nanocom connected to %s, escape character is '^\\'.\n", device_name);

    raw();                                                  // console in raw mode
    while(1)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(console, &fds);
        FD_SET(serial, &fds);
        select(serial+1, &fds, NULL, NULL, NULL);           // wait for received character

        if (FD_ISSET(console, &fds))                        // from console?
        {
            char c;
            if (get(console, &c, 1) == 1)
            {
                switch(c)
                {
                    case 8:
                        if (bsisdel) c = 127;
                        break;

                    case 127:
                        if (!bsisdel) c=8;
                        break;

                    case 10:
                        if (!enterislf) c = 13;
                        break;

                    case 13:
                        if (enterislf) c = 10;
                        break;

                    case fs:
                    {
                        char s[80], *p;
                        cooked();
                        cmd:
                        warn("nanocom %s> ",device_name);
                        fgets(p=s,sizeof(s), stdin);
                        while(isspace(*p)) p++;
                        switch(*p)
                        {
                             case 0: break;
                             case 'q': exit(0); // normal exit
                             case 't':
                                timestamp++; if (timestamp > 2) timestamp=0; // handle extra -t args
                                warn("Timestamps are %s.\n",(timestamp == 1)?"on":(timestamp == 2)?"on, with date":"off");
                                break;
                             case 'k':
                                keylock ^= 1;
                                warn("Keylock is %s.\n",keylock?"on":"off");
                                break;
                             case 's':
                                bsisdel ^= 1;
                                warn("Backspace key sends %s.\n", bsisdel?"DEL":"BS");
                                break;
                             case 'l':
                                enterislf ^= 1;
                                warn("Enter key sends %s.\n", enterislf?"LF":"CR");
                                break;
                             case 'x':
                                showhex = (showhex+1) % 3;
                                warn("%s characters are shown as hex.\n", (showhex==1)?"Unprintable":(showhex==2)?"All":"No");
                                break;
                             case 'e':
                                put(serial, (char []){fs}, 1);
                                break;

                             case 'h':
                             case '?':
                                warn("Commands:\n"
                                       "  t        cycle timestamps\n"
                                       "  k        toggle keylock\n"
                                       "  s        toggle BS key sends DEL\n"
                                       "  l        toggle ENTER key sends LF\n"
                                       "  x        cycle hex display\n"
                                       "  q        quit\n"
                                       "  e        send escape (^\\)\n"
                                       "  ?        print help information\n");

                                goto cmd;
                             default:
                                warn("Invalid (try '?')\n");
                                goto cmd;
                        }
                        raw();
                        goto conx;
                    }
                }
                if (!keylock) put(serial, &c, 1);
            }
        }
        conx:    

        if (FD_ISSET(serial, &fds))                         // from serial
        {
            unsigned char c; int n;
            n=get(serial, (char *)&c, 1);
            if (n <= 0)
            {
                // urk, device has disappeared?
                if (!reconnect) die("Connection error.\n");
                cooked();
                warn("Connection error.\n");
                close(serial);
                while(1)
                {
                    warn("Reconnecting...\n");
                    serial=open_serial(device_name, native);
                    if (serial > 0) break;
                    sleep(1);
                }
                warn("nanocom reconnected to %s, escape character is '^\\'.\n", device_name);
                raw();
                goto serx;
            }
            if (tee >=0) put(tee, (char *)&c, 1);           // send raw data to the tee
            if (showhex > 1)                                // show all chars as hex?
            {
                cursor=0;
                goto hex;
            }
            if (c == cr) cursor |= 1;                       // any cr, set the cr flag
            else if (c == lf)
            {
                if (cursor != 3) cursor |= 2;               // set the lf flag if not already crlf
                else
                {
                    stamp(timestamp);                       // otherwise push a dummy timestamp
                    put(console, (char []){cr}, 1);         // and crlf
                }
            } else
            {
                // any other
                if (cursor == 1)                            // avoid orphan cr's
                    put(console, (char []){lf}, 1);
                if (cursor & 1) stamp(timestamp);           // always stamp from start of line
                cursor=0;

                if (showhex && (c > '~' || (c < ' ' && c != 10 && c != 13)))
                {
                    char s[8];
                  hex:
                    sprintf(s, "[%.2X]", c);
                    put(console, s, 4);
                    goto serx;
                }
            }
            put(console, (char *)&c, 1);
        }
        serx:;
    }
    return 0;
}
