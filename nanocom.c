// A simple serial/tcp comm program intended for use with (and on) embedded
// systems. See https://github.com/glitchub/nanocom for more information.

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
    -b          - backspace key sends DEL instead of BS\n\
    -d          - toggle serial port DTR high on start\n\
    -e          - enter key sends LF instead of CR, 2X to send CR+LF, 3X to send CR+NUL\n\
    -f file     - tee received data to specified file\n\
    -i encoding - encoding for high characters, empty string to pass verbatim ('iconv -l' for list)\n\
    -n          - don't set serial port to 115200 N-8-1, use it as is\n\
    -r          - try to reconnect target if it won't open or closes with error\n\
    -s          - enable timestamps, 2X to also show date\n\
    -t          - enable telnet IAC handling, 2X to disable BINARY and default enter to CR+NUL\n\
    -x          - show unprintable chars as hex, 2X to show all chars as hex\n\
\n\
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

// derived states
#define SBX 0x100               // waiting for end of suboption
#define SBTT 0x200              // waiting for TTYPE suboption command

// the "X" options of interest
#define BINARY 0                // unescaped binary
#define SECHO 1                 // server echo
#define SGA 3                   // suppress go ahead (we require this)
#define TTYPE 24                // terminal type, required for binary
#define NAWS 31                 // negotiate about window size

#define console 1               // console device aka stdout

char *targetname;               // "/dev/ttyX" or "host:port"
int clean = 1;                  // 1 = cursor at start of line
int target = -1;                // target file handle or socket, if >= 0
FILE *tee = NULL;               // tee file handle

// options
char *teename = NULL;           // tee file name
int reconnect = 0;              // 1 = reconnect after failure
int showhex = 0;                // 1 = show received unprintable as hex, 2 = show all as hex
int enterkey = 0;               // 0 = enter key sends cr, 1 = lf, 2 = crlf
int bskey = 0;                  // 1 = send DEL for BS
int native = 0;                 // 1 = don't force serial 115200 N81
int timestamp = 0;              // 1 = show time, 2 = show date and time
int dtr = 0;                    // 1 = twiddle serial DTR on connect
char *encoding = NULL;          // encoding name (see iconv -l)
int telnet = 0;                 // 1 = support telnet IACs

#define msleep(mS) usleep(mS*1000)

// restore console to cooked mode and die
#define die(...) cooked(), printf(__VA_ARGS__), exit(1)

#define RAW 0
#define WARM 1
#define COOKED 2
void ctty(int mode);
void cooked(void) { ctty(COOKED); }

// change console mode to RAW, WARM, or COOKED
void ctty(int mode)
{
    static struct termios orig;
    static int current = -1;
    if (mode != current)
    {
        if (current < 0)
        {
            // first time init
            tcgetattr(console, &orig);
            atexit(cooked); // restore cooked on unpexected exit
        }

        switch(mode)
        {
            case COOKED:
                tcsetattr(console, TCSANOW, &orig);
                if (!clean) printf("\n");
                break;

            case RAW:
            {
                struct termios t = orig;
                t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK);
                t.c_lflag &= 0;
                clean = 1;
                tcsetattr(console, TCSANOW, &t);
                break;
            }

            case WARM:
            {
                // like cooked but without ISIG
                struct termios t = orig;
                t.c_lflag &= ~ISIG;
                tcsetattr(console, TCSANOW, &t);
                if (!clean) printf("\n");
                break;
            }
        }
        current = mode;
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
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        return -1;
    }
}

// put character(s) to file descriptor, if size is 0 use strlen(s). Return -1
// on unrecoverable error.
#define bytes(...) (char *)(unsigned char []){__VA_ARGS__}
int put(int fd, char *s, size_t size)
{
    if (!size) size = strlen(s);

    while (1)
    {
        int n = write(fd, s, size);
        if (n == size) return 0;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
        if (n > 0)
        {
            size -= n;
            s += n;
        }
    }
}

