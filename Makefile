CFLAGS = -g
LDLIBS = -lncurses

.PHONY: all clean

all: dinorun

clean:
	rm dinorun
	
dinorun: *.c

