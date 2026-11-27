# CS 3404 Capstone: webshell
# Student: Christian Dennis (3729509)
#
#   make          build ./webshell with the required strict flags and sanitizers
#   make release  build ./webshell-release (-O2, no sanitizers) for benchmarking
#   make test     build, then run the automated test suite
#   make clean    remove build output

CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -pthread
LDFLAGS  := -fsanitize=address,undefined -pthread

RELEASE_CFLAGS  := -std=c11 -Wall -Wextra -Werror -pedantic -O2 -g -pthread
RELEASE_LDFLAGS := -pthread

SRCS        := $(wildcard src/*.c)
OBJS        := $(SRCS:src/%.c=build/debug/%.o)
RELEASE_OBJS := $(SRCS:src/%.c=build/release/%.o)

.PHONY: all release test clean

all: webshell

webshell: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

webshell-release: $(RELEASE_OBJS)
	$(CC) $(RELEASE_OBJS) $(RELEASE_LDFLAGS) -o $@

release: webshell-release

build/debug/%.o: src/%.c | build/debug
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

build/release/%.o: src/%.c | build/release
	$(CC) $(RELEASE_CFLAGS) -MMD -MP -c $< -o $@

build/debug build/release:
	mkdir -p $@

test: webshell
	python3 -m unittest discover -s tests -p 'test_*.py' -v

clean:
	rm -rf build webshell webshell-release

-include $(OBJS:.o=.d) $(RELEASE_OBJS:.o=.d)
