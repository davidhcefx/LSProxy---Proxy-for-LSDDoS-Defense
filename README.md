# LSProxy - Proxy for LSDDoS Defense

> Description in construction...

## Installation
1. Adjust the parameters
    - Adjust the macros defined in `ls_proxy.h` to your own needs.

2. Compile & run
    ```sh
    make
    make test
    ./ls_proxy 8080 localhost
    ```
    - Alternatively, run in the background via: `./ls_proxy 8080 localhost >$(date -Isec).log 2>&1 &`.

3. Configure DNS records (optional)
    - Since **LSProxy** acts as a [reverse proxy][], we might want to point our domain name to the address of the proxy.
    - When [testing other sites](#some-http-sites-to-try-out), we can create temporary DNS mappings via the [hosts][] file. For example, adding a line `127.0.0.1  www.wangafu.net` to `/etc/hosts` would do the trick for Linux.


## Some HTTP sites to try out
- Text only: http://www.wangafu.net/~nickm/libevent-book/
- With images: http://linux.vbird.org/
- Large files: http://free.nchc.org.tw/ubuntu-cd/
- Interactive: http://www.dailysudoku.com/sudoku/play.shtml?today=1


[reverse proxy]: https://en.wikipedia.org/wiki/Reverse_proxy
[hosts]: https://en.wikipedia.org/wiki/Hosts_(file)
