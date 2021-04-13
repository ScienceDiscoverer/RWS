TARGET_EXEC := rws

BUILD_DIR := ./build
SRC_DIRS := ./

# Find all C++ files we want to compile
SRCS := $(shell find $(SRC_DIRS) -name '*.cpp')

# String substitution for every C++ file.
# As an example, hello.cpp turns into ./build/hello.cpp.o
OBJS := $(SRCS:%=$(BUILD_DIR)/%.o)

# String substitution (suffix version without %).
# As an example, ./build/hello.cpp.o turns into ./build/hello.cpp.d
DEPS := $(OBJS:.o=.d)

# The -MMD and -MP flags together generate .d dependencies files,
# this means there is no need to manually add all header files into makefile
# CPPFLAGS := $(INC_FLAGS) -g -MMD -MP -fno-exceptions -fno-rtti -Wall -Wno-parentheses # debug build
CPPFLAGS := $(INC_FLAGS) -MMD -MP -fno-exceptions -fno-rtti -Wall -Wno-parentheses -DNDEBUG
LDFLAGS := -lwiringPi -lpthread -lm -lstdc++

# The final build step.
$(BUILD_DIR)/$(TARGET_EXEC): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Build step for C++ source
$(BUILD_DIR)/%.cpp.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -r $(BUILD_DIR)

# Include the .d makefiles.
# The - at the front suppresses the errors of missing Makefiles dependencies.
-include $(DEPS)