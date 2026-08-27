CC      = gcc
CFLAGS  = -Wall -Wextra -O2
TARGET  = esp01s_wifi
SRC     = esp01s_wifi.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

run: $(TARGET)
	sudo ./$^ 0

.PHONY: all clean run
