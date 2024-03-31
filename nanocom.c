// A simple serial/tcp comm program intended for use with (and on) embedded
// systems. See https://github.com/glitchub/nanocom for more information.

// This software is released as-is into the public domain, as described at
// https://unlicense.org. Do whatever you like with it.

char *usage = "Usage:\n"
              "\n"
#if NETWORK
              "    nanocom [options] /dev/ttyX|host:port\n"
              "\n"
              "Connect current tty to specified target, which is either a serial device (if\n"
              "target contains a '/') or a TCP host (if target contains a ':').\n"
#else
              "    nanocom [options] /dev/ttyX\n"
              "\n"
              "Connect current tty to specified serial device.\n"
#endif
              "\n"
              "Options:\n"
              "\n"
              "    -b          - backspace key sends DEL instead of BS\n"
              "    -d          - toggle serial port DTR high on start\n"
              "    -e          - enter key sends LF instead of CR\n"
              "    -f file     - log console output to specified file\n"
              "    -h          - display unprintable characters as hex\n"
              "    -H          - display all characters as hex\n"
#if TRANSLIT
              "    -i          - display high-bit characters as CP437\n"
              "    -I charset  - character set for -i, instead of CP437 ('iconv -l' for list)\n"
#endif
              "    -l mS       - flush characters after first connect until idle for specified mS\n"
              "    -L mS       - also flush on reconnect\n"
              "    -n          - don't force target tty to 115200 N-8-1\n"
              "    -r          - try to reconnect target if it won't open or closes with error\n"
              "    -s          - display timestamps\n"
              "    -S          - display date+timestamps\n"
#if TELNET
              "    -t          - enable telnet in binary mode\n"
              "    -T          - enable telnet in ASCII mode (handles CR+NUL)\n"
#endif
#if FXCMD
              "    -x command  - execute FX command after first connect\n"
              "    -X command  - also execute on reconnect\n"
#endif
              "\n"
              "Once connected, press key ^\\ for a menu of command options. Many of the settings\n"
              "above can be toggled there.\n"
              "\n"
              ;

#ifdef FXCMD
#define _GNU_SOURCE // for pipe2()
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
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
#if NETWORK
#include <sys/socket.h>
#include <netdb.h>
#endif
#include <poll.h>
#if TRANSLIT
#include <iconv.h>
#include <locale.h>
#endif
#if FXCMD
#include <pty.h>
#endif

#include "queue.h"
#if TELNET
#include "telnet.h"
#endif

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
bool reconnect = false;         // true = reconnect after failure
int showhex = 0;                // 1 = show received unprintable as hex, 2 = show all as hex
bool enterkey = false;          // true = enter key sends LF instead of CR
bool bskey = false;             // true = send DEL for BS
bool native = false;            // true = don't force serial 115200 N81
int timestamp = 0;              // 1 = show time, 2 = show date and time
bool dtr = false;               // true = twiddle serial DTR on connect
int flush = 0;                  // mS to flush characters after connect
bool reflush = false;           // true if flush on reconnect

#if TRANSLIT
bool encode = false;            // true = enable encoding
char *charset = NULL;           // iconv character set, default "CP437"
#endif
#if TELNET
int telnet = 0;                 // 0=disabled, 1=binary, 2=ascii
void *tctx = NULL;              // telnet context
#endif
#if FXCMD
char *start = NULL;             // initial FX command to run
bool restart = false;           // true if also run on reconnect
#endif

// Other globals
int target = 0;                 // target device or socket, if > 0
int teefd = 0;                  // tee file descriptor, if > 0
struct termios cooked;          // initial cooked console termios
#if FXCMD
char *running = NULL;           // name of currently running FX command or NULL, affects display() and command()
#endif

#define console STDOUT_FILENO   // console is stdout

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

#define bytes(...) (unsigned char []){__VA_ARGS__}  // define an array of bytes

queue qtarget = {0};            // queued data to be sent to target

