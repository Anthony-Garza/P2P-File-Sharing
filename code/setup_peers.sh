#!/usr/bin/env bash

echo "--- Starting P2P Environment Setup ---"

# 1. Compile the code
make clean
if ! make peer; then
    echo "ERROR: Compilation failed. Please check your C code or Makefile."
    exit 1
fi

# 2. Loop to create 13 peers
for i in {1..13}; do
    FOLDER="peer$i"
    
    # Create the peer folder and specific shared folder
    mkdir -p "$FOLDER/shared$i"
    
    # Copy compiled peer program into folder
    if [ -f "peer" ]; then
        cp peer "$FOLDER/"
    fi
    
    # Create unique serverThreadConfig.cfg for each
    PORT=$((4000 + i))
    {
        echo "$PORT"
        echo "shared$i/"
    } > "$FOLDER/serverThreadConfig.cfg"
    
    # Create one clientThreadConfig.cfg for all
    {
        echo "3490"
        echo "127.0.0.1"
        echo "900"
    } > "$FOLDER/clientThreadConfig.cfg"

    echo "Created $FOLDER with Port $PORT"
done

echo "All 13 peer folders are built, configured, and ready."
