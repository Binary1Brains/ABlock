# ABlock - Wayland screen locker using ext-session-lock-v1
.POSIX:

PROG      = ABlock
SRCS      = ABlock.c ext-session-lock-v1-client-protocol.c
OBJS      = $(SRCS:.c=.o)
CFLAGS    = -std=gnu99 -Wall -Wextra -O2 -D_GNU_SOURCE
LDLIBS    = -lwayland-client -lxkbcommon -lpam -lm
CPPFLAGS  = -D_XOPEN_SOURCE=700

# *********Check comments******
# Path to wayland-protocols adjust if needed generally not 
PROTOCOLS_DIR = /usr/share/wayland-protocols
LOCK_XML     = $(PROTOCOLS_DIR)/staging/ext-session-lock/ext-session-lock-v1.xml

all: $(PROG)

# Generate protocol C code and header if XML is present
ext-session-lock-v1-client-protocol.h: $(LOCK_XML)
	wayland-scanner client-header $(LOCK_XML) $@

ext-session-lock-v1-client-protocol.c: $(LOCK_XML)
	wayland-scanner private-code $(LOCK_XML) $@

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

clean:
	rm -f $(PROG) $(OBJS) ext-session-lock-v1-client-protocol.h ext-session-lock-v1-client-protocol.c

install: $(PROG)
	install -Dm755 $(PROG) $(DESTDIR)/usr/local/bin/$(PROG)

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(PROG)

.PHONY: all clean install uninstall
