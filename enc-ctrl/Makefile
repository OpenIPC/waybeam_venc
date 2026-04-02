SHELL := /bin/bash

HOST_CC      := cc
HOST_CFLAGS  := -std=c99 -Wall -Wextra -Werror -g -O0 -D_GNU_SOURCE \
                -Iinclude -I../tests

TEST_RUNNER  := tests/test_runner
TEST_SRCS    := tests/test_runner.c tests/test_ring_buffer.c \
                tests/test_scene_estimator.c tests/test_gop_controller.c \
                tests/test_enc_ctrl.c
LIB_SRCS     := src/ring_buffer.c src/scene_estimator.c \
                src/gop_controller.c src/venc_sdk.c src/enc_ctrl.c \
                src/telemetry.c

.PHONY: test test-asan test-tsan lint clean

test: $(TEST_RUNNER)
	./$(TEST_RUNNER)

$(TEST_RUNNER): $(TEST_SRCS) $(LIB_SRCS)
	$(HOST_CC) $(HOST_CFLAGS) $(TEST_SRCS) $(LIB_SRCS) -lpthread -o $@

test-asan:
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=address,undefined $(TEST_SRCS) $(LIB_SRCS) -lpthread -o $(TEST_RUNNER)
	./$(TEST_RUNNER)

test-tsan:
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=thread $(TEST_SRCS) $(LIB_SRCS) -lpthread -o $(TEST_RUNNER)
	./$(TEST_RUNNER)

lint:
	$(HOST_CC) $(HOST_CFLAGS) -fsyntax-only $(LIB_SRCS)

clean:
	rm -f $(TEST_RUNNER)
