// A simple serial/tcp comm program intended for use with embedded systems.
// See https://github.com/glitchub/nanocom for more information.

// This software is released as-is into the public domain, as described at
// https://unlicense.org. Do whatever you like with it.

#define usage "\
Usage:\n\
\n\
    nanocom [options] /dev/ttyX|host:port\n\
\n\
Connect current tty to specified target, which is either a serial device (if\n\
target contains a '/') or a TCP host (if target contains a ':').\n\
\n\
Options:\n\
\n\
    -b       - backspace keys sends DEL instead of BS\n\
    -c       - encode CP437 characters from target (IBM line draw)\n\
    -d       - toggle serial port DTR high on start\n\
    -e       - enter key sends LF instead of CR (use twice to send CRLF)\n\
    -f file  - tee received data to specified file\n\
    -n       - use existing tty config, don't force 115200 N-8-1\n\
    -r       - try to reconnect to target if it won't open or closes with error\n\
    -s       - enable timestamps (use twice to enable dates)\n\
    -t       - enable telnet IAC handling (use twice to disable server ECHO)\n\
    -x       - show unprintable chars as hex (use twice to show all chars as hex)\n\
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
#undef ECHO  // unwanted termios definition
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
#include <pty.h>

// magic characters
#define BS 8
#define LF 10
#define CR 13
#define FS 28                   // aka CTRL-\, our escape character
#define DEL 127

// magic telnet IAC characters, these are also used as states
#define SB 250                  // IAC SB = start suboption
#define SE 240                  // IAC SE = end suboption
#define WILL 251                // IAC WILL XX = will support option XX
#define WONT 252                // IAC WONT XX = won't perform option XX
#define DO 253                  // IAC DO XX = do perform option XX
#define DONT 254                // IAC DONT XX = don't perform option XX
#define IAC 255                 // IAC IAC = literal 0xff character

// the "X" options of interest
#define BINARY 0                // unescaped binary (we require this)
#define ECHO 1                  // server echo
#define SGA 3                   // suppress go ahead (we require this)

#define console 1               // console device aka stdout

struct termios orig;            // original tty state
char *targetname;               // "/dev/ttyX" or "host:port"
int clean = 1;                  // 1 = cursor at start of line
int target = -1;                // target file handle or socket, if >= 0
char *teename = NULL;           // tee file name
FILE *tee = NULL;               // tee file handle
int reconnect = 0;              // 1 = reconnect after failure
int showhex = 0;                // 1 = show received unprintable as hex, 2 = show all as hex
int enterkey = 0;               // 0 = enter key sends cr, 1 = lf, 2 = crlf
int bsisdel = 0;                // 1 = send DEL for BS
int native = 0;                 // 1 = don't force serial 115200 N81
int timestamp = 0;              // 1 = show time, 2 = show date and time
int dtr = 0;                    // 1 = twiddle serial DTR on connect
int israw = 0;                  // 1 = console in raw mode
int cp437 = 0;                  // 1 = encode cp437, -1 = disallow cp437
int telnet = 0;                 // 1 = support telnet IACs

#define msleep(mS) usleep(mS*1000)

// restore console to cooked mode and die
#define die(...) cooked(), printf(__VA_ARGS__), exit(1)

// put console in raw mode
void raw(void)
{
    // only if it's not already raw
    if (!israw)
    {
        struct termios t = orig;
        cfmakeraw(&t);
        tcsetattr(console, TCSANOW, &t);
        israw = 1;
        clean = 1; // assume cursor is at the left edge
    }
}

// put console in cooked mode
void cooked()
{
    // only if it really is raw
    if (israw)
    {
        tcsetattr(console, TCSANOW, &orig);
        israw = 0;
        if (!clean) printf("\n"); // force next line if needed
    }
}

// return character from file descriptor or -1 if unrecoverable error
int get(int fd)
{
    unsigned char c;
    while (1)
    {
        int n = read(fd, &c, 1);
        if (n == 1) return c;
        if (n < 0 && errno != EAGAIN && errno != EINTR) return -1;
    }
}

// put character(s) to file descriptor, if size is 0 use strlen(s), return -1
// on unrecoverable error.
int put(int fd, char *s, size_t size)
{
    if (!size) size = strlen(s);

    while (1)
    {
        int n = write(fd, s, size);
        if (n == size) return 0;
        if (n < 0 && errno != EAGAIN && errno != EINTR) return -1;
        if (n > 0)
        {
            size -= n;
            s += n;
        }
    }
}

// put character as hex to console
void hex(int c)
{
    char s[5];
    snprintf(s, 5, "[%.2X]", c);
    put(console, s, 0);
}

// if enabled, put current time or time/date to console and return -1. Else return 0
void stamp(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    struct tm *l = localtime(&t.tv_sec);

    char ts[40];
    sprintf(ts + strftime(ts, sizeof(ts)-10, (timestamp < 2) ? "[%H:%M:%S" : "[%Y-%m-%d %H:%M:%S", l),".%.3d] ", (int)t.tv_usec/1000);
    put(console, ts, 0);
}

