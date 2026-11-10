CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-variable
CPPFLAGS ?= -Isrc

TARGET := steamvr_battery_indicator
SOURCES := src/main.cpp
HEADERS := src/runtime.h src/image.h src/indicator.h src/font8x8.h src/openvr_capi.h

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Darwin)
    LDLIBS :=
else
    LDLIBS := -ldl
endif

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $(TARGET) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
