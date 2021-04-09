#! /bin/bash

max_connection=65536  # please see lowslow_proxy.h
value=$((max_connection * 3 + 4))
conf=/etc/security/limits.conf
user=$(id -un)

if [ -f $conf ]; then
    for n in $(sed -n -E "s/^($user|\*) +(hard|-) +nofile//p" $conf); do
        if (( $n >= $value )); then
            echo "done"
            exit 0
        fi
    done
    echo -e "\n$user    hard    nofile    $value" | sudo tee -a $conf
else
    echo "Cannot find '$conf'. Please ensure the hard limit of RLIMIT_NOFILE is above $value."
    exit 1
fi