// Given a character from target server, process telnet IAC commands, return -1 if
// character is swallowed or 0 if character can be sent to target
// If c < 0, initialize IAC state
int iac(int c)
{
    // Send various IAC messages
    void senddo(int c) { put(target, (char []) {IAC, DO, c}, 3); }
    void senddont(int c) { put(target, (char []) {IAC, DONT, c}, 3); }
    void sendwill(int c) { put(target, (char []) {IAC, WILL, c}, 3); }
    void sendwont(int c) { put(target, (char []) {IAC, WONT, c}, 3); }

    static int state = 0; // IAC state

    if (c < 0)
    {
        // Send initial requests to server, these are mandatory or telnet won't
        // work for us.
        senddo(BINARY);
        senddo(SGA);
        sendwill(BINARY);
        sendwill(SGA);
        return 0;
    }

    switch(state)
    {
        case 0:
            if (c != IAC) return 0;                 // tell caller to send it
            state = IAC;
            break;

        case IAC:
            switch(c)
            {
                case IAC:                           // escaped
                    state = 0;
                    return 0;                       // tell caller to send it

                case SB:                            // use command code as state
                case WILL:
                case WONT:
                case DO:
                case DONT:
                    state = c;
                    break;

                default:                            // ignore any other
                    state = 0;
                    break;
            }
            break;

        case WILL:
            state = 0;
            if (c == BINARY || c == SGA) break;     // ACKs to previous DO
            if (c == ECHO && telnet == 1)           // allow ECHO unless -tt
            {
                senddo(c);
                break;
            }
            // anything else, send DONT

        case WONT:
            state = 0;
            senddont(c);
            break;

         case DO:
            state = 0;
            if (c == BINARY || c == SGA) break;     // ACKs to previous WILL
            // anything else, send WONT

         case DONT:
            state = 0;
            sendwont(c);
            break;

        case SB:                                    // in suboption
            if (c == IAC) state = SE;               // just wait for IAC
            break;

        case SE:                                    // IAC in suboption
            if (c == SE) state = 0;                 // done if SE
            else state = SB;                        // else keep waiting
            break;

    }
    return -1; // character is swalloed
}

// open targetname and set target
void connect_target()
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

// run shell command in its own pty, broker stdio to target
void run(char *cmd)
{
    int child;          // child pty
    dup2(console, 99);  // dup console

    int pid = forkpty(&child, NULL, NULL, NULL);
    if (!pid)
    {
        // child
        dup2(99, 2);                    // set stderr to console
        fprintf(stderr, "Starting '%s' in %s...\n", cmd, getcwd(NULL,0));
        int status = system(cmd);       // run the command
        fprintf(stderr, "Exit status %d\n", WEXITSTATUS(status));
        exit(0);
    }

    // parent
    close(99);
    if (pid < 0)
    {
        printf("forkpty failed: %s\n", strerror(errno));
        return;
    }

    // make child raw
    struct termios t;
    tcgetattr(child, &t);
    cfmakeraw(&t);
    tcsetattr(child, TCSAFLUSH, &t);

    unsigned long long tx=0, rx=0;

    while (1)
    {
        struct pollfd p[2] = {{ .fd = child, .events = POLLIN }, { .fd = target, .events = POLLIN }};
        poll(p, 2, -1);
        if (p[0].revents)
        {
            int c = get(child);                                         // read from child
            if (c < 0) break;                                           // done if terminated
            if (telnet && c == IAC) put(target, (char []){IAC}, 1);     // escape IAC
            tx++;
            put(target, (char []){c}, 1);                               // send to target
        }
        if (p[1].revents)
        {
            int c = get(target);                                        // read from target
            if (c < 0) break;                                           // urk, connection dropped
            if (!telnet || !iac(c))                                     // if telnet doesn't swallow it
            {
                rx++;
                if (put(child, (char []){c}, 1) != 1) break;            // send to child, done if couldn't
            }
        }
    }

    close(child);
    if (!kill(-pid, SIGINT)) msleep(250);                               // maybe kill child(ren)
    waitpid(-pid, NULL, WNOHANG);
    fprintf(stderr, "Command complete, received %lld and sent %lld bytes\n", rx, tx);
}

