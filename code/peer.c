#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#define MAXLINE  2048
#define BACKLOG  5
#define CHUNK_BUFSIZE 1024

struct ChunkTask {
    char filename[256];
    char peer_ip[64];
    int peer_port;
    long start_byte;
    long end_byte;
};

void start_multithreaded_download(char *trackfile_name);

// filled from config files on startup
int  tracker_port    = 3490;
char tracker_ip[64]  = "127.0.0.1";
int  update_interval = 900;   // how often we ping the tracker (seconds)

int  my_listen_port  = 4000;
char shared_dir[256] = "shared/";


// clientThreadConfig.cfg format:
//   line 1: tracker port
//   line 2: tracker IP
//   line 3: update interval in seconds
void read_client_config() {
    FILE *f = fopen("clientThreadConfig.cfg", "r");
    if (!f) {
        printf("no clientThreadConfig.cfg found, using defaults\n");
        return;
    }
    fscanf(f, "%d\n%63s\n%d", &tracker_port, tracker_ip, &update_interval);
    fclose(f);
    printf("tracker is at %s:%d, updating every %ds\n",
           tracker_ip, tracker_port, update_interval);
}


// serverThreadConfig.cfg format:
//   line 1: port we listen on for other peers
//   line 2: our shared folder
void read_server_config() {
    FILE *f = fopen("serverThreadConfig.cfg", "r");
    if (!f) {
        printf("no serverThreadConfig.cfg found, using defaults\n");
        return;
    }
    fscanf(f, "%d\n%255s", &my_listen_port, shared_dir);
    fclose(f);

    int len = strlen(shared_dir);
    if (shared_dir[len-1] != '/')
        strncat(shared_dir, "/", sizeof(shared_dir) - len - 1);
}


