// Nanocom serial/tcp terminal

#define usage "\
Usage:\n\
\n\
    nanocom [options] /dev/ttyX|host:port\n\
\n\
Connect tty to specified target, which is either a serial device (if target\n\
contains a '/') or a TCP host (if target contains a ':').\n\
\n\
Options:\n\
\n\
    -b       - backspace keys sends DEL instead of BS\n\
    -c       - target sends CP437 characters (IBM line draw)\n\
    -d       - toggle serial port DTR high on start\n\
    -e       - enter key sends LF instead of CR\n\
    -f file  - tee received data to specified file\n\
    -n       - use existing tty config, do not force 115200 N-8-1\n\
    -r       - try to reconnect to target if it won't open or closes with error\n\
    -t       - enable timestamps (use twice to enable dates)\n\
    -x       - show unprintable chars as hex (use twice to show all chars as hex)\n\
\n\
Once connected, pressing the escape character CTRL+\\ enters command mode, supporting\n\
the following commands:\n\
\n" menu

#define menu "\
    b        - toggle backspacekey sends DEL or BS\n\
    e        - toggle enter key sends LF or CR\n\
    p        - pass escape (CTRL-\\) to target\n\
    q        - quit\n\
    t        - cycle timestamps off, time, or date+time\n\
    x        - cycle hex off, unprintable, or all\n\
    ! cmd    - execute shell 'cmd' with stdin/stdout connected to target\n\
\n\
Enter any valid command, or just enter by itself, to exit command mode.\n\
"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <iconv.h>
#include <locale.h>

// magic characters
#define BS 8
#define LF 10
#define CR 13
#define FS 28           // aka CTRL-\, our escape character
#define DEL 127

#define console 1       // console device aka stdout

struct termios orig;    // original tty state
char *targetname;       // "/dev/ttyX" or "host:port"
int clean = 1;          // 1 = cursor at start of line
int target = -1;        // target file handle or socket, if >= 0
char *teename = NULL;   // tee file name
FILE *tee = NULL;       // tee file handle
int reconnect = 0;      // 1 = reconnect after failure
int showhex = 0;        // 1 = show received unprintable as hex, 2 = show all as hex
int enterislf = 0;      // 0 = enter key sends cr, 1 = lf
int bsisdel = 0;        // 1 = send DEL for BS
int native = 0;         // 1 = don't force serial 115200 N81
int timestamp = 0;      // 1 = show time, 2 = show date and time
int dtr = 0;            // 1 = twiddle serial DTR on connect
int israw = 0;          // 1 = console in raw mode
int cp437 = 0;          // 1 = encode cp437

#define msleep(mS) usleep(mS*1000)

// restore console to cooked mode and die
#define die(...) cooked(), printf(__VA_ARGS__), exit(1)

// put console in raw mode
static void raw(void)
{
    // only if it's not already raw
    if (!israw)
    {
        struct termios t;
        memset(&t,0,sizeof(t));
        t.c_cflag = orig.c_cflag;
        t.c_iflag = orig.c_iflag;
        t.c_oflag = 0;
        t.c_lflag = 0;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(console,TCSANOW,&t);
        israw = 1;
        clean = 1; // assume cursor is at the left edge
    }
}

// put console in cooked mode
static void cooked()
{
    // only if it really is raw
    if (israw)
    {
        tcsetattr(console, TCSANOW, &orig);
        israw = 0;
        if (!clean) printf("\n"); // force next line if needed
    }
}

// return character from file descriptor or -1 if error
static int get(int fd)
{
    unsigned char c;
    int n;
    while ((n = read(fd, &c, 1)) < 0 && errno == EINTR);
    return (n == 1) ? c : -1;
}

// flush file descriptor
static void flush(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags|O_NONBLOCK);
    while (get(fd) >= 0);
    fcntl(fd, F_SETFL, flags);
}

// put character(s) to file descriptor, if size is 0 use strlen(s)
static void put(int fd, char *s, size_t n)
{
    if (!n) n = strlen(s);
    while (write(fd, s, n) < 0 && (errno == EAGAIN || errno == EINTR));
}

// put character as hex to console
static void hex(int c)
{
    char s[5];
    snprintf(s, 5, "[%.2X]", c);
    put(console, s, 0);
}

// if enabled, put current time or time/date to console and return -1. Else return 0
static int stamp(void)
{
    if (!timestamp) return 0;
    struct timeval t;
    gettimeofday(&t, NULL);
    struct tm *l = localtime(&t.tv_sec);

    char ts[40];
    sprintf(ts + strftime(ts, sizeof(ts)-10, (timestamp < 2) ? "[%H:%M:%S" : "[%Y-%m-%d %H:%M:%S", l),".%.3d] ", (int)t.tv_usec/1000);
    put(console, ts, 0);
    return -1;
}

