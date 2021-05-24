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
    -e          - enter key sends LF instead of CR\n\
    -f file     - log console output to specified file\n\
    -h          - display unprintable characters as hex, 2X to display all characters as hex\n\
    -i encoding - encoding for high characters e.g. 'CP437', or '' to display verbatim\n\
    -n          - don't set serial port to 115200 N-8-1, use it as is\n\
    -r          - try to reconnect target if it won't open or closes with error\n\
    -s          - display timestamp, 2X to display with date\n\
    -t          - enable telnet in binary mode, 2X for ASCII mode (handles CR+NUL)\n\
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

// ASCII controls of interest
#define NUL 0
#define BS 8
#define TAB 9
#define LF 10
#define FF 12
#define CR 13
#define ESC 27
#define DEL 127

#define COMMAND 28              // command character is '^\'

// options
char *targetname;               // target name, eg "/dev/ttyX" or "host:port"
char *teename = NULL;           // tee file name
int reconnect = 0;              // 1 = reconnect after failure
int showhex = 0;                // 1 = show received unprintable as hex, 2 = show all as hex
int enterkey = 0;               // 1 = enter key sends LF instead of CR
int bskey = 0;                  // 1 = send DEL for BS
int native = 0;                 // 1 = don't force serial 115200 N81
int timestamp = 0;              // 1 = show time, 2 = show date and time
int dtr = 0;                    // 1 = twiddle serial DTR on connect
char *encoding = NULL;          // encoding name (see iconv -l)
int telnet = 0;                 // 1 = BINARY telnet, 2 = ASCII telnet

// global file descriptors
int target = -1;                // target device or socket, if >= 0
int tee = -1;                   // tee file descriptor, if >= 0

#define msleep(mS) usleep(mS*1000)

// Console device aka stdout
#define console 1

// Console modes
#define COOKED -1               // COOKED (whatever state the console was in to start with)
#define WARM -2                 // COOKED without ISIG, used in the command menu
#define RAW -3                  // RAW, all characters must be sent via display()
#define INIT -4                 // perform one-time init, this must be last

void display(int c);            // write character to raw console or set console mode
void cooked(void) { display(COOKED); } // registered with atexit

// restore console to cooked mode and die
#define die(...) cooked(), printf(__VA_ARGS__), exit(1)

// return monotonic milliseconds, wraps every 49 days
unsigned int mS(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock_gettime failed: %s\n", strerror(errno));
    return (t.tv_sec * 1000) + (t.tv_nsec / 1000000L);
}

// Return character from file descriptor. If unrecoverable error die for console else return -1.
int get(int fd)
{
    unsigned char c;
    while (1)
    {
        int n = read(fd, &c, 1);
        if (n == 1) return c;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (fd == console) die("Console error: %s\n", strerror(errno));
        return -1;
    }
}

