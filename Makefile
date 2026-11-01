CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L
INC     := -Icomponents/hermes_core/include -Icomponents/hermes_core/src
CORE    := components/hermes_core/src

.PHONY: all test clean

all: build/hermes-lite

build:
	mkdir -p build

build/core_%.o: $(CORE)/%.c | build
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

build/posix.o: $(CORE)/platform_posix.c | build
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

build/main.o: examples/linux/main.c | build
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

build/hermes-lite: build/core_json.o build/core_agent.o build/core_skills.o \
                   build/core_history.o build/posix.o build/main.o | build
	$(CC) $^ -o $@ -lcurl

build/test_json: $(CORE)/json.c test/test_json.c | build
	$(CC) $(CFLAGS) $(INC) $^ -o $@

build/test_agent: $(CORE)/json.c $(CORE)/agent.c $(CORE)/skills.c $(CORE)/history.c \
                  $(CORE)/platform_posix.c test/test_agent.c | build
	$(CC) $(CFLAGS) $(INC) $^ -o $@ -lcurl

test: build/test_json build/test_agent
	./build/test_json
	./build/test_agent

clean:
	rm -rf build
