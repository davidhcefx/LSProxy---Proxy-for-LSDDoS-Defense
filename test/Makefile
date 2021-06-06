CPP := $(shell if command -v g++-10 >/dev/null 2>&1; then echo 'g++-10'; \
			elif command -v g++-9 >/dev/null 2>&1; then echo 'g++-9'; \
			else echo 'g++'; fi)
CPPVER := $(shell $(CPP) --version | sed -n -E '1s/[^0-9]*([0-9]+).*/\1/p')
STDVER := $(shell if [ "$(CPPVER)" -ge 10 ]; then echo 'c++20'; else echo 'c++2a'; fi)
CPPFLAGS := -Wall -Wextra -std=$(STDVER) -g -Og
OUTER_SRC := ls_proxy.cpp buffer.cpp client.cpp server.cpp connection.cpp
OUTER_OBJ := $(OUTER_SRC:.cpp=.o)
OUTER_H := $(OUTER_SRC:.cpp=.h)
TEST := test_buffer test_reply_503 test_response test_bugs
OBJ := $(OUTER_OBJ) $(addsuffix .o, $(TEST)) helper.o


all: $(TEST)
	for t in $(TEST); do \
		./$$t || break; \
	done

test_%: test_%.o $(OUTER_OBJ) helper.o
	$(CPP) $(CPPFLAGS) -o $@ $^ ../src/llhttp/libllhttp.so -levent

$(OBJ): %.o: %.cpp $(OUTER_H) helper.h
	$(CPP) -c $(CPPFLAGS) -o $@ $< -levent

# terminal rule (prerequisites are files)
%.cpp:: ../src/%.cpp
	cp $^ $@

ls_proxy.cpp: ../src/ls_proxy.cpp
	# strip main function
	sed '/^int main(int argc/Q' $^ > $@

%.h:: ../src/%.h
	cp $^ $@

ls_proxy.h: helper.h
	# replace header with contents in helper.h
	cp ../src/ls_proxy.h $@; \
	sed '0,/@adjust/d;p' $^ | sed '/@adjust/Q' | while read -r line; do \
		line_escaped=$$(echo "$$line" | sed 's/[\/&]/\\&/g'); \
		case "$$line" in \
		'#define'*) \
			pattern=$$(echo "$$line" | sed -E 's/^([^ ]+ +[^ ]+).*/\1/' | \
			           sed -E 's/([]\/^$.*]|\[)/\\&/g'); \
			sed -i "s/^$$pattern.*/$$line_escaped/" $@ ;; \
		'#include'*'/llhttp.h"'*) \
			sed -i "s/^#include.*\/llhttp\.h\"/$$line_escaped/" $@ ;; \
		*) \
			echo "Unhandled header: $$line"; exit 1 ;; \
		esac \
	done

clean:
	rm -f $(OUTER_SRC) $(OBJ) $(OUTER_H) $(TEST) core*

.PHONY: all clean
.PRECIOUS: %.o