// Put character(s) to file descriptor, if size is 0 use strlen(s). Return -1 on unrecoverable error.
int put(int fd, const void *s, size_t size)
{
    if (!s) return 0;             // ignore NULL
    if (!size) size = strlen(s);
    while (size)
    {
        int n = write(fd, s, size);
        if (n < 0) return -1;
        size -= n;
        s += n;
    }
    return 0;
}

// Mark file descriptor as non-blocking
#define nonblocking(fd) if (fcntl(fd, F_SETFL, O_NONBLOCK)) die("fcntl %d failed: %s\n", fd, strerror(errno))

// Wait for POLLIN (readable) or POLLOUT (writable) bits on specified file descriptor, returns -1 if error, 0 if
// timeout, else the set bit.  Timeout is in mS, -1 never times out.
int await(int fd, int events, int timeout)
{
    struct pollfd p = { .fd = fd, .events = events };
    int r = poll(&p, 1, timeout);
    if (!r) return 0;
    if (r == 1 && p.revents & events) return p.revents & events;
    return -1;
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

// Given one of the modes above, configure the console. Or given a character in RAW mode, display the character with
// timestamps, high-character encoding, hex conversion, and mirror to teefile if enabled.
void display(int c)
{
    // Current mode, 0 if uninitialized
    static int mode = 0;

#if TRANSLIT
    // unicode sequences for high-bit characters
    static char *translit[128];
#endif

    // RAW cursor state: 0=clean, 1=dirty, 2=dirty with deferred CR
    static int dirty = 0;

    // put to console and maybe tee
    void putcon(const void *s, size_t size)
    {
        put(console, s, size);
        if (teefd) put(teefd, s, size);
    }

    // put start of new line
    void startline(void)
    {
#if FXCMD
        if (running) { putcon("| ", 0); dirty = 1; }        // indicate FX command output
#endif
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
        if (teefd) put(teefd, bytes(LF), 1);                // LF to the tee
        dirty = 0;                                          // not dirty
    }

    void putCR(void)
    {
        put(console, bytes(CR), 1);                         // CR to the console
        if (teefd) put(teefd, bytes(LF), 1);                // but LF to the tee
        startline();                                        // maybe (re)timestamp
    }

    int puthex(int c)
    {
#if FXCMD
        if (running) return 0;                              // never hex FX output
#endif
        if (!showhex) return 0;                             // done if hex not enabled
        char s[5];
        sprintf(s, "[%.2X]", c & 0xff);                     // show "[XX]"
        putcon(s, 4);
        dirty = 1;
        return 1;                                           // note slurped
    }

    if (c == RECOOK)                                        // we're exiting, restore cooked if necessary
    {
        if (!mode) return;
        c = COOKED;
    }

    if (!mode)                                              // perform one-time init
    {
#if TRANSLIT
        setlocale(LC_CTYPE, "");
        iconv_t cd = iconv_open("//TRANSLIT", charset?:"CP437");
        setlocale(LC_CTYPE, "C");
        if (cd != (iconv_t)-1)
        {
            int n;
            for (n = 128; n < 256; n++)
            {
                // Get the unicode string for each high char
                char ch = n, *in = &ch, str[16], *out = str;
                size_t nin = 1, nout = sizeof(str);
                if (iconv(cd, &in, &nin, &out, &nout) < 0 || nin || nout == sizeof(str)) goto fail;
                translit[n & 127] = strndup(str, sizeof(str) - nout);
                if (!translit[n & 127]) die("%s\n", "Out of memory");
            }
            iconv_close(cd);
            if (!charset) charset = "CP437"; // encoding is allowed
        } else
        {
          fail:
            // die if specifically requested, else leave charset==NULL
            if (encode || charset) die("%s encoding not supported\n", charset?:"CP437");
        }
#endif
        tcgetattr(console, &cooked);                        // save current console config
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

    if (showhex > 1 && puthex(c)) return;                   // done if show all as hex

    switch(c)
    {
        case LF:                                            // LF
            if (!dirty) startline();                        // timestamp a blank line
            putLF();
            return;

        case CR:                                            // CR
            if (puthex(c)) return;                          // done if shown as hex
            if (dirty) dirty = 2;                           // ignore if clean else defer until next
            return;

        case TAB:                                           // various printables
        case FF:
        case ESC:
            if (puthex(c)) return;                          // done if shown as hex
            // fall through
        case BS:
        case 32 ... 126:
            if (!dirty) startline();                        // timestamp a blank line
            else if (dirty > 1) putCR();                    // or CR if deferred
            break;

        case 128 ... 255:                                   // high characters
#if FXCMD
            if (running) break;                             // FX output displays verbatim
#endif
            if (puthex(c)) return;                          // done if shown as hex
#if TRANSLIT
            if (encode)                                     // use unicode if enabled
            {
                if (!dirty) startline();                    // timestamp a blank line
                else if (dirty > 1) putCR();                // or CR if deferred
                putcon(translit[c & 127], 0);               // put encoded string
                dirty = 1;
                return;
            }
#endif
            break;                                          // show verbatim

        default:                                            // all others
            puthex(c);                                      // maybe show hex
            return;
    }

    // put character to the display
    putcon(bytes(c), 1);
    dirty = 1;
}

// sigwinch signal hander, technically should be #ifdef TELNET but complicates usage
bool sigwinch = false;
void set_sigwinch(int sig) { sigwinch = true; }

// Connect or reconnect to specified targetname and set the 'target' file descriptor. Targetname can be in form
// "host:port" or "/dev/ttyXXX".
void doconnect()
{
    int first = 1;

    if (target)
    {
        // target is already open, report drop
        printf("| Lost connection to %s\n", targetname);
        close(target);
        if (!reconnect) exit(1);
        printf("| Reconnecting to %s...\n", targetname);
    }

    while (1)
    {
#if NETWORK
        if (strchr(targetname, '/'))
        {
#endif
            // "/dev/ttyXXX"
            target = open(targetname, O_RDWR|O_NOCTTY|O_CLOEXEC);
            if (!target) die("Target file opened as fd 0?\n");
            if (target > 0)
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
#if NETWORK
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
            if (!target) die("Target socket opened as fd 0?\n");
            if (target < 0) die("Can't create socket: %s\n", strerror(errno));
            if (connect(target, ai->ai_addr, ai->ai_addrlen))
            {
                if (errno != ECONNREFUSED && errno != ETIMEDOUT && errno != ENETUNREACH) reconnect = 0;
                target = -1;
            }
            free(host);
            freeaddrinfo(ai);
            if (target > 0) break;
        }
        else die("%s must contain '/' or ':'\n", targetname);
#endif

        // connect failed
        if (first) printf("| Can't connect to %s: %s\n", targetname, strerror(errno));
        first = 0;
        if (!reconnect) exit(1); // die

        printf("| Retrying %s...\n", targetname);
        sleep(1);
    }

    nonblocking(target);        // make sure target is non-blocking

    if (flush) while (await(target, POLLIN, flush) > 0)
    {
        char fb[1024];
        if (read(target, fb, sizeof(fb)) < 0) break; // well, just break on read error
    }
    if (!reflush) flush = 0;

    delq(&qtarget, -1);         // wipe qtarget if reconnecting

#if TELNET
    if (telnet)
    {
        if (tctx) free(tctx);   // maybe free old context
        tctx = init_telnet(&qtarget, telnet == 1, getenv("TERM") ?: "dumb"); // target queue, binary, termtype
        sigwinch = true;        // trigger resize
    }
#endif

    printf("| Connected to %s, command key is ^\\.\n", targetname);
}

int command(void);

#if FXCMD
// Run specified FX command with stdin/stdout attached to target and stderr attached to console.
// If cmd is NULL, prompt for it.
void run(char *cmd)
{
    bool quiet = false;

    display(WARM);

    char buf[256];
    if (cmd)
        snprintf(buf, sizeof(buf), "%s", cmd);
    else
    {
        printf("| %s> ", getcwd(buf, sizeof(buf)) ? buf : "");
        fgets(buf, sizeof(buf), stdin);
    }

    cmd = strchr(buf, 0);                       // delete trailing whitespace
    while (cmd > buf && isspace(*(cmd-1))) cmd--;
    *cmd = 0;
    cmd = buf;
    if (*cmd == '-')                            // quiet if starts with '-'
    {
        quiet = true;
        cmd++;
    }
    while (isspace(*cmd)) cmd++;                // skip leading whitespace
    if (*cmd)
    {
        running = cmd;                          // remember it globally

        // use pipes for command stdin and stdout
        int cmdin[2], cmdout[2];
        if (pipe2(cmdin, O_CLOEXEC) || pipe2(cmdout, O_CLOEXEC)) die("Can't create pipes: %s\n", strerror(errno));
        #define rend(p) p[0] // read end
        #define wend(p) p[1] // write end

        setenv("NANOCOM", targetname, 1);       // set targetname in environment

        // shell gets a cooked pty to use for stderr
        int cmderr;
        int pid = forkpty(&cmderr, NULL, &cooked, NULL);
        if (pid < 0) die("Can't forkpty: %s\n", strerror(errno));
        if (!pid)
        {
            // child
            dup2(rend(cmdin), 0);               // stdin is the read end of the input pipe
            dup2(wend(cmdout), 1);              // stdout is the write end of output pipe
            if (!quiet) fprintf(stderr, "Running FX command '%s'...\n", cmd);
            execl("/bin/sh", "sh", "-c", cmd, NULL);
        }

        // parent
        close(rend(cmdin));                     // close child's pipe ends
        close(wend(cmdout));
        nonblocking(rend(cmdout));              // make our ends non-blocking
        nonblocking(wend(cmdin));
        nonblocking(cmderr);

        // cmdin can block, so queue it
        queue qcmdin = {0};

        // true if aborted by user ^\c
        bool aborted = false;

        // count bytes sent and received by child
        int tx = 0, rx = 0;

        // cmdout to qtarget, return bytes read or -1
        int cmdout2qtarget(void)
        {
            unsigned char bf[1024];
            int n = read(rend(cmdout), bf, sizeof bf);
            for (int i = 0; i < n; i++)
            {
#if TELNET
                if (!telnet || tx_telnet(tctx, bf[i]))
#endif
                    putq(&qtarget, &bf[i], 1);
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
            struct pollfd p[] = { { .fd = cmderr, .events = POLLIN },                                       // cmderr to console
                                  { .fd = console, .events = POLLIN },                                      // console to cmderr
                                  { .fd = availq(&qcmdin) < 4096 ? target : -1, .events = POLLIN },         // target to qcmdin, only if space
                                  { .fd = availq(&qtarget) < 4096 ? rend(cmdout) : -1, .events = POLLIN },  // cmdout to qtarget, only if space
                                  { .fd = availq(&qcmdin) ? wend(cmdin) : -1, .events = POLLOUT },          // qcmdin to cmdin, only if something in qcmdin
                                  { .fd = availq(&qtarget) ? target : -1, .events = POLLOUT } };            // qtarget to target, only if something in qtarget

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
                        aborted = true;
                        break;
                    }
                    if (!r) c = 0;      // discard
                }
                if (c && put(cmderr, bytes(c), 1) != 0) break;
            }

            if (p[2].revents)
            {
                // target to qcmdin
                unsigned char bf[1024];
                int n = read(target, bf, sizeof bf);
                if (n <= 0) break;
                for (int i = 0; i < n; i++)
#if TELNET
                    if (!telnet || rx_telnet(tctx, bf[i]))
#endif
                        putq(&qcmdin, bf+i, 1);
            }

            if (p[3].revents && cmdout2qtarget() <= 0) break; // cmdout to qtarget

            if (p[4].revents)
            {
                // qcmdin to cmdin
                int n = dequeue(&qcmdin, wend(cmdin));
                if (n <= 0) break;
                rx += n;
            }

            if (p[5].revents && dequeue(&qtarget, target) <= 0) break; // qtarget to target
        }

        // here, I/O error (possibly because of child exit) or abort.
        close(wend(cmdin)); // this may cause child to exit
        freeq(&qcmdin);

        // maybe flush pended cmdout to qtarget
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
                if (!quiet) printf("| Sending signal %d...\n", try[i]);
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
        if (!quiet)
        {
            printf("| FX command ");
            if (WIFEXITED(wstatus)) printf("exited with status %d", WEXITSTATUS(wstatus));
            else if (WIFSIGNALED(wstatus)) printf("killed by signal %d", WTERMSIG(wstatus));
            else printf("exited with unknown status %d", wstatus);
            printf(" after sending %u and receiving %u bytes\n", tx, rx);
        }

        running = NULL; // no longer running
    }
    display(RAW); // back to raw mode
}
#endif

