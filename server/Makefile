# Default compiler and flags
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror
LDFLAGS ?= -lpthread
TARGET ?= aesdsocket
SRC ?= aesdsocket.c
OBJ ?= $(SRC:.c=.o)

# Default target: builds the "aesdsocket" application
all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ) $(LDFLAGS)

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target: removes the binary and all object files
clean:
	rm -f $(TARGET) $(OBJ)

.PHONY: all clean