// open targetname and set target
static void connect_target()
{
    int first = 1;

    if (target >= 0)
    {
        // target is already set!
        close(target);
        printf("nanocom reconnecting to %s...\n", targetname);
    }

    while (1)
    {
        if (strchr(targetname, '/'))
        {
            // target is a serial device
            struct termios io;

            target = open(targetname, O_RDWR|O_NOCTTY|O_NONBLOCK);
            if (target >= 0)
            {
                // make sure it's raw
                tcgetattr(target, &io);
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
                if (tcsetattr(target, TCSANOW, &io)) die ("Unable to configure %s: %s\n", targetname, strerror(errno));

                if (dtr)
                {
                    ioctl(target, TIOCMBIC, (int[]){TIOCM_DTR}); // clear
                    ioctl(target, TIOCMBIS, (int[]){TIOCM_DTR}); // then set
                    ioctl(target, TIOCMBIS, (int[]){TIOCM_DTR}); // twice?
                    msleep(50);
                    tcflush(target, TCIOFLUSH);
                }

                break;
            }
        }
        else if (strchr(targetname, ':'))
        {
            // target is host:port
            char *host = strdup(targetname);
            char *port = strchr(host, ':');
            *port++ = 0;

            struct addrinfo *ai, hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
            int res = getaddrinfo(host, port, &hints, &ai);
            if (res) die("Unable to resolve %s: %s\n", targetname, gai_strerror(res));
            target = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (connect(target, ai->ai_addr, ai->ai_addrlen)) target = -1;
            free(host);
            freeaddrinfo(ai);
            if (target >= 0) break;
        }
        else die("%s must contain '/' or ':'\n", targetname);

        // failed
        if (first) printf("Could not connect to %s: %s\n", targetname, strerror(errno));
        first=0;
        if (!reconnect) exit(1); // die

        printf("nanocom retrying %s...\n", targetname);
        sleep(1);
    }

    printf("nanocom connected to %s, escape character is 'CTRL-\\'.\n", targetname);
}

// run shell command with stdin/stdout attached or passed to target
static void run(char *cmd)
{
    while (isspace(*cmd)) cmd++;
    if (!*cmd)
    {
        printf("No command specified\n");
        return;
    }

    printf("Starting '%s'...\n", cmd);

    int pid = fork();
    if (pid < 0) die("Unable to fork: %s\n", strerror(errno));
    if (!pid)
    {
        setsid();
        flush(target);
        if (dup2(target,0) != 0 || dup2(target,1) != 1) die("Unable to dup target to stdin/stdout\n");
        close(target);
        exit(system(cmd)); // run the command
    }

    int status;
    wait(&status);
    printf("'%s' exit status %d\n", cmd, WEXITSTATUS(status));
}

// Handle CTRL-\ escape key
static void command(void)
{
    void bstat(int indent) { printf("%*sBackspace key sends %s\n", indent, "", bsisdel ? "DEL" : "BS"); }
    void estat(int indent) { printf("%*sEnter key sends %s\n", indent, "", enterislf ? "LF" : "CR"); }
    void sstat(int indent) { printf("%*sTimestamps are %s\n",indent, "", (timestamp == 0) ? "off" : (timestamp == 1) ? "on" : "on, with date"); }
    void xstat(int indent) { printf("%*s%s characters are shown as hex\n", indent, "", (showhex == 0) ? "No" : (showhex == 1) ? "Unprintable" : "All"); }

    cooked(); // switch to cooked mode
    while(1)
    {
        printf("nanocom %s> ", targetname);

        // read command line
        char s[80];
        if (!fgets(s, sizeof(s), stdin)) die("fgets failed: %s\n", strerror(errno));

        // justify
        char *p = strchr(s,'\n');
        if (p) *p = 0;
        p = s; while(isspace(*p)) p++;

        switch(*p)
        {
            case 0:     // nothing to do!
                break;

            case 'b':
                bsisdel = !bsisdel;
                bstat(0);
                break;

            case 'e':
                enterislf = !enterislf;
                estat(0);
                break;

            case 'p':
                put(target, (char []){FS}, 1);
                printf("Sent CTRL-\\ to target\n");
                break;

            case 'q':
                exit(0);

            case 't':
                timestamp++;
                if (timestamp > 2) timestamp = 0;
                sstat(0);
                break;

            case 'x':
                showhex++;
                if (showhex > 2) showhex = 0;
                xstat(0);
                break;

            case '!':
                run(p+1);
                break;

            default:
                printf("Status:\n");
                bstat(4);
                estat(4);
                sstat(4);
                xstat(4);

                printf("\nCommands:\n" menu);
                continue;
        }
        break;
    }
    raw(); // back to raw mode
}