void bstat(void) { printf("| Backspace key sends %s.\n", bskey ? "DEL" : "BS"); }
void estat(void) { printf("| Enter key sends %s.\n", enterkey ? "LF" : "CR"); }
void hstat(void) { printf("| %s characters are shown as hex.\n", (showhex > 1) ? "All" : (showhex ? "Unprintable" : "No")); }
#ifdef TRANSLIT
void istat(void) { printf("| %s encoding is %s.\n", charset, encode ? "on" : "off"); }
#endif
void rstat(void) { printf("| Automatic reconnect is %s.\n", reconnect ? "on" : "off"); }
void sstat(void) { printf("| Timestamps are %s.\n", (timestamp > 1) ? "on, with date" : (timestamp ? "on" : "off") ); }

// Command key handler. Return 1 if caller should send the COMMAND key to
// target, -1 if caller should kill running FX command, or 0.
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
        case 'h': showhex = !showhex; hstat(); break;
        case 'H': showhex = (showhex != 2) * 2; hstat(); break;
#if TRANSLIT
        case 'i': if (charset) encode = !encode, istat(); break;
#endif
        case 'q': display(COOKED); exit(0);
        case 'r': reconnect = !reconnect; rstat(); break;
        case 's': timestamp = !timestamp; sigwinch = true; sstat(); break;
        case 'S': timestamp = (timestamp != 2) * 2; sigwinch = true; sstat(); break;