// opens a fresh connection to the tracker each time
// runs per command instead of keeping one socket open
// because the tracker closes the connection after each reply
int connect_to_tracker() {
    int sockid = socket(AF_INET, SOCK_STREAM, 0);
    if (sockid < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(tracker_port);

    if (inet_pton(AF_INET, tracker_ip, &addr.sin_addr) <= 0) {
        printf("bad tracker IP: %s\n", tracker_ip);
        close(sockid); return -1;
    }

    if (connect(sockid, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("couldnt connect to tracker"); close(sockid); return -1;
    }
    return sockid;
}


// sends msg and reads back everything the tracker sends until it signals done
// if out_buf is not NULL the response gets saved there too (used by do_get)
void send_and_recv(int sock, const char *msg, char *out_buf, int out_size) {
    if (write(sock, msg, strlen(msg)) < 0) {
        perror("write"); return;
    }

    char buf[MAXLINE];
    int total = 0;
    int done  = 0;

    while (!done) {
        int n = read(sock, buf, MAXLINE - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        printf("%s", buf);

        if (out_buf && total + n < out_size) {
            memcpy(out_buf + total, buf, n);
            total += n;
            out_buf[total] = '\0';
        }

        // tracker always ends its reply with one of these
        if (strstr(buf, "END")     || strstr(buf, "succ") ||
            strstr(buf, "fail")    || strstr(buf, "ferr") ||
            strstr(buf, "invalid"))
            done = 1;
    }
}


void do_list() {
    int sock = connect_to_tracker();
    if (sock < 0) return;

    printf("\n--- REQ LIST ---\n");
    send_and_recv(sock, "<REQ LIST>\n", NULL, 0);
    printf("----------------\n\n");

    close(sock);
}


void do_get(const char *trackfile) {
    int sock = connect_to_tracker();
    if (sock < 0) return;

    char msg[256];
    snprintf(msg, sizeof(msg), "<GET %s>\n", trackfile);

    printf("\n--- GET %s ---\n", trackfile);

    // save response so we can write the .track file locally
    char response[MAXLINE * 4];
    memset(response, 0, sizeof(response));
    send_and_recv(sock, msg, response, sizeof(response));
    close(sock);

    // pull out just the content between BEGIN and END markers
    if (strstr(response, "<REP GET BEGIN>")) {
        char *begin = strstr(response, "<REP GET BEGIN>\n");
        char *end   = strstr(response, "<REP GET END");
        if (begin && end) {
            begin += strlen("<REP GET BEGIN>\n");
            int content_len = end - begin;

            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s%s", shared_dir, trackfile);

            mkdir(shared_dir, 0755);
            FILE *fp = fopen(filepath, "w");
            if (fp) {
                fwrite(begin, 1, content_len, fp);
                fclose(fp);
                printf("saved tracker file to %s\n", filepath);
                
                start_multithreaded_download((char *)trackfile);
            }
        }
    }
    printf("--------------\n\n");
}


void do_createtracker(const char *fname, long fsize, const char *desc,
                      const char *md5, const char *ip, int port) {
    int sock = connect_to_tracker();
    if (sock < 0) return;

    char msg[512];
    snprintf(msg, sizeof(msg),
             "<createtracker %s %ld %s %s %s %d>\n",
             fname, fsize, desc, md5, ip, port);

    printf("\n--- %s", msg);
    send_and_recv(sock, msg, NULL, 0);
    printf("---\n\n");

    close(sock);
}


void do_updatetracker(const char *fname, long start_b, long end_b,
                      const char *ip, int port) {
    int sock = connect_to_tracker();
    if (sock < 0) return;

    char msg[512];
    snprintf(msg, sizeof(msg),
             "<updatetracker %s %ld %ld %s %d>\n",
             fname, start_b, end_b, ip, port);

    printf("\n--- %s", msg);
    send_and_recv(sock, msg, NULL, 0);
    printf("---\n\n");

    close(sock);
}


// background thread that pings the tracker every update_interval seconds
// this is how the tracker knows we're still alive and what bytes we have
void *update_thread_func(void *arg) {
    (void)arg;
    printf("update thread running, will ping tracker every %ds\n", update_interval);

    while (1) {
        sleep(update_interval);
        printf("sending periodic updatetracker...\n");
        // TODO before final: scan shared_dir and send one update per file
        do_updatetracker("demo", 0, 0, tracker_ip, my_listen_port);
    }
    return NULL;
}



//send all bytes in buffer over socket (handles partial writes)
static int send_all(int sock, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;

    //loop until all bytes are sent
    while (sent < len) {
        ssize_t n = write(sock, p + sent, len - sent);

        //retry if interrupted
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1; //real error
        }

        sent += (size_t)n;
    }
    return 0;
}

//read a single line from socket until newline
static int recv_line(int sock, char *buf, size_t size) {
    size_t i = 0;

    if (size == 0) return -1;

    while (i < size - 1) {
        char c;
        ssize_t n = read(sock, &c, 1);

        //try again if interrupted
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        if (n == 0) break;   // connection closed

        buf[i++] = c;
        
        if (c == '\n') break; // stop at newline
    }

    buf[i] = '\0'; // null terminate
    return (int)i;
}

//ensure filename is safe (no directory traversal)
static int filename_is_safe(const char *name) {
    if (!name || !*name) return 0;

    // block ../ or absolute paths
    if (strstr(name, "..")) return 0;
    if (strchr(name, '/')) return 0;
    if (strchr(name, '\\')) return 0;

    return 1;
}

// send standardized error message to peer
static int send_error_msg(int sock, const char *msg) {
    char line[256];
    snprintf(line, sizeof(line), "ERR %s\n", msg);
    return send_all(sock, line, strlen(line));
}

//core logic: send requested byte range from file
static int handle_getchunk_request(int conn, const char *filename, long start_b, long end_b) {
    char filepath[512];
    FILE *fp = NULL;
    long filesize;
    long bytes_to_send;
    char buffer[CHUNK_BUFSIZE];

    //validate filename for security
    if (!filename_is_safe(filename)) {
        return send_error_msg(conn, "invalid filename");
    }

    //build full file path inside shared directory
    snprintf(filepath, sizeof(filepath), "%s%s", shared_dir, filename);

    //open file for reading (binary mode)
    fp = fopen(filepath, "rb");
    if (!fp) {
        return send_error_msg(conn, "file not found");
    }

    //get file size
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return send_error_msg(conn, "fseek failed");
    }

    filesize = ftell(fp);
    if (filesize < 0) {
        fclose(fp);
        return send_error_msg(conn, "ftell failed");
    }

    // validate requested byte range
    if (start_b < 0 || end_b < start_b || start_b >= filesize) {
        fclose(fp);
        return send_error_msg(conn, "invalid byte range");
    }

    //clamp end byte if it exceeds file size
    if (end_b >= filesize) {
        end_b = filesize - 1;
    }

    //total bytes to send
    bytes_to_send = end_b - start_b + 1;
    
    // byte checker 1024
    if (bytes_to_send > 1024) {
        printf("Rejecting request: %ld bytes is too large (max 1024)\n", bytes_to_send);
        return send_error_msg(conn, "invalid chunk size (max 1024)");
    }

    // move file pointer to start byte
    if (fseek(fp, start_b, SEEK_SET) != 0) {
        fclose(fp);
        return send_error_msg(conn, "could not seek to start byte");
    }

    //send response header first
    {
        char header[128];
        snprintf(header, sizeof(header), "OK %ld %ld\n", start_b, end_b);

        if (send_all(conn, header, strlen(header)) < 0) {
            fclose(fp);
            return -1;
        }
    }

    // send file data in chunks
    while (bytes_to_send > 0) {
        size_t want = (bytes_to_send > CHUNK_BUFSIZE) ? CHUNK_BUFSIZE : (size_t)bytes_to_send;

        size_t got = fread(buffer, 1, want, fp);

        //check for read error
        if (got == 0) {
            if (ferror(fp)) {
                fclose(fp);
                return -1;
            }
            break;
        }

        //send chunk to peer
        if (send_all(conn, buffer, got) < 0) {
            fclose(fp);
            return -1;
        }

        bytes_to_send -= (long)got;
    }

    fclose(fp);
    return 0;
}

// background thread: listens for incoming peer connections
void *server_thread_func(void *arg) {
    (void)arg;

    //create TCP socke
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("peer server socket");
        return NULL;
    }

    //allow reuse of address (avoids bind errors)
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    //setup address structure
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(my_listen_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    //bind socket to port
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("peer server bind");
        close(listener);
        return NULL;
    }

    //start listening for connections
    if (listen(listener, BACKLOG) < 0) {
        perror("peer server listen");
        close(listener);
        return NULL;
    }

    printf("listening for peers on port %d\n", my_listen_port);

    //main accept loop
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);

        //accept incoming connection
        int conn = accept(listener, (struct sockaddr *)&client_addr, &clen);
        if (conn < 0) {
            perror("accept");
            continue;
        }

        printf("peer connected from %s\n", inet_ntoa(client_addr.sin_addr));

        //read request line
        char req[MAXLINE];
        int n = recv_line(conn, req, sizeof(req));
        if (n <= 0) {
            close(conn);
            continue;
        }

        printf("peer request: %s", req);

        //parse request
        char cmd[64];
        char filename[256];
        long start_b, end_b;

        int parsed = sscanf(req, "%63s %255s %ld %ld", cmd, filename, &start_b, &end_b);

        //handle "GETCHUNK request
        if (parsed == 4 && strcmp(cmd, "GETCHUNK") == 0) {
            if (handle_getchunk_request(conn, filename, start_b, end_b) < 0) {
                perror("handle_getchunk_request");
            }
        } else {
            //invalid request format
            send_error_msg(conn, "expected: GETCHUNK <filename> <start> <end>");
        }
        close(conn); 
    }
    return NULL;
}

