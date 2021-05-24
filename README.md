# ls_proxy - Proxy for LSDDoS Defense




## Installation
1. Adjust parameters.
    - Adjust the macros defined in `ls_proxy.h` to your own needs.

2. Compile and run.
    ```sh
    make
    make test
    ./ls_proxy 8080 127.0.0.1 80
    ```

3. Configure DNS records. (optional)
    - Since **ls_proxy** acts as a reverse proxy, we might want to point our domain to the address of the proxy.
    - When [testing other sites](#some-http-sites-to-try-out), we can create temporary DNS mappings via the [hosts][] file. For example, adding a line `127.0.0.1  www.wangafu.net` to `/etc/hosts` would do the trick for Linux.


## Some HTTP sites to try out
- Text only: http://www.wangafu.net/~nickm/libevent-book/
- With images: http://linux.vbird.org/
- Large files: http://free.nchc.org.tw/ubuntu-cd/
- Interactive: http://www.dailysudoku.com/sudoku/play.shtml?today=1


[hosts]: https://en.wikipedia.org/wiki/Hosts_(file)