// Put character(s) to file descriptor, if size is 0 use strlen(s). Return -1
// on unrecoverable error.
#define bytes(...) (char *)(unsigned char []){__VA_ARGS__}  // define an array of bytes
int put(int fd, const char *s, size_t size)
{
    if (!s) return 0;                                       // ignore NULL
    if (!size) size = strlen(s);                            // get size
    if (!size) return 0;                                    // done if zero size

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

// Given one of the modes above, configure the console. Or given a character in
// RAW mode, display the character with timestamps, high-character encoding,
// hex conversion, and mirror to teefile if enabled.
void display(int c)
{
    // Current mode, RAW, WARM or COOKED
    static int mode = 0;            // 0 triggers initialization
    static struct termios config;   // initial console termios
    static char **strings = NULL;   // table of 128 strings

    // RAW console state: 0=clean, 1=dirty, 2=dirty with deferred CR
    static int dirty = 0;

    // put to console and maybe tee
    void putcon(const char *s, size_t size)
    {
        put(console, s, size);
        if (tee >= 0) put(tee, s, size);
    }

    void putstamp(void)
    {
        if (!timestamp) return;
        struct timeval t;
        gettimeofday(&t, NULL);                             // get current time
        struct tm *l = localtime(&t.tv_sec);
        char s[40];                                         // format it
        sprintf(s + strftime(s, sizeof(s)-10, (timestamp) > 1 ?  "[%Y-%m-%d %H:%M:%S": "[%H:%M:%S", l),".%.3d] ", (int)t.tv_usec/1000);
        putcon(s, 0);
    }

    void putLF(void)
    {
        put(console, bytes(CR, LF), 2);                     // CRLF to console
        if (tee >= 0) put(tee, bytes(LF), 1);               // LF to the tee
    }

    void putCR(void)
    {
        put(console, bytes(CR), 1);                         // CR to the console
        if (tee >= 0) put(tee, bytes(LF), 1);               // but LF to the tee
        putstamp();                                         // maybe (re)timestamp
    }

    if (c == INIT)
    {
        if (mode) return;                                   // once only
        tcgetattr(console, &config);                        // save current console config
        if (encoding && *encoding)
        {
            // create table of strings for high characters
            strings = calloc(128, sizeof(char *));
            if (!strings) die("Out of memory");
            setlocale(LC_CTYPE, "");
            iconv_t cd = iconv_open("//TRANSLIT", encoding);
            if (cd == (iconv_t)-1) die("Invalid encoding '%s' (try 'iconv -l' for a list)\n", encoding);
            int n;
            for (n = 128; n < 256; n++)
            {
                // Determine the encoded string for each char and save in
                // table.
                char ch = n, *in = &ch, str[16], *out = str;
                size_t nin = 1, nout = sizeof(str);
                if (iconv(cd, &in, &nin, &out, &nout) < 0) die("iconv failed: %s\n", strerror(errno));
                nout = sizeof(str) - nout;
                strings[n & 127] = nout ? strndup(str, nout) : "?";;
                if (!strings[n & 127]) die("Out of memory");
            }
            iconv_close(cd);
            setlocale(LC_CTYPE, "C");
        }
        atexit(cooked);                                     // restore cooked on unexpected exit
        mode = COOKED;                                      // we are now in COOKED mode
        return;
    }

    if (c <= INIT || c > 255 || !mode) return;              // ignore invalid

    if (c < 0)                                              // new mode?
    {
        if (c == mode) return;                              // oops, already set!
        if (mode == RAW && dirty) putLF();                  // maybe linefeed when leaving RAW
        mode = c;                                           // remember new mode
        switch(mode)
        {
            case RAW:
            {
                fflush(stdout);
                struct termios t = config;
                t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK);
                t.c_lflag &= 0;
                tcsetattr(console, TCSANOW, &t);
                dirty = 0;                                  // Assume cursor is at start of line
                break;
            }

            case COOKED:
                tcsetattr(console, TCSANOW, &config);       // restore original config
                break;

            case WARM:
            {
                struct termios t = config;
                t.c_lflag &= ~ISIG;                         // same as COOKED but without ISIG
                tcsetattr(console, TCSANOW, &t);
                break;
            }
        }
        return;
    }

    // here, c is 0 to 255
    if (mode != RAW) return;                                // ignore unless RAW mode

    if (showhex > 1) goto hex;                              // maybe unconditional hex

    switch(c)
    {
        case LF:                                            // LF
            if (!dirty) putstamp();                         // timestamp a blank line
            putLF();
            dirty = 0;
            return;

        case CR:                                            // CR
            if (showhex) goto hex;                          // maybe hex
            if (dirty) dirty = 2;                           // ignore if clean else defer until next
            return;

        case TAB:                                           // various printables
        case FF:
        case ESC:
            if (showhex) goto hex;                          // maybe hex
            // fall through
        case BS:
        case 32 ... 126:
            if (!dirty) putstamp();                         // timestamp a blank line
            else if (dirty > 1) putCR();                    // or CR if deferred
            break;

        case 128 ... 255:                                   // high characters
            if (showhex) goto hex;                          // maybe hex
            if (!encoding) return;                          // ignore if not encoding
            if (!dirty) putstamp();                         // timestamp a blank line
            else if (dirty > 1) putCR();                    // or CR if deferred
            if (!*encoding) break;                          // -i'' was specified, just go display
            putcon(strings[c & 127], 0);                    // else put encoded string
            dirty = 1;
            return;

        default:                                            // all others, ignore except hex
            if (showhex)
            {
                char s[5];
                hex:
                sprintf(s, "[%.2X]", c & 0xff);
                putcon(s, 0);
                dirty = 1;
            }
            return;
    }

    // put character to the display
    putcon(bytes(c), 1);
    dirty = 1;
}

