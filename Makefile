DISTRO := $(shell sed -n -E 's/^NAME="(.*)"/\1/p' /etc/*release)
OPTS := -Wall -Wextra -std=gnu++20 # -O2

all:
	echo $(DISTRO)

attack:
	g++ $(OPTS) src/low_slow_attack.cpp -o low_slow_attack

defense: libevent rlimit_nofile
	g++ $(OPTS) -levent src/low_slow_defense.cpp -o low_slow_defense

libevent:
	case "$(DISTRO)" in \
	*Ubuntu*|*Debian*) \
		sudo apt-get update && sudo apt-get install -y libevent-dev;; \
	*Fedora*) \
		sudo dnf install -y libevent-devel.x86_64;; \
	*CentOS*) \
		sudo yum install -y libevent-devel.x86_64;; \
	*) \
		echo "Please install libevent-dev manually for your platform.";; \
	esac

rlimit_nofile:
	bash raise_nofile_limit.sh

clean:
	rm -f low_slow_attack low_slow_defense
