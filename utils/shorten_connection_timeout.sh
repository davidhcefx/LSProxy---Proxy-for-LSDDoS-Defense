#! /bin/bash
# Shorten TCP connection timeout, as the defaut timeout is WAY TOO LONG
# for a responsive proxy.
#
# Num of retries | Total wait time before timeout (s)
# -------------- | ----------------------------------
# 1              | 3
# 2              | 7
# 3              | 15
# 4              | 31
# 5              | 63
# 6              | 127  (system default)


retry_num=2
config=/proc/sys/net/ipv4/tcp_syn_retries
if [ -f $config ]; then
    if (( $(<$config) > retry_num )); then
        echo "$retry_num" | sudo tee "$config"
    fi
else
    echo "Cannot find '$config'. Please adjust tcp_syn_retries by hand."
    exit 1
fi
