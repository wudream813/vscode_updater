# Windows build with MinGW-w64; the portable tests also run on Linux/macOS.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDFLAGS  ?= -static
LIBS      = -lwininet -lbcrypt -lshell32
TARGET    = vscode_updater.exe
HEADERS   = updater_core.hpp miniz.c miniz.h json.hpp

all: $(TARGET)

$(TARGET): update.cpp $(HEADERS) Makefile
	$(CXX) $(CXXFLAGS) update.cpp $(LDFLAGS) $(LIBS) -o $@

core-tests.exe: tests/core_tests.cpp updater_core.hpp
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

windows-tests.exe: tests/windows_tests.cpp update.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

test: core-tests.exe
	./core-tests.exe

clean:
	rm -f $(TARGET) core-tests.exe windows-tests.exe *.obj *.o

.PHONY: all test clean