#if FXCMD
        case 'x': if (running) ret = -1; else run(NULL); break;
#endif
        case '\\': ret = 1; break; // tell caller to forward the key
        case '?':
            printf("| Connected to %s.\n", targetname);
#if FXCMD
            if (running) printf("| Running FX command '%s'.\n", running);
#endif
#if TELNET
            if (telnet) printf("| Telnet is enabled in %s mode.\n", (telnet == 1) ? "binary" : "ASCII");
#endif
            if (teefd) printf("| Console output is logged to %s.\n", teename);
            bstat();
            estat();
            if (showhex) hstat();
#if TRANSLIT
            if (charset) istat();
#endif
            if (reconnect) rstat();
            if (timestamp) sstat();
            printf("|\n"
                   "| The following keys are supported after ^\\:\n"
                   "|    b - toggle backspace key between BS and DEL.\n"
                   "|    c - toggle enter key between CR and LF.\n"
                   "|    h - toggle unprintable characters as hex on or off.\n"
                   "|    H - toggle all characters as hex on or off.\n");
#if TRANSLIT
            if (charset) {
            printf("|    i - toggle %s encoding on or off.\n", charset);
            }
#endif
            printf("|    q - close connection and quit.\n"
                   "|    r - toggle automatic reconnect.\n"
                   "|    s - toggle timestamps on or off.\n"
                   "|    S - toggle long timestamps on or off.\n");
