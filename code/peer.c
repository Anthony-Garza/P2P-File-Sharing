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

#define MAXLINE  2048
#define BACKLOG  5

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


// background thread that listens for other peers wanting to download from us
// not fully implemented yet - needed for the final demo chunk transfer
void *server_thread_func(void *arg) {
    (void)arg;

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("peer server socket"); return NULL; }

    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(my_listen_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("peer server bind"); close(listener); return NULL;
    }
    if (listen(listener, BACKLOG) < 0) {
        perror("peer server listen"); close(listener); return NULL;
    }

    printf("listening for peers on port %d\n", my_listen_port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int conn = accept(listener, (struct sockaddr *)&client_addr, &clen);
        if (conn < 0) { perror("accept"); continue; }

        printf("peer connected from %s\n", inet_ntoa(client_addr.sin_addr));

        // for now just acknowledge - final demo will send actual file chunks here
        write(conn, "peer server ready\n", 18);
        close(conn);
    }
    return NULL;
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

    return 0;
}