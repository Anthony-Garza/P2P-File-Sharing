#!/usr/bin/env bash

# pre test cleanup
pkill -f tracker
pkill -f peer

# make sure required root folders exist
mkdir -p torrents
rm -f torrents/*.track

# makesure seed folders exist before writing dummy files to them
mkdir -p peer1/shared1 peer2/shared2

echo "Time 0: Starting server and initial seeds..."
./tracker &
sleep 2

# mac or linux
OS_TYPE=$(uname)

# Create movie1 (30 bytes)
echo -n "123456789012345678901234567890" > peer1/shared1/movie1

if [ "$OS_TYPE" == "Darwin" ]; then
    # macOS execution
    mkfile -n 5242880 peer2/shared2/movie2
    MD1=$(md5 -q peer1/shared1/movie1)
    MD2=$(md5 -q peer2/shared2/movie2)
else
    # Linux execution
    dd if=/dev/zero of=peer2/shared2/movie2 bs=1M count=5 status=none
    MD1=$(md5sum peer1/shared1/movie1 | awk '{ print $1 }')
    MD2=$(md5sum peer2/shared2/movie2 | awk '{ print $1 }')
fi

# START SEEDS
# Peer 1 (Small file)
echo "Peer1: createtracker movie1 30 Small_File $MD1 127.0.0.1 4001"
(cd peer1 && ./peer createtracker movie1 30 "Small_File" $MD1 127.0.0.1 4001) &

# Peer 2 (Large file)
echo "Peer2: createtracker movie2 5242880 Large_File $MD2 127.0.0.1 4002"
(cd peer2 && ./peer createtracker movie2 5242880 "Large_File" $MD2 127.0.0.1 4002) &

sleep 30

# START WAVE 1
echo "Time 30s: Starting Peers 3 to 8..."
for i in {3..8}; do
    echo "Peer$i: List"
    echo "Peer$i: Get movie1.track"
    echo "Peer$i: Get movie2.track"
    (cd "peer$i" && ./peer list && ./peer get movie1.track && ./peer get movie2.track) &
done

sleep 60

# TERMINATE SEEDS
echo "Time 1m 30s: Terminating Seeds..."
pkill -f "peer1/peer"
pkill -f "peer2/peer"
echo "Peer1 terminated"
echo "Peer2 terminated"

#  START WAVE 2
echo "Time 1m 30s: Starting Peers 9 to 13..."
for i in {9..13}; do
    echo "Peer$i: List"
    echo "Peer$i: Get movie1.track"
    echo "Peer$i: Get movie2.track"
    (cd "peer$i" && ./peer list && ./peer get movie1.track && ./peer get movie2.track) &
done

echo "Demo script complete. Watch terminal for the final P2P chunk transfers."