#ifdef FXCMD
            printf("|    x - %s.\n", running ? "kill running FX command" : "run FX command");
            printf("|    \\ - send ^\\ to %s.\n", running ? "FX command" : "target");
#else
            printf("|    \\ - send ^\\ to target.");
#endif
            printf("|\n"
                   "| Hit any key to continue...");
            display(RAW);
            key(5000); // Wait up to 5 seconds, this matters when target is spewing data.
            display(COOKED);
            printf("\n");
            display(RAW);
            break;
    }
    display(RAW);
    return ret;
}

int main(int argc, char *argv[])
{
    while (1) switch (getopt(argc,argv,":bdef:hHiI:l:L:nrsStTx:X:"))
    {
        case 'b': bskey = true; break;
        case 'd': dtr = true; break;
        case 'e': enterkey = true; break;
        case 'f': teename = optarg; break;
        case 'h': showhex = 1; break;
        case 'H': showhex = 2; break;
#if TRANSLIT
        case 'i': encode = true; break;
        case 'I': charset = optarg; break;
#endif
        case 'l': flush = atoi(optarg); reflush = false; break;
        case 'L': flush = atoi(optarg); reflush = true; break;
        case 'n': native = true; break;
        case 'r': reconnect = true; break;
        case 's': timestamp = 1; break;
        case 'S': timestamp = 2; break;
#if TELNET
        case 't': telnet = 1; break; // binary
        case 'T': telnet = 2; break; // ascii
#endif
#if FXCMD
        case 'x': start = optarg; restart = false; break;
        case 'X': start = optarg; restart = true; break;
#endif
        case -1: goto optx;                 // no more options
        default: die("%s\n", usage);        // invalid options
    } optx:
    if (optind >= argc) die("%s\n", usage);
    targetname = argv[optind];

    signal(SIGPIPE, SIG_IGN);               // ignore broken pipe
    signal(SIGQUIT, SIG_IGN);               // and ^],
    signal(SIGTSTP, SIG_IGN);               // and ^Z
#ifdef TELNET
    if (telnet) signal(SIGWINCH, set_sigwinch);
#endif

    while(1)
    {
        display(COOKED);
        doconnect();                        // connect (or reconnect) to target, or die
        if (teename && !teefd)              // open tee file if not already
        {
            teefd = open(teename, O_WRONLY|O_APPEND|O_CLOEXEC);
            if (teefd > 0) write(teefd, bytes(LF), 1); // if already exists, add a blank line
            else if (errno == ENOENT) teefd = open(teename, O_WRONLY|O_CREAT|O_CLOEXEC, 0644); // otherwise create it
            if (!teefd) die("Tee file opened as fd 0?\n");
            if (teefd < 0) die("Can't open tee file %s: %s\n", teename, strerror(errno));
        }
        display(RAW);
#if FXCMD
        if (start)
        {
            // run start command
            run(start);
            if (!restart) start = NULL;
        }
#endif
        while(1)
        {
#if TELNET
            if (telnet && sigwinch)
            {
                // resize telnet window
                sigwinch = false;
                struct winsize ws;
                if (ioctl(console, TIOCGWINSZ, &ws) == 0)
                {
                    int cols = ws.ws_col;
                    switch(timestamp)
                    {
                        case 1: cols -= 15; break;  // "[HH:MM:SS.mmm] "
                        case 2: cols -= 26; break;  // "[YYYY:MM:DD HH:MM:SS.mmm] "
                    }
                    resize_telnet(tctx, cols, ws.ws_row);
                }
            }
#endif

            struct pollfd p[] = { { .fd = console, .events = POLLIN },
                                  { .fd = target, .events = POLLIN },
                                  { .fd = availq(&qtarget) ? target : -1, .events = POLLOUT } };

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
#if TELNET
                        if (!telnet || tx_telnet(tctx, CR))
#endif
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
#if TELNET
                    if (!telnet || rx_telnet(tctx, bf[i]))
#endif
                        display(bf[i]);         // display it
            }

            // send qtarget if target writable
            if (p[2].revents && dequeue(&qtarget, target) <= 0) break;
        }
    }
}
