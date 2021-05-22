#! /bin/bash
# Check if the hard limit of RLIMIT_NOFILE is enough.


header="$(dirname "$PWD/$0")/../src/ls_proxy.h"
limit_conf="/etc/security/limits.conf"
user=$(id -un)

# get computed value of MAX_FILE_DSC defined in $header
function get_MAX_FILE_DSC() {
    MAX_CONNECTION=$( \
        sed -n -E 's/^#define MAX_CONNECTION +([0-9]+).*/\1/p' "$header")
    expr=$( \
        sed -n -E 's|^#define MAX_FILE_DSC +([^/]+)//.*|\1|p' "$header" | \
        sed "s/MAX_CONNECTION/$MAX_CONNECTION/g")
    echo $((expr))
}

function append_to() {
    target="$1"
    content="$2"
    if [ -f "$target" ]; then
        echo "Appending '$content' to '$target'..."
        echo "$content" | sudo tee -a "$target"
    fi
}

# append "$user hard nofile $MAX_FILE_DSC" to $limit_conf
function modify_limit_conf() {
    if [ -z "$MAX_FILE_DSC" ]; then echo "Error: MAX_FILE_DSC unset"; exit 1; fi
    if [ -f $limit_conf ]; then
        append_to $limit_conf "$user  hard  nofile  $MAX_FILE_DSC"
        echo -e "\nPlease LOGOUT in order to take affect!"
    else
        echo "Cannot find '$limit_conf'. Please raise the hard limit of" \
             "RLIMIT_NOFILE above $MAX_FILE_DSC manually."
    fi
}

# some GUI imposes additional limits; modify their configuration as well
function modify_gui_conf() {
    if [ -z "$MAX_FILE_DSC" ]; then echo "Error: MAX_FILE_DSC unset"; exit 1; fi
    if [ -f /etc/systemd/system.conf ]; then
        append_to /etc/systemd/system.conf "DefaultLimitNOFILE=$MAX_FILE_DSC" 
        echo -e "\nPlease REBOOT in order to take affect!"
    else
        echo "Cannot find '/etc/systemd/system.conf'. Please ensure by hand" \
             "that \`ulimit -n $MAX_FILE_DSC\` can success."
    fi
}

# main
MAX_FILE_DSC=$(get_MAX_FILE_DSC)
if ulimit -n "$MAX_FILE_DSC"; then  # try to raise limit
    echo "OK!"
else
    modify_limit_conf
    modify_gui_conf
    exit 1
fi
