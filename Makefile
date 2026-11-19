CC ?= cc
CFLAGS ?= -O2
WARN = -std=c11 -Wall -Wextra
SRC = pong.c net.c dhcp.c link.c chat.c
HEADERS = net.h dhcp.h link.h font.h post.h chat.h

LOCAL_SDL := $(wildcard vendor/linux/usr/include/SDL2/SDL.h)
ifneq ($(LOCAL_SDL),)
SDL_CFLAGS ?= -Ivendor/linux/usr/include/SDL2 -Ivendor/linux/usr/include/$(shell $(CC) -dumpmachine)
SDL_LIBS ?= -l:libSDL2-2.0.so.0
GL_LIBS ?= -l:libGL.so.1
else
SDL_CFLAGS ?= $(shell sdl2-config --cflags)
SDL_LIBS ?= $(shell sdl2-config --libs)
GL_LIBS ?= -lGL
endif

WIN_CC ?= x86_64-w64-mingw32-gcc
WIN_SDL ?= $(firstword $(wildcard vendor/SDL2-*/x86_64-w64-mingw32))

.PHONY: all windows smoke test clean
all: PacketPong

PacketPong: $(SRC) $(HEADERS) Makefile
	$(CC) $(CPPFLAGS) $(SDL_CFLAGS) $(CFLAGS) $(WARN) $(SRC) -o $@ $(LDFLAGS) $(SDL_LIBS) $(GL_LIBS)

windows:
	test -f "$(WIN_SDL)/bin/SDL2.dll"
	mkdir -p build/windows
	$(WIN_CC) $(CPPFLAGS) -I"$(WIN_SDL)/include/SDL2" $(CFLAGS) $(WARN) $(SRC) -o build/windows/PacketPong.exe $(LDFLAGS) -L"$(WIN_SDL)/lib" -mwindows -lSDL2 -lopengl32 -lws2_32 -liphlpapi -lbcrypt -lshell32 -ladvapi32
	cp "$(WIN_SDL)/bin/SDL2.dll" build/windows/

smoke: PacketPong
	SDL_VIDEODRIVER=offscreen LIBGL_ALWAYS_SOFTWARE=1 ./PacketPong --smoke-test

test: build/dhcp-test build/chat-test
	./build/dhcp-test
	./build/chat-test

build/chat-test: tests/chat.c chat.c chat.h
	mkdir -p build
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) $(WARN) tests/chat.c chat.c -o $@ $(LDFLAGS)

build/dhcp-test: tests/dhcp.c dhcp.c dhcp.h
	mkdir -p build
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) $(WARN) tests/dhcp.c dhcp.c -o $@ $(LDFLAGS)

clean:
	rm -f PacketPong build/dhcp-test build/chat-test
	rm -rf build/windows
