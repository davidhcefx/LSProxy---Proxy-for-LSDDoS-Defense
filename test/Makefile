CPP := $(shell if command -v g++-10 >/dev/null 2>&1; then echo 'g++-10'; \
			elif command -v g++-9 >/dev/null 2>&1; then echo 'g++-9'; \
			else echo 'g++'; fi)
CPPVER := $(shell $(CPP) --version | sed -n -E '1s/[^0-9]*([0-9]+).*/\1/p')
STDVER := $(shell if [ $(CPPVER) -ge 10 ]; then echo 'c++20'; else echo 'c++2a'; fi)
CPPFLAGS := -Wall -Wextra -std=$(STDVER) -g -Og
SRC := _ls_proxy.cpp buffer.cpp client.cpp server.cpp connection.cpp
TEST := test_buffer test_reply_503


all: $(TEST)

test_buffer: $(SRC) test_buffer.cpp
	$(CPP) $(CPPFLAGS) -o $@ $^ ../src/llhttp/libllhttp.so -levent
	./$@

test_reply_503: $(SRC) test_reply_503.cpp
	$(CPP) $(CPPFLAGS) -o $@ $^ ../src/llhttp/libllhttp.so -levent
	./$@

_ls_proxy.cpp: ls_proxy.h
	# main function stripped
	sed '/^int main(int argc/Q' ../src/ls_proxy.cpp > $@

ls_proxy.h:
	# as requested by test_buffer.cpp
	sed -e 's/^\(#define HIST_CACHE_SIZE\).*/\1 17/' \
		-e 's/^\(#define SOCK_IO_BUF_SIZE\).*/\1 150/' \
		../src/ls_proxy.h > $@

%:
	cp ../src/$@.cpp .

clean:
	rm -f $(SRC) ls_proxy.h $(TEST)

.PHONY: all clean
