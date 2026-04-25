#!/bin/bash

# 1. Compile the code first so we have a fresh executable
make peer

# 2. Loop to create 13 peers
for i in {1..13}
do
    # Create the peer folder and its specific shared folder
    FOLDER="peer$i"
    mkdir -p $FOLDER/shared$i
    
    # Copy the compiled peer program into the folder
    cp peer $FOLDER/
    
    # --- Create UNIQUE serverThreadConfig.cfg ---
    # We start at port 4000 and add $i (Peer 1 = 4001, Peer 2 = 4002, etc.)
    PORT=$((4000 + i))
    echo "$PORT" > $FOLDER/serverThreadConfig.cfg
    echo "shared$i/" >> $FOLDER/serverThreadConfig.cfg
    
    # --- Create STANDARD clientThreadConfig.cfg ---
    # Everyone talks to the same tracker (Port 3490, localhost, 900s interval)
    echo "3490" > $FOLDER/clientThreadConfig.cfg
    echo "127.0.0.1" >> $FOLDER/clientThreadConfig.cfg
    echo "900" >> $FOLDER/clientThreadConfig.cfg

    echo "Created $FOLDER with Port $PORT"
done

echo "All 12 peer folders are ready!"
