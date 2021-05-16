DISTRO := $(shell sed -n -E 's/^NAME="(.*)"/\1/p' /etc/*release)
SUDO := sudo
CPP := $(shell if command -v g++-10 >/dev/null 2>&1; then echo 'g++-10'; \
            elif command -v g++-9 >/dev/null 2>&1; then echo 'g++-9'; \
            else echo 'g++'; fi)
CPPVER := $(shell $(CPP) --version | sed -n -E '1s/[^0-9]*([0-9]+).*/\1/p')
STDVER := $(shell if [ $(CPPVER) -ge 10 ]; then echo 'c++20'; else echo 'c++2a'; fi)
CPPFLAGS := -Wall -Wextra -std=$(STDVER) -g -Og
#CPPFLAGS := -Wall -Wextra -std=$(STDVER) -O2


all: g++9 libevent raise_limit simple_attack ls_proxy

simple_attack: src/simple_attack.cpp
	$(CPP) $(CPPFLAGS) -o $@ $^

ls_proxy: src/ls_proxy.cpp src/buffer.cpp src/client.cpp src/server.cpp src/connection.cpp
	$(CPP) $(CPPFLAGS) -o $@ $^ src/llhttp/libllhttp.so -levent

g++9:
	# check if g++-9 or above has been installed
	if [ $(CPPVER) -lt 9 ]; then \
		case "$(DISTRO)" in \
		*Ubuntu*|*Debian*) \
			# install 'add-apt-repository' \
			$(SUDO) apt update && $(SUDO) apt-get install -y software-properties-common && \
			$(SUDO) add-apt-repository -y ppa:ubuntu-toolchain-r/test && $(SUDO) apt-get update && \
			($(SUDO) apt-get install -y g++-10 || $(SUDO) apt-get install -y g++-9) ;; \
		*Fedora*) \
			$(SUDO) dnf install -y gcc-c++ ;; \
		*CentOS*) \
			$(SUDO) yum install -y gcc-toolset-9-gcc-c++ && \
			update-alternatives --install /usr/bin/g++-9 g++-9 /opt/rh/gcc-toolset-9/root/usr/bin/g++ 10 ;; \
		*) \
			 echo "Please install g++-9 or later manually in order to support c++20." ;; \
		esac \
	fi

libevent:
	# check if libevent has been installed
	if [ -z "`ldconfig -p | grep libevent`" ]; then \
		case "$(DISTRO)" in \
		*Ubuntu*|*Debian*) \
			$(SUDO) apt-get update && $(SUDO) apt-get install -y libevent-dev ;; \
		*Fedora*) \
			$(SUDO) dnf install -y libevent-devel.x86_64 ;; \
		*CentOS*) \
			$(SUDO) yum install -y libevent-devel.x86_64 ;; \
		*) \
			echo "Please install libevent-dev manually on your platform." ;; \
		esac \
	fi

raise_limit:
	./utils/raise_nofile_limit.sh

test:
	make -C test all

clean:
	rm -f simple_attack ls_proxy

.PHONY: all test clean raise_limit gnu++2a libevent
