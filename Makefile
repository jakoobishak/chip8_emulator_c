CFLAGS=-std=c17 -Wall -Wextra -Werror
LIBS=.\SDL2-2.26.2\x86_64-w64-mingw32\lib -lmingw32 -lSDL2main -lSDL2
INCLUDES=.\SDL2-2.26.2\x86_64-w64-mingw32\include\SDL2

all:
	gcc chip8.c -o chip8 $(CFLAGS) -L$(LIBS) -I$(INCLUDES) 