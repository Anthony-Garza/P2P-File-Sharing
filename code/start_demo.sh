#!/bin/bash

pkill -f tracker
pkill -f peer

mkdir -p torrents
rm -f torrents/*.track

echo "Time 0: Starting server and initial seeds..."
./tracker &
sleep 2

# Create the real files! (Using echo -n to make it exactly 30 bytes)
echo -n "123456789012345678901234567890" > peer1/shared1/movie1
mkfile -n 5m peer2/shared2/movie2

# Grab the REAL MD5 hashes!
MD1=$(md5 -q peer1/shared1/movie1)
MD2=$(md5 -q peer2/shared2/movie2)

# Peer 1 (Small file)
echo "Peer1: createtracker movie1 30 Small_File $MD1 127.0.0.1 4001"
(cd peer1 && ./peer createtracker movie1 30 "Small_File" $MD1 127.0.0.1 4001) &

# Peer 2 (Large file) - 5m is exactly 5242880 bytes!
echo "Peer2: createtracker movie2 5242880 Large_File $MD2 127.0.0.1 4002"
(cd peer2 && ./peer createtracker movie2 5242880 "Large_File" $MD2 127.0.0.1 4002) &

sleep 30

echo "Time 30s: Starting Peers 3 to 8..."
for i in {3..8}; do
    echo "Peer$i: List"
    echo "Peer$i: Get movie1.track"
    echo "Peer$i: Get movie2.track"
    (cd peer$i && ./peer list && ./peer get movie1.track && ./peer get movie2.track) &
done

sleep 60

echo "Time 1m 30s: Terminating Seeds..."
pkill -f "peer1/peer"
pkill -f "peer2/peer"
echo "Peer1 terminated"
echo "Peer2 terminated"

echo "Time 1m 30s: Starting Peers 9 to 13..."
for i in {9..13}; do
    echo "Peer$i: List"
    echo "Peer$i: Get movie1.track"
    echo "Peer$i: Get movie2.track"
    (cd peer$i && ./peer list && ./peer get movie1.track && ./peer get movie2.track) &
done

echo "Demo script complete. Watch the terminal for the final P2P chunk transfers!"
