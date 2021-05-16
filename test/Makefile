CPP := $(shell if command -v g++-10 >/dev/null 2>&1; then echo 'g++-10'; \
			elif command -v g++-9 >/dev/null 2>&1; then echo 'g++-9'; \
			else echo 'g++'; fi)
CPPVER := $(shell $(CPP) --version | sed -n -E '1s/[^0-9]*([0-9]+).*/\1/p')
STDVER := $(shell if [ $(CPPVER) -ge 10 ]; then echo 'c++20'; else echo 'c++2a'; fi)
CPPFLAGS := -Wall -Wextra -std=$(STDVER) -g -Og
SRC := ls_proxy.cpp buffer.cpp client.cpp server.cpp connection.cpp helper.cpp
OBJ := $(SRC:.cpp=.o)
TEST := test_buffer test_reply_503 test_response test_bugs


all: $(TEST)
	for t in $(TEST); do \
		./$$t || break; \
	done

test_%: test_%.o $(OBJ)
	$(CPP) $(CPPFLAGS) -o $@ $^ ../src/llhttp/libllhttp.so -levent

%.o: %.cpp ls_proxy.h helper.h
	$(CPP) -c $(CPPFLAGS) -o $@ $< -levent

# terminal rule (prerequisites are files)
%.cpp:: ../src/%.cpp
	cp $^ $@

ls_proxy.cpp: ../src/ls_proxy.cpp
	# strip main function
	sed '/^int main(int argc/Q' $^ > $@

ls_proxy.h: test_buffer.cpp
	# replace header with contents in test_buffer.cpp
	replace_line() { \
		line=$$(sed -n "/$$1/p" test_buffer.cpp | sed 's/[\/&]/\\&/g'); \
		sed -i "s/$$1/$$line/" $@; \
	}; \
	cp ../src/ls_proxy.h $@ && \
	replace_line '^#include.*llhttp\/llhttp\.h"' && \
	replace_line '^#define HIST_CACHE_SIZE.*' && \
	replace_line '^#define SOCK_IO_BUF_SIZE.*'

clean:
	rm -f ls_proxy.h $(SRC) *.o $(TEST) core.*

.PHONY: all clean
.PRECIOUS: %.o
