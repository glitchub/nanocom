# Example script for nanocom shell command, run with:
#
#   ^\x bash example.sh
#
# The target device is accessed via pipes on stdin and stdout, the console via a pty on stderr.

die() { echo "$*" >2; exit 1; }

if [ -t 1 ]; then
    # stdout is a tty!
    die "This script should be run as a nanocom shell command"
fi

echo "Listing current directory..." >&2

# Send a command to target on stdout, then slurp (and log) stdin until lines stop coming.
# The final command prompt won't appear since read is blocked on CR termination.
printf "ls -al\r"
while read -t 1 line; do echo $line >&2; done

echo "Bye!" >&2