// Put line ending to target as specified by enterkey, return -1 irf error.
// Note telnet client-to-server may require CR+LF or CR+NUL
int enter()
{
    switch (enterkey)
    {
        case 0:  return put(target, bytes(CR), 1);
        case 1:  return put(target, bytes(LF), 1);
        case 2:  return put(target, bytes(CR, LF), 2);
        default: return put(target, bytes(CR, 0), 2);
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
void stamp(int withdate)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    struct tm *l = localtime(&t.tv_sec);

    char ts[40];
    sprintf(ts + strftime(ts, sizeof(ts)-10, withdate ?  "[%Y-%m-%d %H:%M:%S": "[%H:%M:%S", l),".%.3d] ", (int)t.tv_usec/1000);
    put(console, ts, 0);
}

// Given a character from target server, process telnet IAC commands, return -1
// if character is swallowed or 0 if character can be sent to target.
int iac(int c)
{
    // Send various IAC messages
    void senddo(int c) { put(target, bytes(IAC, DO, c), 3); }
    void senddont(int c) { put(target, bytes(IAC, DONT, c), 3); }
    void sendwill(int c) { put(target, bytes(IAC, WILL, c), 3); }
    void sendwont(int c) { put(target, bytes(IAC, WONT, c), 3); }

    static int state = 0;   // IAC state
    static int isinit = 0;  // true if we've sent our initial IAC requests

    if (c < 0)
    {
        state = 0;
        isinit = 0;
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
            if (!isinit)
            {
                // Send our initial requests to server.
                senddo(SGA);
                sendwill(SGA);
                sendwill(TTYPE);
                senddo(SECHO);
                if (telnet == 1)
                {
                    // binary unless started with -tt
                    senddo(BINARY);
                    sendwill(BINARY);
                }
                isinit = 1;
            }
            break;

        case WILL:
            state = 0;
            switch(c)
            {
                case BINARY:
                    if (telnet > 1) senddont(BINARY);
                    break;

                case SGA:
                case SECHO:
                    break;

                default:
                    senddont(c);
                    break;
            }
            break;

         case DO:
            state = 0;
            switch(c)
            {
                case BINARY:
                    if (telnet > 1) sendwont(BINARY);
                    break;

                case SGA:
                case TTYPE:
                    break;

                default:
                    sendwont(c);
                    break;
            }
            break;

         case DONT:
         case WONT:
            state = 0;
            break;

        case SB:                                    // expecting first suboption byte
            if (c == IAC) state = SE;
            else if (c == TTYPE) state = SBTT;
            else state = SBX;
            break;

        case SBTT:                                  // expecting terminal type send command
            if (c == IAC) { state = SE; break; }
            if (c == 1)
            {
                put(target, bytes(IAC, SB, TTYPE, 0), 4);
                put(target, getenv("TERM") ?: "dumb", 0);
                put(target, bytes(IAC, SE), 2);
            }
            state = SBX;
            break;

        case SBX:                                   // wait for end of suboption
            if (c == IAC) state = SE;               // just wait for IAC
            break;

        case SE:                                    // IAC in suboption
            if (c == SE) state = 0;                 // done if SE
            else state = SBX;                       // else keep waiting
            break;

    }
    return -1; // character is swalloed
}

// open targetname and set target
void do_connect()
{
    int first = 1;

    if (target >= 0)
    {
        // target is already open, we must be reconnecting
        close(target);
        printf("nanocom reconnecting to %s...\n", targetname);
    }

    while (1)
    {
        if (strchr(targetname, '/'))
        {
            // "/dev/ttyXXX"
            target = open(targetname, O_RDWR|O_NOCTTY|O_NONBLOCK);
            if (target >= 0)
            {
                // make sure it's raw
                struct termios io;
                tcgetattr(target, &io);
                io.c_oflag = 0;
                io.c_lflag = 0;
                io.c_cc[VMIN] = 1;
                io.c_cc[VTIME] = 0;
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
            // "host:port"
            char *host = strdup(targetname);
            char *port = strchr(host, ':');
            *port++ = 0;
            struct addrinfo *ai, hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
            int res = getaddrinfo(host, port, &hints, &ai);
            if (res) die("Unable to resolve %s: %s\n", targetname, gai_strerror(res));
            target = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (target < 0) die("Unable to create socket: %s\n", strerror(errno));
            if (connect(target, ai->ai_addr, ai->ai_addrlen))
            {
                if (errno != ECONNREFUSED && errno != ETIMEDOUT) reconnect = 0;
                target = -1;
            }
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
void run(char *cmd)
{
    while (isspace(*cmd)) cmd++;
    if (!cmd)
    {
        fprintf(stderr, "Must provide a shell command\n");
        return;
    }

    int cmdin[2], cmdout[2];
    if (pipe(cmdin) || pipe(cmdout)) die("Unable to create pipes: %s\n", strerror(errno));

    int pid = fork();
    if (pid < 0) die("Unable to fork: %s\n", strerror(errno));

    unsigned long long tx=0, rx=0;
    #define REND 0
    #define WEND 1

    if (!pid)
    {
        // child
        close(target);
        dup2(console, 2);       // dup the console to stderr
        dup2(cmdin[REND], 0);   // dup read end of the input pipe to stdin
        dup2(cmdout[WEND], 1);  // dup write end of output pipe to stdout
        close(cmdout[0]); close(cmdout[1]);
        close(cmdin[0]); close(cmdin[1]);
        setsid();
        fprintf(stderr, "Starting '%s' in %s...\n", cmd, getcwd(NULL,0));
        int status = system(cmd);       // run the command
        fprintf(stderr, "Exit status %d\n", WEXITSTATUS(status));
        close(1);
        exit(0);
    }

    // parent
    close(cmdin[REND]);    // close read end of input pipe
    close(cmdout[WEND]);   // close write end of output pipe

    while (1)
    {
        struct pollfd p[2] = {{ .fd = cmdout[REND], .events = POLLIN }, { .fd = target, .events = POLLIN }};
        poll(p, 2, -1);                                                 // wait for child or target
        if (p[0].revents)
        {
            int c = get(cmdout[REND]);                                  // read from child
            if (c < 0) break;                                           // done if terminated
            tx++;
            switch(c)
            {
                case CR: enter(); break;                                // expand CR to target
                case IAC: if (telnet) put(target, bytes(IAC), 1);       // escape telnet IAC and fall thru
                default: put(target, bytes(c), 1); break;               // put the character
            }
        }

        if (p[1].revents)
        {
            int c = get(target);                                        // read from target
            if (c < 0) break;                                           // urk, connection dropped
            if (!telnet || !iac(c))                                     // if not swallowed by telnet
            {
                rx++;
                if (put(cmdin[WEND], bytes(c), 1)) break;               // send to child, done if couldn't
            }
        }
    }

    close(cmdin[WEND]);
    close(cmdout[REND]);

    if (!kill(-pid, SIGINT)) msleep(250);                               // maybe kill child(ren)
    waitpid(-pid, NULL, WNOHANG);                                       // try to reap but not too hard
    fprintf(stderr, "Command received %lld and sent %lld bytes\n", rx, tx);
}

// Handle CTRL-\ escape key
void command(void)
{
    void bstat(char *fmt) { printf(fmt, (char*[]){"BS", "DEL"}[bskey]); }
    void estat(char *fmt) { printf(fmt, (char*[]){"CR", "LF", "CR+LF", "CR+NUL"}[enterkey]); }
    void sstat(char *fmt) { printf(fmt, (char*[]){"off", "on", "on with date"}[timestamp]); }
    void xstat(char *fmt) { printf(fmt, (char*[]){"off", "on", "all"}[showhex]); }

    ctty(WARM); // cooked without signals
    while(1)
    {
        stamp(0);
        printf("nanocom %s> ", targetname);

        // read command line
        char s[80];
        if (!fgets(s, sizeof(s), stdin)) die("fgets failed: %s\n", strerror(errno));

        char *p = strchr(s, 0);;
        while (p > s && isspace(*(p-1))) p--;           // rtrim
        *p = 0;
        for (p = s; isspace(*p); p++);                  // ltrim
        if (!*p) break;                                 // we're done
        if (*p == '!') run(p+1);
        else if (*(p+1)) goto invalid;
        else switch(*p)
        {
            case 'b':
                bskey = !bskey;
                bstat("Backspace key sends %s\n");
                break;

            case 'e':
                enterkey++;
                if (enterkey > 3) enterkey = 0;
                estat("Enter key sends %s\n");
                break;

            case 'p':
                put(target, bytes(FS), 1);
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

            invalid:
            default:
                printf("Invalid command: ");
                for(; *p; p++) printf(isprint(*p) ? "%c" : "[%02X]", *p);
                printf("\n\n");
                // fall thru

            case '?':
                printf("Commands:\n\n");
                bstat( "    b         - toggle backspace key sends DEL or BS (now %s)\n");
                estat( "    e         - cycle enter key sends LF, CR, CR+LF, or CR+NUL (now %s)\n");
                printf("    p         - forward ^\\ to target\n");
                printf("    q         - quit\n");
                sstat( "    s         - cycle timestamps off, on, or on with date (now %s)\n");
                xstat( "    x         - cycle hex output off, on, or all (now %s)\n");
                printf("    ! command - pass rest of the line to shell with stdin/stdout connected to target\n");
                printf("\n");
                printf("An empty line exits command mode.\n");
                break;
        }
    }
    ctty(RAW);
}

// write encoded character 128-255
void encode(unsigned int c)
{
    static char **table = NULL;

    if (c <= 127 || !encoding) return;         // not specified, ignore

    if (!*encoding) put(console, bytes(c), 1); // empty string, pass verbatim

    if (!table)
    {
        // build encode table on the first call
        table = calloc(128, sizeof(char *));
        if (!table) die("Out of memory!\n");

        setlocale(LC_CTYPE, "");
        iconv_t cd = iconv_open("//TRANSLIT", encoding);
        if (cd != (iconv_t)-1)
        {
            int n;
            for (n = 128; n < 256; n++)
            {
                // determine native sequence for each char and save in table
                char i = n, *in = &i, buf[8], *out = buf;
                size_t nin = 1, nout = sizeof(buf);
                if (iconv(cd, &in, &nin, &out, &nout) >= 0 && nout < sizeof(buf))
                    table[n & 127] = strndup(buf, sizeof(buf) - nout);
            }
            iconv_close(cd);
        }
        setlocale(LC_CTYPE, "C");
    }

    put(console, table[c & 127] ?: "?", 0);
}

int main(int argc, char *argv[])
{
    while (1) switch (getopt(argc,argv,":bdef:i:nrstx"))
    {
        case 'b': bskey = 1; break;
        case 'd': dtr = 1; break;
        case 'e': if (enterkey < 3) enterkey++; break;
        case 'f': teename = optarg; break;
        case 'i': encoding = optarg; break;
        case 'n': native = 1; break;
        case 'r': reconnect = 1; break;
        case 's': if (timestamp < 2) timestamp++; break;
        case 't': if (telnet < 2) telnet++; break;
        case 'x': if (showhex < 2) showhex++; break;

        case ':':              // missing
        case '?': die(usage);  // or invalid options
        case -1: goto optx;    // no more options
    } optx:

    if (optind >= argc) die(usage);

    targetname = argv[optind];
    do_connect();

    if (teename && (!(tee = fopen(teename, "a")))) die("Could not open tee file '%s': %s\n", teename, strerror(errno));

    if (telnet > 1 && enterkey == 0) enterkey = 3; // if not BINARY, default to CR+NUL

    signal(SIGPIPE, SIG_IGN);

    ctty(RAW); // put console in raw mode

    while(1)
    {
        struct pollfd p[2] = {{ .fd = console, .events = POLLIN }, { .fd = target, .events = POLLIN }};
        poll(p, 2, -1);

        if (p[0].revents)
        {
            // Process key from console. Note we're inside a loop, break to
            // skip forwarding the key to the target.
            int c = get(console);

            if (c < 0) die("Console error: %s\n", strerror(errno));

            switch(c)
            {
                case FS:
                    command();
                    break;

                case BS:
                case DEL:
                    put(target, bytes(bskey ? DEL: BS), 1);
                    break;

                case LF: // enter key
                    enter();
                    break;

                case IAC:
                    if (telnet) put(target, bytes(IAC),  1); // escape 255's if telnet
                    // fall thru

                default:
                    put(target, bytes(c), 1);
                    break;
            }

        }

        if (p[1].revents)
        {
            // Process character from target. Note we're inside a loop, break to
            // skip forwarding the character to the console

            // get key from target
            int c = get(target);

            if (c < 0)
            {
                // urk, target dropped
                ctty(COOKED);
                printf("nanocom lost connection to %s\n", targetname);
                if (!reconnect) exit(1);
                do_connect();
                ctty(RAW);
                if (telnet) iac(-1); // restart IACsn
                continue;
            }

            if (telnet && iac(c)) continue;                     // done if key swallowed by telnet IAC

            if (tee) fputc(c, tee);                             // maybe send raw data to the tee

            if (showhex > 1)                                    // show all as hex?
            {
                hex(c);
                clean = 0;
                continue;
            }

            switch(c)
            {
                case LF:
                    if (clean && timestamp) stamp(timestamp>1); // LF, maybe timestamp on blank line
                    put(console, bytes(CR, LF), 2);             // send CR+LF
                    clean = 1;                                  // we're now at the start of the line
                    break;

                case CR:
                    put(console, bytes(CR), 1);                 // just send it
                    break;

                default:
                    if (clean && timestamp) stamp(timestamp>1); // timestamp on blank line
                    clean = 0;                                  // not clean anymore

                    if (showhex && !isprint(c))                 // maybe show unprintable as hex
                    {
                        hex(c);
                        break;
                    }

                    if (c > 127)                                // maybe output high characters
                    {
                        encode(c);
                        break;
                    }

                    // send character to console
                    put(console, bytes(c), 1);
            }
        }
    }
    return 0;
}
