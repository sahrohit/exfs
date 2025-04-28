all:
	gcc main.c -o exfs2

reset:
	rm -f dataseg{0..100} inodeseg{0..100}

clean:
	rm -f exfs2 dataseg{0..100} inodeseg{0..100}

# # Birat
# CC = gcc
# CFLAGS = -Wall -Werror -g -std=c11
# LDFLAGS =
# TARGET = exfs2

# SRCS = birat.c
# OBJS = $(SRCS:.c=.o)

# .PHONY: all clean

# all: $(TARGET)

# $(TARGET): $(OBJS)
# 	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)

# %.o: %.c
# 	$(CC) $(CFLAGS) -c $< -o $@

# clean:
# 	rm -f $(OBJS) $(TARGET) *.seg core *.stackdump