// Magic telnet IAC characters, these are also used as states
#define SB 250      // IAC SB = start suboption
#define SE 240      // IAC SE = end suboption
#define WILL 251    // IAC WILL XX = will support option XX
#define WONT 252    // IAC WONT XX = won't perform option XX
#define DO 253      // IAC DO XX = do perform option XX
#define DONT 254    // IAC DONT XX = don't perform option XX
#define IAC 255     // IAC IAC = literal 0xff character

// Derived states
#define SBX 0x100   // waiting for end of suboption
#define SBTT 0x200  // waiting for TTYPE suboption command

// The "XX" options of interest
#define BINARY 0    // binary (doesn't use CR+NUL)
#define SECHO 1     // server echo
#define SGA 3       // suppress go ahead (we require this)
#define TTYPE 24    // terminal type aka $TERM
#define NAWS 31     // Negotiate About Window Size

// Given a character from target, process telnet IAC commands and return -1 if
// character was swallowed else 0. Call with -1 after connect to initialize the
// iac state machine.
int iac(int c)
{
    if (!telnet) return 0; // only if telnet enabled

    // Send various IAC messages
    void senddo(int c) { put(target, bytes(IAC, DO, c), 3); }
    void senddont(int c) { put(target, bytes(IAC, DONT, c), 3); }
    void sendwill(int c) { put(target, bytes(IAC, WILL, c), 3); }
    void sendwont(int c) { put(target, bytes(IAC, WONT, c), 3); }

    static int state = 0;   // IAC state
    static int isinit = 0;  // true if we've sent our initial IAC requests

    if (c < 0)
    {
        // initialize, when connection is established
        state = 0;
        isinit = 0;
        return -1;
    }

    c &= 0xff;

    switch(state)
    {
        case 0:
            if (c != IAC) return 0;                 // caller can send it
            state = IAC;
            break;

        case IAC:
            switch(c)
            {
                case IAC:                           // escaped
                    state = 0;
                    return 0;                       // caller can send it

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

// Connect to specified targetname and set the 'target' file descriptor.
// Targetname can be in form "host:port" or "/dev/ttyXXX".
void contact()
{
    int first = 1;

    if (target >= 0)
    {
        // target is already open, we must be reconnecting
        close(target);
        printf("| Reconnecting to %s...\n", targetname);
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
                if (tcsetattr(target, TCSANOW, &io)) die ("Can't configure %s: %s\n", targetname, strerror(errno));

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
            if (res) die("Can't resolve %s: %s\n", targetname, gai_strerror(res));
            target = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (target < 0) die("Can't create socket: %s\n", strerror(errno));
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
        if (first) printf("| Can't connect to %s: %s\n", targetname, strerror(errno));
        first=0;
        if (!reconnect) exit(1); // die

        printf("| Retrying %s...\n", targetname);
        sleep(1);
    }

    printf("| Connected to %s, command key is ^\\.\n", targetname);
}

// Prompt for shell command and run with stdin/stdout attached to target.
void run(void)
{
    display(WARM);
    char s[256];
    if (!getcwd(s, 255)) s[0] = 0;
    printf("| Current directory is %s\n", s);
    printf("| Enter a shell command to run with stdin/stdout attached to target.\n| > ");
    fgets(s, sizeof(s), stdin);
    char *cmd = strchr(s, 0);   // rstrip
    while (cmd > s && isspace(*(cmd-1))) cmd--;
    *cmd = 0;
    cmd = s;
    while(isspace(*cmd)) cmd++; // lstrip
    if (*cmd)
    {
        int cmdin[2], cmdout[2];
        if (pipe(cmdin) || pipe(cmdout)) die("Can't create pipes: %s\n", strerror(errno));

        int pid = fork();
        if (pid < 0) die("Can't fork: %s\n", strerror(errno));

        unsigned long long tx=0, rx=0;
        #define REND 0
        #define WEND 1

        if (!pid)
        {
            // child
            close(target);
            dup2(console, 2);           // dup the console to stderr
            dup2(cmdin[REND], 0);       // dup read end of the input pipe to stdin
            dup2(cmdout[WEND], 1);      // dup write end of output pipe to stdout
            close(cmdout[0]); close(cmdout[1]);
            close(cmdin[0]); close(cmdin[1]);
            setsid();
            fprintf(stderr, "| Running command '%s'...\n", cmd);
            int status = system(cmd);   // run the command
            fprintf(stderr, "\n| Command exit status: %d\n", WEXITSTATUS(status));
            close(1);
            exit(0);
        }

        // parent
        close(cmdin[REND]);             // close read end of input pipe
        close(cmdout[WEND]);            // close write end of output pipe

        int wascr = 0;

        while (1)
        {
            struct pollfd p[2] = {{ .fd = cmdout[REND], .events = POLLIN }, { .fd = target, .events = POLLIN }};
            poll(p, 2, -1);                                                 // wait for child or target
            if (p[0].revents)
            {
                int c = get(cmdout[REND]);                                  // read from child
                if (c < 0) break;                                           // done if terminated

                tx++;
                if (c == CR && telnet > 1)                                  // CR is sent as CR+NUL if ASCII telnet
                {
                    if (put(target, bytes(CR, NUL), 2)) break;
                }
                else if (c == IAC && telnet)                                // IAC must be escaped if telnet
                {
                    if (put(target, bytes(IAC, IAC), 2)) break;
                }
                else
                {
                    if (put(target, bytes(c), 1)) break;                    // otherwise send the character, done if unable
                }
            }

            if (p[1].revents)
            {
                int c = get(target);                                        // read from target
                if (c < 0) break;                                           // urk, connection dropped

                if (telnet && iac(c)) continue;                             // done if swallowed by telnet

                if (telnet > 1)                                             // ASCII telnet?
                {
                    if (wascr)                                              // If previous was CR
                    {
                        wascr = 0;
                        if (c == NUL) continue;                             // swallow the NUL if CR+NUL
                    }
                    if (c == CR) wascr = 1;                                 // Remember CR
                }

                rx++;
                if (put(cmdin[WEND], bytes(c), 1)) break;                   // send character to child, done if couldn't
            }
        }

        close(cmdin[WEND]);
        close(cmdout[REND]);

        if (!kill(-pid, SIGINT)) msleep(250);                               // maybe kill child(ren)
        waitpid(-pid, NULL, WNOHANG);                                       // try to reap but not too hard
        fprintf(stderr, "| Command received %lld and sent %lld bytes\n", rx, tx);
    }
    // return to raw mode
    display(RAW);
}

// Command key handler
void bstat(void) { printf("| Backspace key sends %s.\n", bskey ? "DEL" : "BS"); }
void estat(void) { printf("| Enter key sends %s.\n", enterkey ? "LF" : "CR"); }
void hstat(void) { printf("| %s characters are shown as hex.\n", (showhex > 1) ? "All" : (showhex ? "Unprintable" : "No")); }
void rstat(void) { printf("| Automatic reconnect is %s.\n", reconnect ? "on" : "off"); }
void sstat(void) { printf("| Timestamps are %s.\n", (timestamp > 1) ? "on, with date" : (timestamp ? "on" : "off") ); }
void command(void)
{
    display(WARM);
    printf("| Command (? for help)? ");
    display(RAW);
    int c = 0;
    struct pollfd p = { .fd = console, .events = POLLIN };
    if (poll(&p, 1, 5000)) c = get(console); // give it 5 seconds
    display(WARM);
    printf("%c\n", (c >= ' ' && c <= '~') ? c : 0);
    switch(c)
    {
        case 'b': bskey = !bskey; bstat(); break;
        case 'e': enterkey = !enterkey; estat(); break;
        case 'f': put(target, bytes(COMMAND), 1); break;
        case 'h': showhex = (showhex + 1) % 3; hstat(); break;
        case 'q': display(COOKED); exit(0);
        case 'r': reconnect = !reconnect; rstat(); break;
        case 's': timestamp = (timestamp + 1) % 3; sstat(); break;
        case '!': run(); break;
        case '?':
            printf("| Connected to %s.\n", targetname);
            if (telnet) printf("| Telnet is enabled in %s mode.\n", telnet == 1 ? "binary" : "ASCII");
            if (teename) printf("| Console output is logged to %s.\n", teename);
            bstat();
            estat();
            if (showhex) hstat();
            else if (encoding) printf("| High characters are encoded as %s.\n", *encoding ? encoding : "raw bytes");
            if (reconnect) rstat();
            if (timestamp) sstat();
            printf("|\n"
                   "| The following keys are supported after ^\\:\n"
                   "|    b - toggle backspace key between BS and DEL.\n"
                   "|    e - toggle enter key between CR and LF.\n"
                   "|    f - forward ^\\ to target.\n"
                   "|    h - cycle hex output off, unprintable, or all.\n"
                   "|    q - close connection and quit.\n"
                   "|    r - toggle automatic reconnect.\n"
                   "|    s - cycle timestamps off, on, or on with date.\n"
                   "|    ! - prompt for shell command and run it with stdin/stdout attached to target.\n"
                   "|    ? - show this text.\n"
                   );
            break;
    }
    display(RAW);
}

int main(int argc, char *argv[])
{
    while (1) switch (getopt(argc,argv,":bdef:hi:nrstx"))
    {
        case 'b': bskey = 1; break;
        case 'd': dtr = 1; break;
        case 'e': enterkey = 1; break;
        case 'f': teename = optarg; break;
        case 'x': // legacy, fall thru
        case 'h': if (showhex < 2) showhex++; break;
        case 'i': encoding = optarg; break;
        case 'n': native = 1; break;
        case 'r': reconnect = 1; break;
        case 's': if (timestamp < 2) timestamp++; break;
        case 't': if (telnet < 2) telnet++; break;

        case ':':                           // missing
        case '?': die(usage);               // or invalid options
        case -1: goto optx;                 // no more options
    } optx:
    if (optind >= argc) die(usage);
    targetname = argv[optind];

    display(INIT);                          // initialize console or die

    signal(SIGPIPE, SIG_IGN);               // disable SIGPIPE

    contact();                              // connect to target

    if (teename)
    {
        // open tee file, sets tee file descriptor global
        tee = open(teename, O_WRONLY|O_APPEND);
        if (tee >= 0) write(tee, bytes(LF), 1); // if already exists, add a blank line
        else if (errno == ENOENT) tee = open(teename, O_WRONLY|O_CREAT, 0644); // otherwise create it
        if (tee < 0) die("Can't open tee file %s: %s\n", teename, strerror(errno));
    }

    display(RAW);                           // console in raw mode

    while(1)
    {
        struct pollfd p[2] = {{ .fd = console, .events = POLLIN }, { .fd = target, .events = POLLIN }};
        poll(p, 2, -1);

        if (p[0].revents)
        {
            // Process key from console
            int c = get(console);

            switch(c)
            {
                case COMMAND:               // command key
                    command();
                    break;

                case BS:                    // backspace sends BS or DEL
                case DEL:
                    put(target, bytes(bskey ? DEL : BS), 1);
                    break;

                case LF:                    // enter sends CR or LF
                    if (enterkey)
                        put(target, bytes(LF), 1);
                    else
                    {
                        if (telnet > 1) put(target, bytes(CR, NUL), 2); // ASCII telnet needs CR+NUL
                        else put(target, bytes(CR), 1);
                    }
                    break;

                case CR:                    // drop CR
                case 128 ... 255:           // and high characters
                    break;

                default:                    // send all others
                    put(target, bytes(c), 1);
                    break;
            }
        }

        if (p[1].revents)
        {
            // Process character from target
            int c = get(target);

            if (c < 0)
            {
                // Urk, target dropped
                display(COOKED);
                printf("| Lost connection to %s\n", targetname);
                if (!reconnect) exit(1);    // Done unless reconnect is enabled
                contact();
                if (telnet) iac(-1);        // reset IAC state machine
                display(RAW);
                continue;
            }

            if (telnet && iac(c)) continue; // process telnet inband characters

            display(c);                     // else display it
        }
    }
    return 0;
}