// Handle CTRL-\ escape key
void command(void)
{
    void bstat(char *fmt) { printf(fmt, bsisdel ? "DEL" : "BS"); }
    void estat(char *fmt) { printf(fmt, (enterkey == 0) ? "LF" : (enterkey == 1) ? "CR" : "CRLF"); }
    void sstat(char *fmt) { printf(fmt, (timestamp == 0) ? "off" : (timestamp == 1) ? "on" : "on with date"); }
    void xstat(char *fmt) { printf(fmt, (showhex == 0) ? "off" : (showhex == 1) ? "on" : "all"); }
    void cstat(char *fmt) { printf(fmt, (cp437 < 0) ? "not supported" : cp437 ? "on" : "off"); }

    cooked(); // switch to cooked mode
    while(1)
    {
        printf("nanocom %s> ", targetname);

        // read command line
        char s[80];
        if (!fgets(s, sizeof(s), stdin)) die("fgets failed: %s\n", strerror(errno));

        // justify
        char *p1 = strchr(s,'\n');
        if (p1) *p1 = 0;
        p1 = s; while(isspace(*p1)) p1++;   // find first char
        if (!*p1) break;
        char *p2 = p1+1;
        while(isspace(*p2)) p2++;           // find second char
        if (*p1 == '!')                     // ! shell command
        {
            if (!*p2) goto invalid;         // only if there's a second char
            run(p2);                        // run it
            break;
        }
        if (*p2) goto invalid;              // otherwise, invalid if there's a second char
        switch(*p1)                         // find it
        {
            case 'b':
                bsisdel = !bsisdel;
                bstat("Backspace key sends %s\n");
                break;

            case 'c':
                if (cp437 >= 0) cp437 = !cp437;
                cstat("CP437 encoding is %s\n");
                break;

            case 'e':
                enterkey++;
                if (enterkey > 2) enterkey = 0;
                estat("Enter key sends %s\n");
                break;

            case 'p':
                put(target, (char []){FS}, 1);
                printf("Sent CTRL-\\ to target\n");
                break;

            case 'q':
                exit(0);

            case 's':
                timestamp++;
                if (timestamp > 2) timestamp = 0;
                sstat("Timestamps are %s\n");
                break;

            case 'x':
                showhex++;
                if (showhex > 2) showhex = 0;
                xstat("Hex output is %s\n");
                break;

            default:
            invalid:
                printf("Invalid command: %s\n\n", p1);
                // fall thru

            case '?':
                printf("Commands:\n\n");
                bstat( "    b     - toggle backspace key sends DEL or BS (now %s)\n");
                cstat( "    c     - toggle CP437 encoding (now %s)\n");
                estat( "    e     - cycle enter key sends LF, CR, or CRLF (now %s)\n");
                printf("    p     - send CTRL-\\ to target\n");
                printf("    q     - quit\n");
                sstat( "    s     - cycle timestamps off, on, or on with date (now %s)\n");
                xstat( "    x     - cycle hex output off, on, or all (now %s)\n");
                printf("    ! cmd - execute shell 'cmd' with stdin/stdout connected to target\n");
                printf("\n^Z, ^C, etc do the usual things.\n");
                printf("Any valid command, or enter by itself, exits command mode.\n");
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

    while (1) switch (getopt(argc,argv,":bcdef:nrstx"))
    {
        case 'b': bsisdel++; break;
        case 'c': cp437++; break;
        case 'd': dtr++; break;
        case 'e': enterkey++; break;
        case 'f': teename = optarg; break;
        case 'n': native++; break;
        case 'r': reconnect++; break;
        case 's': timestamp++; break;
        case 't': telnet++; break;
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

    if (telnet) iac(-1);                                        // init IAC

    if (!setencode())                                           // configure cp437 encoding
    {
        if (cp437) printf("Warning, can't encode CP437 for current locale\n");
        cp437 = -1;
    }

    tcgetattr(console,&orig);                                   // get original tty states
    raw();                                                      // put console in raw mode
    atexit(cooked);                                             // restore cooked on unexpected exit

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
                    // enterkey 0=CR, 1=LF, 2=CRLF
                    if (!enterkey)
                        c = CR;
                    else if (enterkey > 1)
                        put(target, (char []){CR}, 1);
                    break;

                case IAC:
                    if (telnet) put(target, (char []){IAC}, 1); // escape 255's if telnet
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

            if (telnet && iac(c)) break;                        // done if key swallowed by telnet IAC

            if (tee) fputc(c, tee);                             // maybe send raw data to the tee

            if (showhex > 1)                                    // show all as hex?
            {
                hex(c);
                clean = 0;
                break;
            }

            if (c == LF)
            {
                if (clean && timestamp) stamp();                // LF, maybe timestamp on blank line
                put(console, (char []){CR}, 1);                 // inject CR
                clean = 1;                                      // we're now at the start of the line
            }
            else if (c != CR)                                   // let CR through unmolested
            {                                                   // else not CR or LF
                if (clean && timestamp) stamp();                // timestamp on blank line
                clean = 0;                                      // not clean anymore

                if (showhex && !isprint(c))                     // maybe show unprintable as hex
                {
                    hex(c);
                    break;
                }

                if (cp437 > 0 && c > 127 && encoded[c & 127])   // if cp437 high character
                {
                    put(console, encoded[c & 127], 0);          // put utf8(?) string
                    break;
                }
            }

            // send character to console
            put(console, (char []){c}, 1);

        } while(0);
    }
    return 0;
}
