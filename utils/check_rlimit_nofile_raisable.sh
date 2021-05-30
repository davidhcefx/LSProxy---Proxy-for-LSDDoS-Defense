#! /bin/bash
# Check if RLIMIT_NOFILE could be raised to MAX_FILE_DSC, if not then modify
# related config files.


limit_conf="/etc/security/limits.conf"   # per-process setting
file_max_conf="/proc/sys/fs/file-max"    # system-wide setting
systemd_conf="/etc/systemd/system.conf"  # systemd which starts the GUI
header="$(dirname "$PWD/$0")/../src/ls_proxy.h"
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

# no checks on arguments
function append_to() {
    target="$1"
    content="$2"
    echo "Appending '$content' to '$target'..."
    echo "$content" | sudo tee -a "$target"
}

# no checks on arguments
function write_to() {
    target="$1"
    content="$2"
    echo "Writing '$content' to '$target'..."
    echo "$content" | sudo tee "$target"
}

function modify_limit_conf() {
    if [ -z "$MAX_FILE_DSC" ]; then exit 1; fi
    if [ -f $limit_conf ]; then
        append_to $limit_conf "$user  hard  nofile  $MAX_FILE_DSC"
        echo -e "\nPlease LOGOUT in order to take affect!"
    else
        echo "Cannot find '$limit_conf'. Please raise the hard limit of" \
             "RLIMIT_NOFILE above $MAX_FILE_DSC manually."
    fi
}

# systemd, which starts the GUI, will impose additional limits
function modify_systemd_conf() {
    if [ -z "$MAX_FILE_DSC" ]; then exit 1; fi
    if [ -f $systemd_conf ]; then
        append_to $systemd_conf "DefaultLimitNOFILE=$MAX_FILE_DSC"
        echo -e "\nPlease REBOOT in order to take affect!"
    else
        echo "Cannot find '/etc/systemd/system.conf'. Please ensure by hand" \
             "that \`ulimit -n $MAX_FILE_DSC\` can success."
    fi
}

function raise_system_wide_file_max() {
    if [ -z "$MAX_FILE_DSC" ]; then exit 1; fi
    if [ -f $file_max_conf ]; then
        write_to $file_max_conf "$MAX_FILE_DSC"
    else
        echo "Cannot find '$file_max_conf'. Please report a bug!"
    fi
}


# main
MAX_FILE_DSC=$(get_MAX_FILE_DSC)
if ulimit -n "$MAX_FILE_DSC"; then  # try to raise limit
    if (( $(<$file_max_conf) < MAX_FILE_DSC )); then
        raise_system_wide_file_max
    fi
    echo "OK!"
else
    modify_limit_conf
    modify_systemd_conf
    exit 1
fi
