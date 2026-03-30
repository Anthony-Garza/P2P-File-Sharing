# P2P-File-Sharing
File sharing protocol built similarly to bit torrent protocol.


# Running Using make file

Go to:
> cd ~/P2P-File-Sharing/code

run:
> make

next:
> ./tracker

open a new terminal and run:
> ./peer createtracker movie1 1000 test abc123 127.0.0.1 4000

new terminal:
> ./peer createtracker movie1 1000 test abc123 127.0.0.1 4000

new terminal:
> ./peer createtracker movie1 1000 test abc123 127.0.0.1 4000

    list and update:
    > ./peer list
    > ./peer get movie1.track
    > ./peer updatetracker movie1 0 1000 127.0.0.1 4000
    

# How to RUN, Mid Term

Move to code folder and run this: 
> gcc tracker.c -o tracker 
> gcc peer.c -o peer -lpthread

Open a terminal
> move to code folder
> run: ./tracker

Open another terminal
    Create a tracker
> move to code folder
> run: ./peer createtracker movie1 1000 test abc123 127.0.0.1 4000

In a third terminal
    Create tracker
	> run: ./peer createtracker movie2 2000 test2 xyz789 127.0.0.1 4001

In a fourth terminal 
    list trackers:
> run: ./peer list

    update trackers:
> ./peer updatetracker movie1 0 1000 127.0.0.1 4002
> ./peer updatetracker movie2 0 2000 127.0.0.1 4002
