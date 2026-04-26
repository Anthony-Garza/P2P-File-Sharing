How to run demo step by step

1. ensure scripts are executable
	chmod +x setup_peers.sh start_demo.sh

2. clean old files (if any), compiles peer code, and builds 13 peer files
	./setup_peers.sh

3. build server executable
	make tracker

4. run demo
	./start_demo.sh


What happens after running?
Time 0s: Tracker starts. Peer1 creates 30B size file and Peer2 creates 5MB large file and bothr egister to server.

Time 30s: Peers 3-8 wake up and requests the file list and begin download in 1024 byte chunks

Time 90s: Peers 9-13 wake up and joins the download. Peer1 and Peer2 are terminated

Completion: Remaining continue sharing chunks until all are downloaded.

Once completed, "Success! MD5 matched" message will be shown at bottom signifying completion. 
To verify run this command and you would see Movie1 and Movie2 inside.
	- ls -lh peer13/shared13/
