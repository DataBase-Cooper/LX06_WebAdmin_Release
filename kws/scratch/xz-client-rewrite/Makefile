CC ?= gcc
CPPFLAGS += -Isrc -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -O2 -g
CFLAGS += -Wall -Wextra -Werror
LDLIBS += -lopus -lssl -lcrypto -lm -lpthread

BUILD := build
TEST_BUILD := $(BUILD)/tests

CLIENT_SOURCES := \
src/aec.c src/event.c src/home_mcp.c src/identity.c src/json.c src/main.c src/opus_codec.c src/player.c \
src/protocol.c src/session.c src/talk.c src/task_poll.c src/text_match.c src/timeline.c src/ws.c src/ws_frame.c \
../../lx06-installer/runtime/common/control_asr.c

UNIT_TESTS := test_json test_protocol test_session test_event test_timeline test_aec \
	test_player test_opus_codec test_text_match test_ws_frame test_identity test_talk test_input_gain test_home_mcp
INTEGRATION_BINARY := $(TEST_BUILD)/test_ws_client

.PHONY: all test armhf-package-test clean

all: $(BUILD)/xz_client

$(BUILD) $(TEST_BUILD):
	mkdir -p $@

$(BUILD)/xz_client: $(CLIENT_SOURCES) src/home_mcp.h src/input_gain.h src/task_poll.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_SOURCES) -o $@ $(LDLIBS)

$(TEST_BUILD)/test_json: src/json.c tests/test_json.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_BUILD)/test_protocol: src/json.c src/protocol.c tests/test_protocol.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_BUILD)/test_home_mcp: src/home_mcp.c src/json.c src/protocol.c tests/test_home_mcp.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DXZ_HOME_MCP_APPS='"."' $^ -o $@

$(TEST_BUILD)/test_session: src/session.c tests/test_session.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_BUILD)/test_event: src/json.c src/event.c tests/test_event.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_BUILD)/test_timeline: src/timeline.c tests/test_timeline.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_BUILD)/test_aec: src/aec.c tests/test_aec.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_BUILD)/test_input_gain: tests/test_input_gain.c src/input_gain.h | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_input_gain.c -o $@ -lm

$(TEST_BUILD)/test_player: src/player.c tests/test_player.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lpthread

$(TEST_BUILD)/test_opus_codec: src/opus_codec.c tests/test_opus_codec.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lopus

$(TEST_BUILD)/test_text_match: src/text_match.c tests/test_text_match.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_BUILD)/test_ws_frame: src/ws_frame.c tests/test_ws.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_BUILD)/test_identity: src/identity.c tests/test_identity.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_BUILD)/test_talk: src/talk.c tests/test_talk.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_BUILD)/test_ws_client: src/ws.c src/ws_frame.c tests/test_ws_client.c | $(TEST_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lssl -lcrypto

test: all $(addprefix $(TEST_BUILD)/,$(UNIT_TESTS)) $(INTEGRATION_BINARY)
	@set -e; for test in $(addprefix $(TEST_BUILD)/,$(UNIT_TESTS)); do $$test; done
	@XZ_WS_TEST_BINARY=$(CURDIR)/$(INTEGRATION_BINARY) python3 tests/test_ws_integration.py
	@XZ_CLIENT_BINARY=$(CURDIR)/$(BUILD)/xz_client python3 tests/test_runtime_integration.py
	@python3 tests/test_idle_runtime.py
	@python3 tests/test_task_poll_runtime.py
	@python3 tests/test_tts_overlap_runtime.py
	@python3 tests/test_dual_barge.py
	@python3 tests/test_playback_tail_runtime.py

armhf-package-test:
	@bash scripts/build-armhf.sh
	@python3 tests/test_package_lx06.py

clean:
	rm -rf $(BUILD)