// =========================================================
// THE MULTI-THREADED DOWNLOADER & MD5 VERIFIER
// =========================================================

// function so the TA's computer doesn't crash if they aren't on a Mac
void get_cross_platform_md5(const char *filepath, char *result_hash) {
    FILE *pipe;
    char cmd[512];
    
    // First try the standard Mac command built this on Macs
    sprintf(cmd, "md5 -q %s 2>/dev/null", filepath);
    pipe = popen(cmd, "r");
    if (pipe != NULL && fscanf(pipe, "%63s", result_hash) == 1) {
        pclose(pipe);
        return;
    }
    if (pipe) pclose(pipe);

    // If that failed probably on Linux or WSL, so try md5sum instead
    sprintf(cmd, "md5sum %s 2>/dev/null", filepath);
    pipe = popen(cmd, "r");
    if (pipe != NULL && fscanf(pipe, "%63s", result_hash) == 1) {
        pclose(pipe);
        return;
    }
    if (pipe) pclose(pipe);

    // If all else fails return an error string
    strcpy(result_hash, "ERROR_CALCULATING_MD5");
}

// The "Worker" thread its only job is to grab one specific 1024-byte piece of the file
void *download_chunk_thread(void *arg) {
    struct ChunkTask *task = (struct ChunkTask *)arg;
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in peer_addr;
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(task->peer_port);
    peer_addr.sin_addr.s_addr = inet_addr(task->peer_ip);

    if (connect(sock, (struct sockaddr *)&peer_addr, sizeof(peer_addr)) < 0) {
        printf("ERROR: Peer offline! Could not connect to %s:%d\n", task->peer_ip, task->peer_port);
        free(task);
        return NULL;
    }

    char request[256];
    sprintf(request, "GETCHUNK %s %ld %ld\n", task->filename, task->start_byte, task->end_byte);
    write(sock, request, strlen(request));

    char buffer[2048];
    // SAFETY: Leave room for null terminator so strchr doesn't crash
    int bytes_read = read(sock, buffer, sizeof(buffer) - 1);
    
    if (bytes_read <= 0) {
        printf("ERROR: Connected, but peer sent no data.\n");
        close(sock);
        free(task);
        return NULL;
    }
    
    buffer[bytes_read] = '\0'; // Make it a safe string
    
    char *data_start = strchr(buffer, '\n');
    if (data_start != NULL) {
        data_start++;
        int header_size = data_start - buffer;
        int data_bytes = bytes_read - header_size;

        char filepath[512];
        sprintf(filepath, "%s%s", shared_dir, task->filename);
        
        FILE *fp = fopen(filepath, "r+");
        if (fp == NULL) fp = fopen(filepath, "w");
        
        if (fp != NULL) {
            fseek(fp, task->start_byte, SEEK_SET);
            fwrite(data_start, 1, data_bytes, fp);
            
            int total_needed = (task->end_byte - task->start_byte + 1);
            int remaining = total_needed - data_bytes;
            while (remaining > 0) {
                int n = read(sock, buffer, sizeof(buffer));
                if (n <= 0) break;
                fwrite(buffer, 1, n, fp);
                remaining -= n;
            }
            fclose(fp);
        }
    }
    
    int my_peer_num = my_listen_port - 4000;
    printf("Peer%d downloading %ld to %ld bytes of %s from %s %d\n",
           my_peer_num, task->start_byte, task->end_byte, task->filename, task->peer_ip, task->peer_port);
           
    usleep(25000); // 25ms artificial network lag per rubric
           
    close(sock);
    free(task);
    return NULL;
}

