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

#define _GNU_SOURCE // for pipe2()
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
#include <pty.h>

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

// Other globals
int target = -1;                // target device or socket, if >= 0
int teefd = -1;                 // tee file descriptor, if >= 0
struct termios cooked;          // initial cooked console termios
char *running = NULL;           // name of currently running shell command or NULL, affects display() and command()

#define console 1               // Assume stdin is the console device

// Console modes for display()
#define COOKED -1               // COOKED, set whatever state the console was in to start with
#define WARM -2                 // COOKED without ISIG, for user input
#define RAW -3                  // RAW, all characters must be sent via display()
#define RECOOK -4               // restore COOKED if not already

void display(int c);            // write character to raw console or set console mode

// restore console, registered with atexit()
void recook(void) { display(RECOOK); }

// restore console to cooked mode and die
#define die(...) recook(), fprintf(stderr, __VA_ARGS__), exit(1)

typedef struct
{
    int head, count, size;
    unsigned char *data;
} queue;

queue qtarget = {0};            // target can block, queue outgoing data

// Add count bytes of data to specified queue, which is expanded as needed to fit
void putq(queue *q, void *data, int count)
{
    if (q->size <= q->count + count)
    {
        if (!q->size) q->size = 1024; // first time init
        while (q->size <= q->count + count) q->size *= 2;
        q->data = realloc(q->data, q->size); // just malloc() if q->data == NULL
        if (!q->data) die("putq failed: %s\n", strerror(errno));
    }

    for (int i = 0; i < count; i++, q->count++)
        q->data[(q->head + q->count) % q->size] = *(unsigned char *)(data+i);
}

// Point *data at a monolithic chunk of queue data and return its length, or 0
// if queue is empty. Caller must call delq() once the getq data has been
// consumed.
int getq(queue *q, void **data)
{
    if (!q->count) return 0;
    *data = q->data + q->head;
    int len = q->size - q->head;
    if (len > q->count) len = q->count;
    return len;
}

// Remove specified number of characters from queue, after using getq.
void delq(queue *q, int count)
{
    if (!count) return;
    if (count < 0 || count >= q->count)
    {
        q->count = 0;
        q->head = 0;
        return;
    }
    q->count -= count;
    q->head = (q->head + count) % q->size;
}

// Move data from queue to file descriptor, return number of bytes written
// (possibly 0) or -1 if error.
int dequeue(queue *q, int fd)
{
    void *p;
    int n = getq(q, &p);
    if (!n) return 0;
    int r = write(fd, p, n);
    if (r > 0) delq(q, r);
    return r;
}

// Mark file descriptor as non-blocking
#define nonblocking(fd) if (fcntl(fd, F_SETFL, O_NONBLOCK)) die("fcntl %d failed: %s\n", fd, strerror(errno))

// Wait for POLLIN (readable) or POLLOUT (writable) bits on specified file
// descriptor, returns -1 if error, 0 if timeout, else the set bit.
// Timeout is in mS, -1 never times out.
int await(int fd, int events, int timeout)
{
    struct pollfd p = { .fd = fd, .events = events };
    int r = poll(&p, 1, timeout);
    if (!r) return 0;
    if (r == 1 && p.revents & events) return p.revents & events;
    return -1;
}

// Put character(s) to file descriptor, if size is 0 use strlen(s). Return -1
// on unrecoverable error.
#define bytes(...) (unsigned char []){__VA_ARGS__}  // define an array of bytes
int put(int fd, const void *s, size_t size)
{
    if (!s) return 0;                                       // ignore NULL
    if (!size) size = strlen(s);                            // get size
    while (size)
    {
        int n = write(fd, s, size);
        if (n < 0) return -1;
        size -= n;
        s += n;
    }
    return 0;
}

// Return character from raw console, 0 if timeout, or die
int key(int timeout)
{
    int r = await(console, POLLIN, timeout);
    if (!r) return 0;
    if (r > 0)
    {
        unsigned char c;
        if (read(console, &c, 1) == 1) return c;
    }
    die("Console error: %s\n", strerror(errno));
}

