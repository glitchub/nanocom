// Telnet stream processor

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "queue.h"
#include "telnet.h"

// ASCII characters of interest
#define NUL 0
#define CR 13

// Magic telnet IAC characters, these are also used as states
#define SB 250          // IAC SB = start suboption
#define SE 240          // IAC SE = end suboption
#define WILL 251        // IAC WILL XX = will support option XX
#define WONT 252        // IAC WONT XX = won't perform option XX
#define DO 253          // IAC DO XX = do perform option XX
#define DONT 254        // IAC DONT XX = don't perform option XX
#define IAC 255         // IAC IAC = literal 0xff character

// Derived states
#define SBX 0x100       // waiting for end of suboption
#define SBTT 0x200      // waiting for TTYPE suboption command

// The telnet "XX" options of interest
#define BINARY 0        // binary (doesn't use CR+NUL)
#define SECHO 1         // server echo
#define SGA 3           // suppress go ahead (we require this)
#define TTYPE 24        // terminal type aka $TERM
#define NAWS 31         // Negotiate About Window Size

#define bytes(...) (unsigned char []){__VA_ARGS__}  // define an array of bytes

typedef struct
{
    queue *target;          // queue that is delivered to target
    bool binary;            // negotiate binary telnet
    char *termtype;         // terminal type string
    bool initialized;       // true if connection has been initialized
    int state;              // IAC state
    bool wascr;             // true if last character was CR in ASCII mode
    bool willnaws;          // true if we say WILL NAWS
    bool donaws;            // true if server replies DO NAWS
    unsigned char hicols;   // window width
    unsigned char locols;
    unsigned char hirows;   // window height
    unsigned char lorows;
} context;

// Initialize telnet IAC processor and return pointer to context or NULL.
void *init_telnet(queue *target, bool binary, char *termtype)
{
    context *ctx = calloc(1, sizeof(context));
    if (!ctx) abort(); // abort on OOM
    ctx->target = target;
    ctx->binary = binary;
    ctx->termtype = termtype;
    return ctx;
}

// send window size to server
static void naws(context *ctx)
{
    putq(ctx->target, bytes(IAC, SB, NAWS), 3);
    if (ctx->hicols == IAC) putq(ctx->target, bytes(IAC, IAC), 2); else putq(ctx->target, bytes(ctx->hicols), 1);
    if (ctx->locols == IAC) putq(ctx->target, bytes(IAC, IAC), 2); else putq(ctx->target, bytes(ctx->locols), 1);
    if (ctx->hirows == IAC) putq(ctx->target, bytes(IAC, IAC), 2); else putq(ctx->target, bytes(ctx->hirows), 1);
    if (ctx->lorows == IAC) putq(ctx->target, bytes(IAC, IAC), 2); else putq(ctx->target, bytes(ctx->lorows), 1);
    putq(ctx->target, bytes(IAC, SE), 2);
}

// Given character to be sent to telnet, return true if caller should send it or false if handled internally
bool tx_telnet(void *_ctx, unsigned char c)
{
    if (_ctx)
    {
        context *ctx = _ctx;
        if (c == IAC) { putq(ctx->target, bytes(IAC, IAC), 2); return false; }               // always expand IAC
        if (!ctx->binary && c == CR) { putq(ctx->target, bytes(CR, NUL), 2); return false; } // expand CR in ascii mode
    }
    return true; // caller can handle it
}

