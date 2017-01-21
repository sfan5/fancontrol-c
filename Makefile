CFLAGS = -pipe -std=c99 -Wall -O2
LDFLAGS =

PREFIX ?= /usr

OBJ = main.o

all: fancontrol-c

fancontrol-c: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

obj/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ)

install:
	install -pDm755 fi6s $(DESTDIR)$(PREFIX)/sbin/fancontrol-c

.PHONY: clean
