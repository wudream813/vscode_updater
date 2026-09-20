# VS Code 自动更新程序 - Makefile (MinGW-w64)
# 用法:
#   make          编译生成 vscode_updater.exe
#   make clean    清理编译产物
# 需要: MinGW-w64 g++（winlibs / MSYS2 / mingw-builds）
# 提示: 若报 "undefined reference to std::filesystem"，说明编译器较老，
#       请在 LDFLAGS 中追加 -lstdc++fs

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall
LDFLAGS  ?= -static
LIBS      = -lwininet

TARGET    = vscode_updater.exe
SOURCES   = update.cpp

all: $(TARGET)

$(TARGET): $(SOURCES) miniz.c miniz.h json.hpp Makefile
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LIBS) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
