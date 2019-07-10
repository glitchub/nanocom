A simple serial comm program with switchable timestamps, hex mode, sz/rz
support, etc, for use on embedded platforms.

Usage:

    nanocom [options] serial_device

where:

    -f file - tee received serial data to specified file
    -k      - enable keylock
    -l      - ENTER key sends LF 
    -n      - use existing stty config, do not force 115200 N-8-1
    -r      - try to reconnect to serial device if it won't open or closes with error
    -s      - BS key sends DEL 
    -t      - enable timestamps (use twice to enable dates)
    -x      - show unprintable chars as hex (use twice to show all chars as hex)
