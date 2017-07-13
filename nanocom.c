// Copyright 2013 by TiVo Inc. All Rights Reserved
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

#define console 0       // console device is stdin (the output can be tee'd) 

// die() requires this
struct termios cooked;

int leftmost=3;         // cursor is cr'd and lf'd
int timestamp=0;        // show timestamp 
int keylock=0;          // lock keyboard
int bsisdel=0;          // send del for bs (or del)
int enterislf=0;        // send lf for enter (or cr or lf)
int showhex=0;          // show chars as hex
int tee=-1;             // tee file descriptor, if >=0

#define lf 10
#define cr 13
#define fs 28           // aka "^\", escape char, different from telnet's 

// die with a message
static void die(char *fmt,...)
{
    va_list ap;
    
    tcsetattr(console,TCSANOW,&cooked);                 // restore original settings
    va_start(ap, fmt);
    if (leftmost != 3) fprintf(stderr, "\n");
    vfprintf(stderr, fmt, ap);
    exit(0);
}

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

static void usage(void)
{
    die("Usage:\n"
        "   nanocom [-t] [-k] [-s] [-l] [-u] [-n] [-f filename] serial_device\n"
        "where:\n"
        "   -t   enable timestamps (use twice to enable dates)\n"
        "   -k   enable keylock\n"
        "   -s   BS key sends DEL\n" 
        "   -l   ENTER key sends LF\n" 
        "   -x   show unprintable chars as hex (use twice to show all chars as hex)\n"
        "   -n   use existing stty config, do not force 115200 N-8-1\n"
        "   -f   tee serial output to specified file\n"
       );
}       
    
// maybe write a timestamp    
static void stamp(void)
{
    if (timestamp)
    {
        char ts[40];
        struct timeval t;                       
        struct tm *l;
 
        gettimeofday(&t,NULL);
        l=localtime(&t.tv_sec);
        sprintf(ts+strftime(ts, sizeof(ts)-10, (timestamp==1)?"[%H:%M:%S":"[%Y/%m/%d %H:%M:%S", l),".%.3d] ", t.tv_usec/1000);
        put(console, ts, strlen(ts));
    }    
}    
    
int main(int argc, char *argv[])
{
    int native=0;                                           // default, set 115200 N81
    struct termios raw, io;
    int serial;                                             // serial port handle
    int x;
    char *device_name;
    char *teefile=NULL;
    
    tcgetattr(console,&cooked);                             // get this first so die() will work

    while (1) switch (getopt(argc,argv,":tkslxnf:"))
    {
       case 't': timestamp++; break;
       case 'k': keylock=1; break;
       case 's': bsisdel=1; break;
       case 'l': enterislf=1; break;
       case 'x': showhex++; break;
       case 'n': native=1; break; 
       case 'f': teefile=optarg; break;
            
       case ':':            // missing 
       case '?': usage();   // or invalid options
       case -1: goto optx;  // no more options
    } optx:             
   
    if (optind >= argc) usage();
  
    if (teefile && (tee=open(teefile, O_CREAT|O_WRONLY|O_APPEND))<0) die("Could not open tee file '%s': %s\n", teefile, strerror(errno));

    // hunt for device, /dev/device, or /dev/ttydevice
    device_name=malloc(strlen(argv[optind]+10));
    for (x=0; x <= 2; x++)
    {
        char *path[] = {"%s","/dev/%s", "/dev/tty%s"};
        sprintf(device_name, path[x], argv[optind]);
        serial = open(device_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial > 0) goto opened;
    }    
    die ("Could not open '%s'\n", argv[optind]);
    opened:        
    fprintf(stderr, "nanocom connected to %s, escape character is '^\\'.\n", device_name);
  
    // setup raw console
    memset(&raw,sizeof(raw),0);
    raw.c_cflag=cooked.c_cflag;                             
    raw.c_iflag=cooked.c_iflag;
    raw.c_oflag = 0;
    raw.c_lflag = 0;
    raw.c_cc[VMIN]=1;                                       
    raw.c_cc[VTIME]=0;  
    tcsetattr(console,TCSANOW,&raw);                        
    
    // setup serial port
    tcgetattr(serial, &io);
    io.c_oflag = 0;
    io.c_lflag = 0;
    io.c_cc[VMIN]=1;
    io.c_cc[VTIME]=0;   
    if (!native)
    {
        // force 115200 N81
        io.c_cflag = B115200 | CS8 | CLOCAL | CREAD;
        io.c_iflag = IGNPAR;
    }
    tcsetattr(serial, TCSANOW, &io);
    
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
                      
                        tcsetattr(console,TCSANOW,&cooked);        
                        if (leftmost != 3) printf("\n");
                        cmd:
                        printf("nanocom %s> ",device_name);
                        fgets(p=s,sizeof(s), stdin);
                        while(isspace(*p)) p++;
                        switch(*p)
                        {
                             case 0: break; 
                             case 'q': die("Connection closed.\n");
                             case 't': 
                                timestamp++; if (timestamp > 2) timestamp=0; // handle extra -t args
                                printf("Timestamps are %s.\n",(timestamp == 1)?"on":(timestamp == 2)?"on, with date":"off");
                                break;
                             case 'k': 
                                keylock ^= 1; 
                                printf("Keylock is %s.\n",keylock?"on":"off");
                                break;
                             case 's':
                                bsisdel ^= 1; 
                                printf("Backspace key sends %s.\n", bsisdel?"DEL":"BS");
                                break;
                             case 'l':
                                enterislf ^= 1; 
                                printf("Enter key sends %s.\n", enterislf?"LF":"CR");
                                break;
                             case 'x':
                                showhex++; if (showhex > 2) showhex=0; // handle extra -x args
                                printf("%s characters are shown as hex.\n", (showhex == 1)?"Unprintable":(showhex == 2)?"All":"No");
                                break;
                             case 'e':
                                put(serial, (char []){fs}, 1);
                                break;

                             case 'h':   
                             case '?': 
                                printf("Commands:\n"
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
                                printf("Invalid (try '?')\n");
                                goto cmd;
                        }        
                        tcsetattr(console,TCSANOW,&raw);        
                        leftmost=3;
                        continue;
                    }    
                } 
                if (!keylock) put(serial, &c, 1);
            }
        }    
                
        if (FD_ISSET(serial, &fds))                         // from serial
        {
            unsigned char c; int n;
            n=get(serial, (char *)&c, 1);
            if (n<=0) die("Connection error.\n");           // urk, device has disappeared?
            if (tee >=0) put(tee, (char *)&c, 1);           // send raw data to the tee
            if (showhex > 1)                                // show all chars as hex?
            {   
                leftmost=0;
                goto hex;
            }    
            if (c == cr) leftmost |= 1;                     // any cr, set the cr flag
            else if (c == lf)
            {
                if (leftmost != 3) leftmost |= 2;           // set the lf flag if not already crlf 
                else
                {
                    stamp();                                // otherwise push a dummy timestamp
                    put(console, (char []){cr}, 1);         // and crlf
                }    
            } else 
            {
                // any other 
                if (leftmost == 1)                          // avoid orphan cr's 
                    put(console, (char []){lf}, 1);            
                if (leftmost & 1) stamp();                  // always stamp from start of line 
                leftmost=0;
                
                if (showhex && (c > '~' || (c < ' ' && c != 10 && c != 13)))
                {
                    char s[8];
                  hex:
                    sprintf(s, "[%.2X]", c);
                    put(console, s, 4);
                    continue;
                }    
            }    
            put(console, (char *)&c, 1);                            
        }    
    }
    return 0;
}
