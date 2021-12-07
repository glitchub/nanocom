# Example script for nanocom shell command, run with:
#
#   ^]x bash example.sh
#
# The target device is accessed via stdin and stdout, the terminal via stderr.

echo "Listing current directory..." >&2

# Send ls command to target on stdout, then slurp stdin until lines stop
# coming. The final command prompt won't appear since read expects CR
# termination.
printf "ls -al\r"
while read -t 1 line; do echo $line >&2; done

echo "Bye!" >&2
