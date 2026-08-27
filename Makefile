CC      = gcc
CFLAGS  = -Wall -Wextra -O2
TARGET  = onenet_wifi

SRCS    = $(shell find src -name '*.c')
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

run: $(TARGET)
	sudo ./$^

.PHONY: all clean run
