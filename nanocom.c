// Nanocom serial terminal
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
#include <sys/wait.h>

// magic characters
#define lf 10
#define cr 13
#define fs 28           // aka "^\", escape char, different from telnet's

#define console 2       // console device

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
    nanocom [options] serial_device\n\
\n\
where:\n\
\n\
    -f file - tee received serial data to specified file\n\
    -k      - enable keylock\n\
    -l      - ENTER key sends LF \n\
    -n      - use existing stty config, do not force 115200 N-8-1\n\
    -r      - try to reconnect to serial device if it won't open or closes with error\n\
    -s      - BS key sends DEL \n\
    -t      - enable timestamps (use twice to enable dates)\n\
    -x      - show unprintable chars as hex (use twice to show all chars as hex)\n\
")

// get character from file descriptor
static int get(int fd, char *s)
{
    int x;
    while ((x=read(fd, s, 1)) < 0 && errno == EINTR);
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
        cfsetspeed(&io, B115200);
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

    device_name=argv[optind];
    serial = open_serial(device_name, native);
    if (serial <= 0)
    {
        if (!reconnect) die("Could not open '%s': %s\n", device_name, strerror(errno));
        warn("Could not open %s: %s\n", device_name, strerror(errno));
        while (serial <= 0)
        {
            warn("Retrying '%s'...\n", device_name);
            sleep(1);
            serial = open_serial(device_name, native);
        }
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
            int n=get(console, &c);
            if (n < 0) die("Console connection error: %s\n", strerror(errno));
            if (n)
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
                        if (!fgets(s,sizeof(s), stdin)) die("fgets failed: %s\n", strerror(errno));
                        p=strchr(s,'\n'); if (p) *p=0;
                        p=s; while(isspace(*p)) p++;
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
                             case '!':
                             {
                                int pid, status;
                                p++; while (isspace(*p)) p++;
                                if (!*p) { warn("No command specified!\n"); break; }
                                warn("Starting '%s'...\n", p);
                                pid=fork();
                                if (pid < 0) die("Fork failed: %s\n", strerror(errno));
                                if (!pid)
                                {
                                    fcntl(serial, F_SETFL, O_RDWR|O_NOCTTY); // blocking
                                    if (dup2(serial,0) != 0 || dup2(serial,1) != 1) die("Unable to dup serial to stdin/stdout\n");
                                    exit(system(p));
                                }
                                wait(&status);
                                warn("'%s' exited with status %d\n", p, status);
                             }
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
                                       "  ! cmd    execute 'cmd' with serial stdin/stdout\n" 
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
            n=get(serial, (char *)&c);
            if (n <= 0)
            {
                // urk, device has disappeared?
                if (!reconnect) die("Serial connection error\n");
                cooked();
                warn("Serial connection error\n");
                close(serial);
                while(1)
                {
                    warn("Reconnecting '%s'...\n", device_name);
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