// Given one of the modes above, configure the console. Or given a character in
// RAW mode, display the character with timestamps, high-character encoding,
// hex conversion, and mirror to teefile if enabled.
void display(int c)
{
    // Current mode, 0 if uninitialized
    static int mode = 0;
    static char **strings = NULL;   // table of 128 strings

    // RAW cursor state: 0=clean, 1=dirty, 2=dirty with deferred CR
    static int dirty = 0;

    // put to console and maybe tee
    void putcon(const void *s, size_t size)
    {
        put(console, s, size);
        if (teefd >= 0) put(teefd, s, size);
    }

    // put start of new line
    void startline(void)
    {
        if (running) { putcon("| ", 0); dirty = 1; }        // indicate shell command output
        if (!timestamp) return;
        struct timeval t;
        gettimeofday(&t, NULL);                             // get current time
        struct tm *l = localtime(&t.tv_sec);
        char s[40];                                         // format it
        sprintf(s + strftime(s, sizeof(s)-10, (timestamp) > 1 ?  "[%Y-%m-%d %H:%M:%S": "[%H:%M:%S", l),".%.3d] ", (int)t.tv_usec/1000);
        putcon(s, 0);
        dirty = 1;
    }

    void putLF(void)
    {
        put(console, bytes(CR, LF), 2);                     // CRLF to console
        if (teefd >= 0) put(teefd, bytes(LF), 1);           // LF to the tee
        dirty = 0;                                          // not dirty
    }

    void putCR(void)
    {
        put(console, bytes(CR), 1);                         // CR to the console
        if (teefd >= 0) put(teefd, bytes(LF), 1);           // but LF to the tee
        startline();                                        // maybe (re)timestamp
    }

    if (c == RECOOK)                                        // we're exiting, restore cooked if necessary
    {
        if (!mode) return;
        c = COOKED;
    }

    if (!mode)                                              // perform one-time init
    {
        tcgetattr(console, &cooked);                        // save current console config
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
        atexit(recook);                                     // restore cooked on unexpected exit
        mode = COOKED;                                      // we are now in COOKED mode
    }

    if (c < 0)                                              // new mode?
    {
        if (c == mode || c < RAW) return;                   // ignore redundant or invalid (RECOOK)
        if (dirty) putLF();                                 // make the cursor clean
        mode = c;
        switch(mode)
        {
            case RAW:
            {
                // enable raw console
                fflush(stdout);
                struct termios t = cooked;
                t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK);
                t.c_lflag &= 0;
                tcsetattr(console, TCSANOW, &t);
                break;
            }

            case COOKED:
                tcsetattr(console, TCSANOW, &cooked);       // restore original config
                break;

            case WARM:
            {
                struct termios t = cooked;
                t.c_lflag &= ~ISIG;                         // same as COOKED but without ISIG
                tcsetattr(console, TCSANOW, &t);
                break;
            }
        }
        return;
    }

    // Here, handle valid characters in RAW mode
    if (mode != RAW || c > 255) return;                     // done if not

    if (!running && showhex > 1) goto hex;                  // maybe show as hex

    switch(c)
    {
        case LF:                                            // LF
            if (!dirty) startline();                        // timestamp a blank line
            putLF();
            return;

        case CR:                                            // CR
            if (!running && showhex) goto hex;              // maybe hex
            if (dirty) dirty = 2;                           // ignore if clean else defer until next
            return;

        case TAB:                                           // various printables
        case FF:
        case ESC:
            if (!running && showhex) goto hex;              // maybe hex
            // fall through
        case BS:
        case 32 ... 126:
            if (!dirty) startline();                        // timestamp a blank line
            else if (dirty > 1) putCR();                    // or CR if deferred
            break;

        case 128 ... 255:                                   // high characters
            if (running) break;                             // shell output displays verbatim
            if (showhex) goto hex;                          // maybe hex
            if (!encoding) return;                          // ignore if not encoding
            if (!dirty) startline();                        // timestamp a blank line
            else if (dirty > 1) putCR();                    // or CR if deferred
            if (!*encoding) break;                          // -i'' was specified, just go display
            putcon(strings[c & 127], 0);                    // else put encoded string
            dirty = 1;
            return;

        default:                                            // all others, ignore except hex
            if (!running && showhex)
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
    void senddo(int c) { putq(&qtarget, bytes(IAC, DO, c), 3); }
    void senddont(int c) { putq(&qtarget, bytes(IAC, DONT, c), 3); }
    void sendwill(int c) { putq(&qtarget, bytes(IAC, WILL, c), 3); }
    void sendwont(int c) { putq(&qtarget, bytes(IAC, WONT, c), 3); }

    static int state = 0;   // IAC state
    static int isinit = 0;  // true if we've sent our initial IAC requests
    static int wascr = 0;   // true if last character was CR in ASCII mode

    if (c < 0)
    {
        // initialize, when connection is established
        state = 0;
        isinit = 0;
        wascr = 0;
        return -1;
    }

    c &= 0xff;

    switch(state)
    {
        case 0:
            if (c == IAC)                           // escape?
            {
                state = IAC;                        // remember it
                break;
            }

            if (telnet > 1)                         // ASCII mode?
            {
                if (c == NUL && wascr)              // swallow NUL after CR
                {
                    wascr = 0;
                    break;
                }
                wascr = c == CR;                    // remember CR
            }
            return 0;                               // caller can send it

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
                char *term = getenv("TERM") ?: "dumb";
                putq(&qtarget, bytes(IAC, SB, TTYPE, 0), 4);
                putq(&qtarget, term, strlen(term));
                putq(&qtarget, bytes(IAC, SE), 2);
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

// Connect or reconnect to specified targetname and set the 'target' file
// descriptor. Targetname can be in form "host:port" or "/dev/ttyXXX".
void doconnect()
{
    int first = 1;

    if (target >= 0)
    {
        // target is already open, report drop
        printf("| Lost connection to %s\n", targetname);
        close(target);
        if (!reconnect) exit(1);
        printf("| Reconnecting to %s...\n", targetname);
    }

    while (1)
    {
        if (strchr(targetname, '/'))
        {
            // "/dev/ttyXXX"
            target = open(targetname, O_RDWR|O_NOCTTY|O_CLOEXEC);
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
                    usleep(50000); // 50 mS
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
            target = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
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

        // connect failed
        if (first) printf("| Can't connect to %s: %s\n", targetname, strerror(errno));
        first = 0;
        if (!reconnect) exit(1); // die

        printf("| Retrying %s...\n", targetname);
        sleep(1);
    }

    nonblocking(target);        // make sure target is non-blocking
    delq(&qtarget, -1);         // empty qtarget
    iac(-1);                    // init telnet

    printf("| Connected to %s, command key is ^\\.\n", targetname);
}

int command(void);

// Prompt for shell command and run with stdin/stdout attached to target and stderr attached to console.
void run(void)
{
    display(WARM);
    char buf[256];
    printf("| Enter a shell command to run with stdin/stdout attached to target and stderr attached to console.\n");
    if (getcwd(buf, 255)) printf("| Current directory is %s\n", buf);
    printf("| > ");
    fgets(buf, sizeof(buf), stdin);
    char *cmd = strchr(buf, 0);                 // delete trailing whitespace
    while (cmd > buf && isspace(*(cmd-1))) cmd--;
    *cmd = 0;
    cmd = buf;                                  // and leading whitespace
    while (isspace(*cmd)) cmd++;
    if (*cmd)
    {
        running = cmd;                          // remember it globally

        // use pipes for command stdin and stdout
        int cmdin[2], cmdout[2];
        if (pipe2(cmdin, O_CLOEXEC) || pipe2(cmdout, O_CLOEXEC)) die("Can't create pipes: %s\n", strerror(errno));
        #define rend(p) p[0] // read end
        #define wend(p) p[1] // write end

        // use a pty for command stderr
        struct winsize ws;
        ioctl(console, TIOCGWINSZ, &ws);
        ws.ws_col -= 28; // parent may prepend "| " and a timestamp

        // shell gets a cooked pty to use for stderr
        int cmderr, pty;
        if (openpty(&cmderr, &pty, NULL, &cooked, &ws)) die("Can't create pty: %s\n", strerror(errno));

        int pid = fork();
        if (pid < 0) die("Can't fork: %s\n", strerror(errno));
        if (!pid)
        {
            // child
            dup2(rend(cmdin), 0);               // stdin is the read end of the input pipe
            dup2(wend(cmdout), 1);              // stdout is the write end of output pipe
            dup2(pty, 2);                       // stderr is the pty
            close(cmderr);                      // close the extra ttys
            close(pty);                         // (everything else has F_CLOEXEC)
            // set the controlling terminal
            if (setsid() == -1) die("setsid() failed: %s\n", strerror(errno));
            if (ioctl(2, TIOCSCTTY, NULL) < 0) die("TIOCSCTTY failed: %s\n", strerror(errno));
            fprintf(stderr, "Running shell command '%s'...\n", cmd);
            execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
        }

        // parent
        close(rend(cmdin));                     // close child's pipe ends
        close(wend(cmdout));
        close(pty);
        nonblocking(rend(cmdout));              // make our ends non-blocking
        nonblocking(wend(cmdin));
        nonblocking(cmderr);

        // cmdin can block, so queue it
        queue qcmdin = {0};

        // 1 = aborted by user
        int aborted = 0;

        // count bytes sent and received by child
        int tx = 0, rx = 0;

        // cmdout to qtarget, handles IACs, return bytes read or -1
        int cmdout2qtarget(void)
        {
            unsigned char bf[1024];
            int n = read(rend(cmdout), bf, sizeof bf);
            for (int i = 0; i < n; i++)
            {
                if (telnet && bf[i] == IAC) putq(&qtarget, bytes(IAC, IAC), 2);
                else if (telnet > 1 && bf[i] == CR) putq(&qtarget, bytes(CR, NUL), 2);
                else putq(&qtarget, &bf[i], 1);
                tx++;
            }
            return n;
        }

        // cmderr to console, return bytes read or -1
        int cmderr2console(void)
        {
            unsigned char bf[1024];
            int n = read(cmderr, bf, sizeof bf);
            for (int i = 0; i < n; i++) display(bf[i]);
            return n;
        }

        display(RAW);

        while (1)
        {
            struct pollfd p[] = { { .fd = cmderr, .events = POLLIN },                                   // cmderr to console
                                  { .fd = console, .events = POLLIN },                                  // console to cmderr
                                  { .fd = qcmdin.count < 4096 ? target : -1, .events = POLLIN },        // target to qcmdin, only if space
                                  { .fd = qtarget.count < 4096 ? rend(cmdout) : -1, .events = POLLIN }, // cmdout to qtarget, only if space
                                  { .fd = qtarget.count ? target : -1, .events = POLLOUT },             // qtarget to target, only if something in qtarget
                                  { .fd = qcmdin.count ? wend(cmdin) : -1, .events = POLLOUT } };       // qcmdin to cmdin, only if something in qcmdin

            int r = poll(p, 6, -1);
            if (r <= 0) break;

            if (p[0].revents && cmderr2console() <= 0) break; // cmderr to console

            if (p[1].revents)
            {
                // console to cmderr
                int c = key(0);
                if (c == COMMAND)       // command key
                {
                    int r = command();  // handle it, 0 = done, -1 = user abort, 1 = send command key to target
                    if (r < 0)
                    {
                        aborted = 1;
                        break;
                    }
                    if (!r) c = 0;      // discard
                }
                if (c && put(cmderr, bytes(c), 1) != 1) break;
            }

            if (p[2].revents)
            {
                // target to qcmdin
                unsigned char bf[1024];
                int n = read(target, bf, sizeof bf);
                if (n <= 0) break;
                for (int i = 0; i < n; i++)
                    if (!iac(bf[i]))
                        putq(&qcmdin, bf+i, 1);
            }

            if (p[3].revents && cmdout2qtarget() <= 0) break; // cmdout to qtarget

            if (p[4].revents)
            {
                // qtarget to target
                if (dequeue(&qtarget, target) <= 0) break;
            }

            if (p[5].revents)
            {
                // qcmdin to cmdin
                int n = dequeue(&qcmdin, wend(cmdin));
                if (n <= 0) break;
                rx += n;
            }
        }

        // here, I/O error (possibly because of child exit) or abort.
        close(wend(cmdin)); // this may cause child to exit
        free(qcmdin.data);  // done with qcmdin

        // maybe flush pended cmdout
        if (!aborted) while (cmdout2qtarget() > 0);
        close(rend(cmdout));

        // flush pended cmderr
        while (cmderr2console() > 0);
        close(cmderr);

        display(WARM);

        int wstatus;
        usleep(100000); // 100 mS
        if (!waitpid(pid, &wstatus, WNOHANG))
        {
            // kill sh and friends in increasingly impolite ways
            int try[] = {SIGTERM, SIGHUP, SIGINT, SIGKILL, 0};
            for (int i = 0; try[i]; i++)
            {
                printf("| Sending signal %d...\n", try[i]);
                kill(-pid, try[i]);
                for (int i = 0; i < 10; i++)
                {
                    usleep(100000); // 100 mS
                    if (waitpid(pid, &wstatus, WNOHANG)) goto out;
                }
            }
            wstatus = -1;
        }
        out:
        printf("| Shell command ");
        if (WIFEXITED(wstatus)) printf("exited with status %d", WEXITSTATUS(wstatus));
        else if (WIFSIGNALED(wstatus)) printf("killed by signal %d", WTERMSIG(wstatus));
        else printf("exited with unknown status %d", wstatus);
        printf(" after sending %u and receiving %u bytes\n", tx, rx);

        running = NULL; // no longer running
    }
    display(RAW); // back to raw mode
}

void bstat(void) { printf("| Backspace key sends %s.\n", bskey ? "DEL" : "BS"); }
void estat(void) { printf("| Enter key sends %s.\n", enterkey ? "LF" : "CR"); }
void hstat(void) { printf("| %s characters are shown as hex.\n", (showhex > 1) ? "All" : (showhex ? "Unprintable" : "No")); }
void rstat(void) { printf("| Automatic reconnect is %s.\n", reconnect ? "on" : "off"); }
void sstat(void) { printf("| Timestamps are %s.\n", (timestamp > 1) ? "on, with date" : (timestamp ? "on" : "off") ); }

// Command key handler. Return 1 if caller should send the COMMAND key to
// target, -1 if caller should kill running shell command, or 0.
int command(void)
{
    int ret = 0;
    display(WARM);
    printf("| Command (? for help)? ");
    // read one character from console
    display(RAW);
    int c = key(5000) ?: '?';
    display(WARM);
    printf("%c\n", (c >= ' ' && c <= '~') ? c : 0);
    switch(c)
    {
        case 'b': bskey = !bskey; bstat(); break;
        case 'e': enterkey = !enterkey; estat(); break;
        case 'f': ret = 1; break; // tell caller to forward the key
        case 'h': showhex = (showhex + 1) % 3; hstat(); break;
        case 'q': display(COOKED); exit(0);
        case 'r': reconnect = !reconnect; rstat(); break;
        case 's': timestamp = (timestamp + 1) % 3; sstat(); break;
        case 'x': if (running) ret = -1; else run(); break;
        case '?':
            printf("| Connected to %s.\n", targetname);
            if (running) printf("| Running shell command '%s'.\n", running);
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
                   "|    f - forward ^\\ to %s.\n"
                   "|    h - cycle hex output off, unprintable, or all.\n"
                   "|    q - close connection and quit.\n"
                   "|    r - toggle automatic reconnect.\n"
                   "|    s - cycle timestamps off, on, or on with date.\n"
                   "|    x - %s.\n"
                   "|\n"
                   "| Hit any key to continue...",
                    running ? "shell command" : "target",
                    running ? "kill running shell command" : "run a shell command"
                  );
            display(RAW);
            key(5000); // wait 5 seconds
            display(LF);
            break;
    }
    display(RAW);
    return ret;
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

        case -1: goto optx;                 // no more options
        default: die(usage);                // invalid options
    } optx:
    if (optind >= argc) die(usage);
    targetname = argv[optind];

    signal(SIGPIPE, SIG_IGN);               // ignore broken pipe
    signal(SIGQUIT, SIG_IGN);               // and ^\,
    signal(SIGTSTP, SIG_IGN);               // and ^Z

    while(1)
    {
        display(COOKED);
        doconnect();                        // connect (or reconnect) to target, or die
        if (teename && teefd < 0)           // open tee file if not already
        {
            teefd = open(teename, O_WRONLY|O_APPEND|O_CLOEXEC);
            if (teefd >= 0) write(teefd, bytes(LF), 1); // if already exists, add a blank line
            else if (errno == ENOENT) teefd = open(teename, O_WRONLY|O_CREAT|O_CLOEXEC, 0644); // otherwise create it
            if (teefd < 0) die("Can't open tee file %s: %s\n", teename, strerror(errno));
        }
        display(RAW);

        while(1)
        {
            struct pollfd p[] = { { .fd = console, .events = POLLIN },
                                  { .fd = target, .events = POLLIN },
                                  { .fd = qtarget.count ? target : -1, .events = POLLOUT } };
            poll(p, 3, -1);

            if (p[0].revents)
            {
                // console to target
                int c = key(0);
                switch(c)
                {
                    case COMMAND:               // command key
                        if (command() == 1)     // maybe forward command key to target
                            putq(&qtarget, bytes(COMMAND), 1);
                        break;

                    case BS:                    // backspace sends BS or DEL
                    case DEL:
                        putq(&qtarget, bytes(bskey ? DEL : BS), 1);
                        break;

                    case LF:                    // enter sends CR or LF
                        if (enterkey)
                            putq(&qtarget, bytes(LF), 1);
                        else
                        if (telnet > 1)         // ASCII telnet needs CR+NUL
                            putq(&qtarget, bytes(CR, NUL), 2);
                        else
                            putq(&qtarget, bytes(CR), 1);
                        break;

                    case 0:                     // drop 0
                    case CR:                    // drop CR
                    case 128 ... 255:           // and high characters
                        break;

                    default:                    // send all others
                        putq(&qtarget, bytes(c), 1);
                        break;
                }
            }

            if (p[1].revents)
            {
                // target to console
                unsigned char bf[1024];
                int n = read(target, bf, sizeof bf);
                if (n <= 0) break;              // assume dropped if errorr
                for (int i = 0; i < n; i++)     // for each char
                    if (!iac(bf[i]))            // if not slurped by telnet
                        display(bf[i]);         // display it
            }

            // send qtarget if target writable
            if (p[2].revents && dequeue(&qtarget, target) <= 0) break;
        }
    }
}
