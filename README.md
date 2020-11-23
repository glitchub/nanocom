A simple serial comm program with switchable timestamps, hex mode, sz/rz
support, etc, for use on embedded platforms.

Usage:

    nanocom [options] /dev/ttyX|host:port

Connect tty to specified target, which is either a serial device (if target
contains a '/') or a TCP host (if target contains a ':').

Options:

    -b       - backspace keys sends DEL instead of BS
    -d       - toggle serial port DTR high on start
    -e       - enter key sends LF instead of CR
    -f file  - tee received data to specified file
    -n       - use existing serial port config, do not force 115200 N-8-1
    -r       - try to reconnect to target if it won't open or closes with error
    -t       - enable timestamps (use twice to enable dates)
    -x       - show unprintable chars as hex (use twice to show all chars as hex)

Once connected, pressing the escape character CTRL+\ enters command mode, supporting
the following commands:

    b        - toggle backspacekey sends DEL or BS
    e        - toggle enter key sends LF or CR
    p        - pass escape (CTRL-\) to target
    q        - quit
    t        - cycle timestamps off, time, or date+time
    x        - cycle hex off, unprintable, or all
    ! cmd    - execute shell 'cmd' with stdin/stdout connected to target

Enter any valid command, or just enter by itself, to exit command mode.