// encoded CP437 characters 128-255, if not NULL
char *encoded[128] = {NULL};

// configure encoding and return true, or false if can't
int setencode(void)
{
    int n = 0;
    // Convert CP437 to native
    setlocale(LC_CTYPE, "");
    iconv_t cd = iconv_open("//TRANSLIT", "CP437");
    if (cd == (iconv_t)-1) goto out;
    // for each high character
    for (n = 128; n <= 255; n++)
    {
        // try to determine native sequence
        char i = n, *in = &i, buf[8], *out = buf;
        size_t nin = 1, nout = sizeof(buf);
        if (iconv(cd, &in, &nin, &out, &nout) >= 0 && nout < sizeof(buf))
        {
            // save it, XXX this will break if the sequence contains NUL
            encoded[n & 127] = strndup(buf, sizeof(buf) - nout);
        }
    }
    iconv_close(cd);
    out:
    setlocale(LC_CTYPE, "C");
    return n > 0;
}

int main(int argc, char *argv[])
{

    while (1) switch (getopt(argc,argv,":bcdef:nrtx"))
    {
        case 'b': bsisdel++; break;
        case 'c': cp437++; break;
        case 'd': dtr++; break;
        case 'e': enterislf++; break;
        case 'f': teename = optarg; break;
        case 'n': native++; break;
        case 'r': reconnect++; break;
        case 't': timestamp++; break;
        case 'x': showhex++; break;

        case ':':              // missing
        case '?': die(usage);  // or invalid options
        case -1: goto optx;    // no more options
    } optx:

    if (optind >= argc) die(usage);

    targetname = argv[optind];
    connect_target();

    if (teename && (tee = fopen(teename, "a"))) die("Could not open tee file '%s': %s\n", teename, strerror(errno));

    signal(SIGPIPE, SIG_IGN);

    if (cp437 && !setencode())                              // configure cp437 encoding
    {
        printf("Warning, can't encode CP437 on this terminal\n");
        cp437 = 0;
    }

    tcgetattr(console,&orig);                               // get original tty states
    raw();                                                  // put console in raw mode
    atexit(cooked);                                         // restore cooked on unexpected exit

    while(1)
    {
        struct pollfd p[2] = {{ .fd = console, .events = POLLIN }, { .fd = target, .events = POLLIN }};
        poll(p, 2, -1);
        if (p[0].revents) do
        {
            // Process key from console. Note we're inside a loop, break to
            // skip forwarding the key to the target.
            int c = get(console);

            if (c < 0) die("Console error: %s\n", strerror(errno));

            if (c == FS)
            {
                command();
                break;
            }

            switch(c)
            {
                case BS:
                    if (bsisdel) c = DEL;
                    break;

                case DEL:
                    if (!bsisdel) c = BS;
                    break;

                case LF:
                    if (!enterislf) c = CR;
                    break;
            }

            // send key to the target
            put(target, (char []){c}, 1);

        } while(0);

        if (p[1].revents) do
        {
            // Process character from target. Note we're inside a loop, break to
            // skip forwarding the character to the console

            // get key from target
            int c = get(target);

            if (c < 0)
            {
                // urk, target dropped
                cooked();
                printf("nanocom lost connection to %s\n", targetname);
                if (!reconnect) exit(1);
                connect_target();
                raw();
                break;
            }

            if (tee) fputc(c, tee);                         // maybe send raw data to the tee

            if (showhex > 1)
            {
                hex(c);
                clean = 0;
                break;
            }

            if (c == LF)
            {
                if (clean) stamp();                         // LF, maybe timestamp on blank line
                put(console, (char []){CR}, 1);             // inject CR
                clean = 1;                                  // we're now at the start of the line
            }
            else if (c != CR)                               // let CR through unmolested
            {                                               // else not CR or LF
                if (clean) stamp();                         // timestamp on blank line
                clean = 0;                                  // not clean anymore
                if (showhex && !isprint(c))                 // maybe show unprintable as hex
                {
                    hex(c);
                    break;
                }
                if (cp437 && c > 127 && encoded[c & 127])   // if cp437 high character
                {
                    put(console, encoded[c & 127], 0);      // put utf8 (or whatever for this console)
                    break;
                }
            }

            // send character to console
            put(console, (char []){c}, 1);

        } while(0);
    }
    return 0;
}
