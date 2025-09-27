# Compiler to use
CC = gcc

# Compiler flags: -g for debugging, -Wall for all warnings
CFLAGS = -g -Wall

# The target executable name
TARGET = myshell

# The source files
SOURCES = main.c utils.c

# Default rule that builds the target
all: $(TARGET)

# Rule to link the object files into the final executable
$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

# Rule to clean up the directory (remove executable)
clean:
	rm -f $(TARGET) test1.txt test2.txt