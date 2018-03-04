OBJS   =  concurrent latency sharing testlock treelock lrubench testmw
LD     =  $(CC)
CFLAGS = -O3 -fomit-frame-pointer -Wall -W -Wextra

all: $(OBJS) atomic.o
clean:
	rm -f  $(OBJS) *.o *~ core

$(OBJS):%: %.c
	$(CC) -I.. $(CFLAGS) -o $@ $^ -lpthread

%.o: %.c
	$(CC) -I.. $(CFLAGS) -c $^
