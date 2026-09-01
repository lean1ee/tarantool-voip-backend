#
# Dynamic Sanitizers & Static Analysis Makefile for Tarantool C Drivers
#

CC ?= gcc
CLANG ?= clang
CFLAGS = -Wall -Wextra -Wpedantic -g -O1 -fno-omit-frame-pointer
SANFLAGS = -fsanitize=address,undefined,leak

INCLUDES = -I../../rtpengine_tarantool/include \
           -I../../opensips_tarantool/cachedb_tarantool

all: test_rtpe_asan test_opensips_asan

test_rtpe_asan: test_rtpengine_asan.c ../../rtpengine_tarantool/src/tarantool.c
	$(CC) $(CFLAGS) $(SANFLAGS) $(INCLUDES) $^ -lpthread -o $@

test_opensips_asan: test_opensips_asan.c ../../opensips_tarantool/cachedb_tarantool/cachedb_tarantool_dbase.c
	$(CC) $(CFLAGS) $(SANFLAGS) $(INCLUDES) $^ -lpthread -o $@

test: all
	@echo "\n>>> RUNNING RTPEngine ASan / UBSan Dynamic Memory Checks <<<"
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1 ./test_rtpe_asan
	@echo "\n>>> RUNNING OpenSIPS CacheDB ASan / UBSan Dynamic Memory Checks <<<"
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1 ./test_opensips_asan

clean:
	rm -f test_rtpe_asan test_opensips_asan *.o