// Given a character received from telnet, return true if caller should handle it or false if handled internally
bool rx_telnet(void *_ctx, unsigned char c)
{
    if (!_ctx) return true;                         // tell caller to handle it
    context *ctx = _ctx;

    switch(ctx->state)
    {
        case 0:
            if (c == IAC)                           // escape?
            {
                ctx->state = IAC;                   // remember it
                break;
            }

            if (!ctx->binary)                       // ascii mode?
            {
                if (c == NUL && ctx->wascr)         // swallow NUL after CR
                {
                    ctx->wascr = false;
                    break;
                }
                ctx->wascr = c == CR;               // remember CR
            }
            return true;                            // caller should handle it

        case IAC:
            switch(c)
            {
                case IAC:                           // escaped
                    ctx -> state = 0;
                    return true;                    // caller should handle it

                case SB:                            // use command code as state
                case WILL:
                case WONT:
                case DO:
                case DONT:
                    ctx->state = c;
                    break;

                default:                            // ignore any other
                    ctx->state = 0;
                    break;
            }

            if (!ctx->initialized)
            {
                // Send our initial requests to server
                putq(ctx->target, bytes(IAC, DO, SGA), 3);
                putq(ctx->target, bytes(IAC, WILL, SGA), 3);
                if (ctx->termtype) putq(ctx->target, bytes(IAC, WILL, TTYPE), 3);
                putq(ctx->target, bytes(IAC, DO, SECHO), 3);
                if (ctx->binary)
                {
                    putq(ctx->target, bytes(IAC, DO, BINARY), 3);
                    putq(ctx->target, bytes(IAC, WILL, BINARY), 3);
                }
                if (ctx->willnaws) putq(ctx->target, bytes(IAC, WILL, NAWS), 3);
                ctx->initialized = true;
            }
            break;

        case WILL:
            ctx->state = 0;
            switch(c)
            {
                case SGA:
                case SECHO:
                    break;

                case BINARY:
                    if (!ctx->binary) goto dont;
                    break;

                default:
                dont:
                    putq(ctx->target, bytes(IAC, DONT, c), 3);
                    break;
            }
            break;

         case DO:
            ctx->state = 0;
            switch(c)
            {
                case SGA:
                    break;

                case BINARY:
                    if (!ctx->binary) goto wont;
                    break;

                case TTYPE:
                    if (!ctx->termtype) goto wont;
                    break;

                case NAWS:
                    if (!ctx->willnaws) goto wont;
                    ctx->donaws = true;
                    naws(ctx);
                    break;

                default:
                wont:
                    putq(ctx->target, bytes(IAC, WONT, c), 3);
                    break;
            }
            break;

         case DONT:
         case WONT:
            ctx->state = 0;
            break;

        case SB:                                    // expecting first suboption byte
            if (c == IAC) ctx->state = SE;
            else if (c == TTYPE && ctx->termtype) ctx->state = SBTT;
            else ctx->state = SBX;
            break;

        case SBTT:                                  // expecting terminal type send command
            if (c == IAC) { ctx->state = SE; break; }
            if (c == 1)
            {
                putq(ctx->target, bytes(IAC, SB, TTYPE, 0), 4);
                putq(ctx->target, ctx->termtype, strlen(ctx->termtype));
                putq(ctx->target, bytes(IAC, SE), 2);
            }
            ctx->state = SBX;
            break;

        case SBX:                                   // wait for end of suboption
            if (c == IAC) ctx->state = SE;          // just wait for IAC
            break;

        case SE:                                    // IAC in suboption
            if (c == SE) ctx->state = 0;            // done if SE
            else ctx->state = SBX;                  // else keep waiting
            break;

    }
    return false;                                   // character has been handled
}

// Update server's window size via NAWS (if server supports)
void resize_telnet(void *_ctx, int cols, int rows)
{
    if (_ctx)
    {
        context *ctx = _ctx;
        if (cols < 8) cols = 8; else if (cols > 65535) cols = 65535;
        ctx->hicols = cols >> 8;
        ctx->locols = cols & 255;
        if (rows < 2) rows = 2; else if (rows > 65535) rows = 65535;
        ctx->hirows = rows >> 8;
        ctx->lorows = rows & 255;
        if (!ctx->willnaws)
        {
            // first call, send WILL NAWS
            ctx->willnaws = true;
            if (ctx->initialized) putq(ctx->target, bytes(IAC, WILL, NAWS), 3);
        } else if (ctx->donaws)
        {
            // server previously said DO NAWS
            naws(ctx);
        }
    }
}
