CC = gcc
LD = ld


CFLAGS = -fno-builtin-strlen -fno-builtin-bcopy -fno-builtin-bzero

#-fomit-frame-pointer
CCOPTS = -Wall -O1 -c -fno-builtin -fno-stack-protector -fno-defer-pop \
		 -m32

TEST_OBJS = testFake.o shellutilFake.o utilFake.o fsFake.o blockFake.o

all:  p6_test


p6_test: $(TEST_OBJS)
	$(CC) -o p6_test $(TEST_OBJS)

shellFake.o : shell.c
	$(CC) -Wall $(CFLAGS) -g -c -DFAKE -o shellFake.o shell.c

testFake.o : p6_test.c
	$(CC) -Wall $(CFLAGS) -g -c -DFAKE -o testFake.o p6_test.c

shellutilFake.o : shellutilFake.c
	$(CC) -Wall $(CFLAGS) -g -c -DFAKE -o shellutilFake.o shellutilFake.c

blockFake.o : blockFake.c
	$(CC) -Wall $(CFLAGS) -g -c -DFAKE -o blockFake.o blockFake.c

utilFake.o : util.c
	$(CC) -Wall $(CFLAGS) -g -c -DFAKE -o utilFake.o util.c

fsFake.o : fs.c
	$(CC) -Wall $(CFLAGS) -g -c -DFAKE -o fsFake.o fs.c

# Clean up!
clean:
	rm -f *.o
	rm -f p6_test
	rm -f disk