// The "Boss" function: reads the .track file and sends out the worker threads
void start_multithreaded_download(char *trackfile_name) {
    char filepath[512];
    sprintf(filepath, "%s%s", shared_dir, trackfile_name);

    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) return;

    char real_filename[256] = "";
    int filesize = 0;
    char expected_md5[64] = "";
    char peer_ip[64] = "";
    int peer_port = 0;

    // Dig through the file to find the filename, size, MD5, and the peer's IP/Port
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Filename:")) sscanf(line, "Filename: %s", real_filename);
        else if (strstr(line, "Filesize:")) sscanf(line, "Filesize: %d", &filesize);
        else if (strstr(line, "MD5:")) sscanf(line, "MD5: %s", expected_md5);
        else if (line[0] != '#' && strstr(line, ":") && peer_port == 0) {
            sscanf(line, "%[^:]:%d", peer_ip, &peer_port);
        }
    }
    fclose(fp);

    // Figure out exactly how many threads we need if everyone only takes 1024 bytes
    int num_chunks = (filesize + 1023) / 1024;
    pthread_t threads[num_chunks];

    // Spawn a thread for every single chunk
    for (int i = 0; i < num_chunks; i++) {
        struct ChunkTask *task = malloc(sizeof(struct ChunkTask));
        strcpy(task->filename, real_filename);
        strcpy(task->peer_ip, peer_ip);
        task->peer_port = peer_port;
        
        task->start_byte = i * 1024;
        task->end_byte = task->start_byte + 1023;
        
        // Dont let the last chunk accidentally ask for more file than actually exists
        if (task->end_byte >= filesize) task->end_byte = filesize - 1;

        pthread_create(&threads[i], NULL, download_chunk_thread, task);
        usleep(10000); // Wait 10 milliseconds between creating threads
    }

    // Wait for all the worker threads to come back
    for (int i = 0; i < num_chunks; i++) {
        pthread_join(threads[i], NULL);
    }

    // Did we get corrupted data check
    char downloaded_filepath[512];
    sprintf(downloaded_filepath, "%s%s", shared_dir, real_filename);

    char computed_md5[64] = "";
    get_cross_platform_md5(downloaded_filepath, computed_md5);

    if (strcmp(computed_md5, expected_md5) == 0) {
        printf("Success! MD5 Matched (%s).\n", computed_md5);
        remove(filepath); // delete this file when we hit 100%
    } else {
        printf("ERROR: MD5 Mismatch!\nExpected: %s\nGot:      %s\n", expected_md5, computed_md5);
    }
}

