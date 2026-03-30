# Makefile for P2P project

CC = gcc
CFLAGS = -Wall -g

all: tracker peer

tracker: tracker.c
	$(CC) $(CFLAGS) tracker.c -o tracker

peer: peer.c
	$(CC) $(CFLAGS) peer.c -o peer -lpthread

clean:
	rm -f tracker peer *.o