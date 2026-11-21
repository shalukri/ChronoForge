# ChronoForge build. Requires g++ >= 11 or clang >= 14 (C++20: <bit>, std::bit_cast).
CXX      ?= g++
BUILD    ?= build
OPT      ?= -O2
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
CXXFLAGS ?= -std=c++20 $(OPT) $(WARN) -pthread
INC      := -Iinclude -Isrc

# Strict -Wconversion applies to the library; demo/test drivers do lots of benign size_t->double math.
$(BUILD)/src/main.o $(BUILD)/tests/test_suite.o: WARN_EXTRA := -Wno-conversion

CORE_SRC := src/models/label_set.cpp src/algorithms/gorilla_codec.cpp \
            src/services/inverted_index.cpp src/services/cardinality_guard.cpp \
            src/services/storage_engine.cpp src/services/query_engine.cpp
CORE_OBJ := $(CORE_SRC:%.cpp=$(BUILD)/%.o)

.PHONY: all run test sanitize tsan clean
all: $(BUILD)/chronoforge $(BUILD)/chronoforge_tests

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(WARN_EXTRA) $(INC) -MMD -MP -c $< -o $@

$(BUILD)/chronoforge: $(CORE_OBJ) $(BUILD)/src/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/chronoforge_tests: $(CORE_OBJ) $(BUILD)/tests/test_suite.o
	$(CXX) $(CXXFLAGS) $^ -o $@

run: $(BUILD)/chronoforge
	./$(BUILD)/chronoforge

test: $(BUILD)/chronoforge_tests
	./$(BUILD)/chronoforge_tests

# ASan + UBSan: catches out-of-bounds in the bit reader / decoder and signed-overflow bugs.
sanitize:
	$(MAKE) BUILD=build/asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" test

# TSan: validates the locking protocol (double-checked series creation, shard striping).
tsan:
	$(MAKE) BUILD=build/tsan OPT="-O1 -g -fsanitize=thread" test

clean:
	rm -rf build

-include $(CORE_OBJ:.o=.d)