int main(int argc, char *argv[]) {
    read_client_config();
    read_server_config();

    // kick off background threads
    pthread_t update_tid, server_tid;
    pthread_create(&update_tid, NULL, update_thread_func, NULL);
    pthread_detach(update_tid);
    pthread_create(&server_tid, NULL, server_thread_func, NULL);
    pthread_detach(server_tid);

    if (argc < 2) {
        printf("\nusage:\n");
        printf("  ./peer list\n");
        printf("  ./peer get <filename.track>\n");
        printf("  ./peer createtracker <fname> <fsize> <desc> <md5> <ip> <port>\n");
        printf("  ./peer updatetracker <fname> <start> <end> <ip> <port>\n\n");
        return 1;
    }

    if (!strcmp(argv[1], "list")) {
        do_list();

    } else if (!strcmp(argv[1], "get")) {
        if (argc < 3) { printf("usage: ./peer get <filename.track>\n"); return 1; }
        do_get(argv[2]);

    } else if (!strcmp(argv[1], "createtracker")) {
        if (argc < 8) {
            printf("usage: ./peer createtracker <fname> <fsize> <desc> <md5> <ip> <port>\n");
            return 1;
        }
        do_createtracker(argv[2], atol(argv[3]), argv[4], argv[5], argv[6], atoi(argv[7]));

    } else if (!strcmp(argv[1], "updatetracker")) {
        if (argc < 7) {
            printf("usage: ./peer updatetracker <fname> <start> <end> <ip> <port>\n");
            return 1;
        }
        do_updatetracker(argv[2], atol(argv[3]), atol(argv[4]), argv[5], atoi(argv[6]));

    } else {
        printf("unknown command: %s\n", argv[1]);
        return 1;
    }
    
    // If this peer just created a tracker, it needs to stay alive forever to serve the files!
    if (argc > 1 && strcmp(argv[1], "createtracker") == 0) {
        printf("Seed mode active. Keeping server thread alive...\n");
        while (1) {
            sleep(1); // Sleep forever so the program doesn't close
        }
    }

    return 0;
}
