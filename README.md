# LSProxy - Proxy for LSDDoS Defense

[![Build & Test](https://github.com/davidhcefx/LSProxy---Proxy-for-LSDDoS-Defense/actions/workflows/test.yml/badge.svg)](https://github.com/davidhcefx/LSProxy---Proxy-for-LSDDoS-Defense/actions/workflows/test.yml)

LSProxy is a [reverse proxy][] to defend low and slow DDoS attacks (LSDDoS), which is a kind of application layer denial of service attack (DoS) that drains server's resources by sending packages at a very slow date rate. There are three kinds of LSDDoS, namely **Slow header attacks**, **Slow body attacks** and **Slow read attacks**. This solution is capable of protecting both [Apache][] and [Nginx][] server, doesn't require [additional server-side modifications][] (which could turn into a tedious task for web admins), and is tolerable to low bandwidth users (meaning that users' connections won't be dropped due to their network being slow).

## Build
1. Adjust the parameters.
    - Adjust the macros defined in [`ls_proxy.h`](/src/ls_proxy.h) to your own needs.

2. Compile and test.
    ```sh
    $ make
    $ make test
    ```
    - **[Caution!]** For the build dependencies and the security needs, it will:
      * Install packages: [`g++-10`][g++], `libevent-dev`.
      * Modify files: [`/etc/security/limits.conf`][limits.conf], `/etc/systemd/system.conf`, `/proc/sys/fs/file-max`, `/proc/sys/net/ipv4/tcp_syn_retries`.
    - We recommend you to try out in a VM or docker.

3. Execute. (See more options via `ls_proxy -h`)
    ```sh
    $ ./ls_proxy 8080 localhost
    ```
    - It can also be run in the background like this: `./ls_proxy 8080 localhost >$(date -Isec).log 2>&1 &`.

4. (optional) Configure DNS records.
    - Since **LSProxy** acts as a reverse proxy, we might want to associate our domain name to it's IP address.
    - While [testing other sites](#some-http-sites-to-try-out), one can create temporary DNS mappings via the [hosts file][]. For example, adding a line to `/etc/hosts` would do the trick for Linux:
        ```hosts
        127.0.0.1  www.wangafu.net
        ```

## Some HTTP sites to try out
- Text only: http://www.wangafu.net/~nickm/libevent-book/
- With images: http://linux.vbird.org/
- Large files: http://free.nchc.org.tw/ubuntu-cd/
- Interactive: http://www.dailysudoku.com/sudoku/play.shtml?today=1


## License
With the exeption of [nodejs/llhttp][] source code, LSProxy is licensed under [MIT license](/LICENSE).



[reverse proxy]: https://en.wikipedia.org/wiki/Reverse_proxy
[Apache]: https://httpd.apache.org/
[Nginx]: https://www.nginx.com/
[additional server-side modifications]: https://www.digitalocean.com/community/tutorials/how-to-configure-nginx-as-a-web-server-and-reverse-proxy-for-apache-on-one-ubuntu-18-04-server
[g++]: https://github.com/davidhcefx/LSProxy---Proxy-for-LSDDoS-Defense/blob/5cf49a21998c7a6ed5e62e2d739e02846b6639bb/Makefile#L33
[limits.conf]: https://github.com/davidhcefx/LSProxy---Proxy-for-LSDDoS-Defense/blob/5cf49a21998c7a6ed5e62e2d739e02846b6639bb/utils/check_rlimit_nofile_raisable.sh#L6
[hosts file]: https://en.wikipedia.org/wiki/Hosts_(file)
[nodejs/llhttp]: https://github.com/nodejs/llhttp
