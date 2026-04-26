#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#define MAXLINE  2048
#define BACKLOG  10

// globals - filled in from sconfig at startup
int  server_port = 3490;
char torrent_dir[256] = "torrents/";

// socket fds - global so peer_handler can reach them
int sockid;
int sock_child;

// the message buffer peer_handler reads into
char read_msg[MAXLINE];

// when we parse a createtracker or updatetracker message we
// dump the fields into these so the handler functions can use them
char tk_fname[256];
char tk_fsize[64];
char tk_desc[256];
char tk_md5[64];
char tk_ip[64];
int  tk_port;
long tk_start;
long tk_end;


// called when a child process finishes so it doesnt become a zombie
void sigchld_handler(int s) {
    (void)s;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}


void read_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("couldnt open %s, sticking with defaults\n", path);
        return;
    }
    fscanf(f, "%d\n%255s", &server_port, torrent_dir);
    fclose(f);

    // make sure the path ends with a slash or our snprintfs will be wrong
    int len = strlen(torrent_dir);
    if (torrent_dir[len-1] != '/')
        strncat(torrent_dir, "/", sizeof(torrent_dir) - len - 1);
}


// messages come in wrapped in angle brackets like <createtracker ...>
// this just strips those off so we can parse the inside
void strip_brackets(char *msg) {
    char *s = strchr(msg, '<');
    char *e = strrchr(msg, '>');
    if (s && e && e > s) {
        *e = '\0';
        memmove(msg, s + 1, strlen(s));
    }
}


// pulls the filename out of a GET message
// e.g. "<GET movie1.avi.track>" -> tk_fname = "movie1.avi.track"
void xtrct_fname(char *msg) {
    char tmp[MAXLINE];
    strncpy(tmp, msg, MAXLINE - 1);
    strip_brackets(tmp);

    strtok(tmp, " \t\n");  // skip "GET"
    char *tok = strtok(NULL, " \t\n<>\r\n");
    if (tok)
        strncpy(tk_fname, tok, sizeof(tk_fname) - 1);
}


// breaks apart a createtracker message into the global tk_ vars
// format: createtracker fname fsize desc md5 ip port
void tokenize_createmsg(char *msg) {
    char tmp[MAXLINE];
    strncpy(tmp, msg, MAXLINE - 1);
    strip_brackets(tmp);

    char *tok = strtok(tmp, " \t\n");  // "createtracker" keyword, skip it
    if (!tok) return;

    tok = strtok(NULL, " \t\n"); if (tok) strncpy(tk_fname, tok, sizeof(tk_fname)-1);
    tok = strtok(NULL, " \t\n"); if (tok) strncpy(tk_fsize, tok, sizeof(tk_fsize)-1);
    tok = strtok(NULL, " \t\n"); if (tok) strncpy(tk_desc,  tok, sizeof(tk_desc) -1);
    tok = strtok(NULL, " \t\n"); if (tok) strncpy(tk_md5,   tok, sizeof(tk_md5)  -1);
    tok = strtok(NULL, " \t\n"); if (tok) strncpy(tk_ip,    tok, sizeof(tk_ip)   -1);
    tok = strtok(NULL, " \t\n"); if (tok) tk_port = atoi(tok);
}


// same idea but for updatetracker
// format: updatetracker fname start_byte end_byte ip port
void tokenize_updatemsg(char *msg) {
    char tmp[MAXLINE];
    strncpy(tmp, msg, MAXLINE - 1);
    strip_brackets(tmp);

    char *tok = strtok(tmp, " \t\n");  // skip "updatetracker"
    if (!tok) return;

    tok = strtok(NULL, " \t\n"); if (tok) strncpy(tk_fname, tok, sizeof(tk_fname)-1);
    tok = strtok(NULL, " \t\n"); if (tok) tk_start = atol(tok);
    tok = strtok(NULL, " \t\n"); if (tok) tk_end   = atol(tok);
    tok = strtok(NULL, " \t\n"); if (tok) strncpy(tk_ip,    tok, sizeof(tk_ip)   -1);
    tok = strtok(NULL, " \t\n"); if (tok) tk_port  = atoi(tok);
}


// handles REQ LIST - opens the torrents folder, reads each .track file
// to get the name/size/md5, and sends them back to the peer
void handle_list_req(int sock) {
    DIR *d = opendir(torrent_dir);
    if (!d) {
        printf("cant open %s\n", torrent_dir);
        write(sock, "<REP LIST 0>\n<REP LIST END>\n", 28);
        return;
    }

    // grab all the .track filenames first
    char files[128][256];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < 128) {
        if (strstr(entry->d_name, ".track"))
            strncpy(files[count++], entry->d_name, 255);
    }
    closedir(d);

    char buf[MAXLINE];
    snprintf(buf, sizeof(buf), "<REP LIST %d>\n", count);
    write(sock, buf, strlen(buf));

    for (int i = 0; i < count; i++) {
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s%s", torrent_dir, files[i]);

        // open each .track file and pull out the fields we need
        FILE *fp = fopen(filepath, "r");
        char fname[256] = "?";
        char fsize[64]  = "0";
        char md5[64]    = "?";
        char line[512];
        while (fp && fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "Filename:", 9) == 0)
                sscanf(line, "Filename: %255s", fname);
            else if (strncmp(line, "Filesize:", 9) == 0)
                sscanf(line, "Filesize: %63s", fsize);
            else if (strncmp(line, "MD5:", 4) == 0)
                sscanf(line, "MD5: %63s", md5);
        }
        if (fp) fclose(fp);

        snprintf(buf, sizeof(buf), "<%d %s %s %s>\n", i+1, fname, fsize, md5);
        write(sock, buf, strlen(buf));
    }

    write(sock, "<REP LIST END>\n", 15);
    printf("sent list to peer (%d files)\n", count);
}


// handles GET - just streams the .track file back to whoever asked
void handle_get_req(int sock, char *filename) {
    filename[strcspn(filename, " \t\r\n")] = '\0';

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s%s", torrent_dir, filename);

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        printf("GET failed - %s doesnt exist\n", filepath);
        write(sock, "<GET invalid>\n", 14);
        return;
    }

    write(sock, "<REP GET BEGIN>\n", 16);

    char buf[MAXLINE];
    while (fgets(buf, sizeof(buf), fp))
        write(sock, buf, strlen(buf));
    fclose(fp);

    // TODO: compute actual md5 of the tracker file content before final demo
    write(sock, "<REP GET END placeholder_md5>\n", 30);
    printf("sent %s to peer\n", filename);
}


// handles createtracker - makes a new .track file if one doesnt exist yet
// only seeds (peers with the full file) should be calling this
void handle_createtracker_req(int sock) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s%s.track", torrent_dir, tk_fname);

    if (access(filepath, F_OK) == 0) {
        // already exists, tell the peer
        write(sock, "<createtracker ferr>\n", 21);
        printf("createtracker rejected - %s.track already exists\n", tk_fname);
        return;
    }

    mkdir(torrent_dir, 0755);

    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        write(sock, "<createtracker fail>\n", 21);
        printf("createtracker failed - couldnt write to %s\n", filepath);
        return;
    }

    fprintf(fp, "Filename: %s\n",    tk_fname);
    fprintf(fp, "Filesize: %s\n",    tk_fsize);
    fprintf(fp, "Description: %s\n", tk_desc);
    fprintf(fp, "MD5: %s\n",         tk_md5);
    fprintf(fp, "# ip:port:start:end:timestamp\n");
    fprintf(fp, "%s:%d:0:%s:%ld\n",  tk_ip, tk_port, tk_fsize, time(NULL));
    fclose(fp);

    write(sock, "<createtracker succ>\n", 21);
    printf("created %s.track for %s:%d\n", tk_fname, tk_ip, tk_port);
}


// handles updatetracker - appends a new peer entry to the .track file
// peers call this periodically to say "i still have bytes X to Y"
void handle_updatetracker_req(int sock) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s%s.track", torrent_dir, tk_fname);

    char resp[512];

    if (access(filepath, F_OK) != 0) {
        snprintf(resp, sizeof(resp), "<updatetracker %s ferr>\n", tk_fname);
        write(sock, resp, strlen(resp));
        printf("updatetracker failed - no %s.track found\n", tk_fname);
        return;
    }

    // just append for now - for the final demo we should check if this
    // peer already has an entry and update it instead of duplicating
    FILE *fp = fopen(filepath, "a");
    if (!fp) {
        snprintf(resp, sizeof(resp), "<updatetracker %s fail>\n", tk_fname);
        write(sock, resp, strlen(resp));
        return;
    }
    fprintf(fp, "%s:%d:%ld:%ld:%ld\n", tk_ip, tk_port, tk_start, tk_end, time(NULL));
    fclose(fp);

    snprintf(resp, sizeof(resp), "<updatetracker %s succ>\n", tk_fname);
    write(sock, resp, strlen(resp));
    printf("updated %s.track - peer %s has bytes %ld to %ld\n",
           tk_fname, tk_ip, tk_start, tk_end);
}


// this is what the forked child runs - reads the command and calls
// the right handler, then returns so the child can close and exit
void peer_handler(int sock) {
    int length = read(sock, read_msg, MAXLINE - 1);
    if (length <= 0) return;
    read_msg[length] = '\0';

    printf("got from peer: %s\n", read_msg);

    if (strstr(read_msg, "REQ LIST") || strstr(read_msg, "req list")) {
        handle_list_req(sock);

    } else if (strstr(read_msg, "GET") || strstr(read_msg, "get")) {
        xtrct_fname(read_msg);
        handle_get_req(sock, tk_fname);

    } else if (strstr(read_msg, "createtracker") ||
               strstr(read_msg, "Createtracker") ||
               strstr(read_msg, "CREATETRACKER")) {
        tokenize_createmsg(read_msg);
        handle_createtracker_req(sock);

    } else if (strstr(read_msg, "updatetracker") ||
               strstr(read_msg, "Updatetracker") ||
               strstr(read_msg, "UPDATETRACKER")) {
        tokenize_updatemsg(read_msg);
        handle_updatetracker_req(sock);

    } else {
        printf("no idea what this is: %s\n", read_msg);
        write(sock, "<error unknown command>\n", 24);
    }
}


int main() {
    read_config("sconfig");

    // clean up dead child processes so we dont accumulate zombies
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    struct sockaddr_in server_addr, client_addr;
    socklen_t clilen = sizeof(client_addr);
    pid_t pid;

    if ((sockid = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed"); exit(1);
    }

    // lets us restart the tracker quickly without waiting for the port to free
    int yes = 1;
    setsockopt(sockid, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(server_port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    memset(server_addr.sin_zero, 0, sizeof(server_addr.sin_zero));

    if (bind(sockid, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed"); exit(1);
    }

    if (listen(sockid, BACKLOG) < 0) {
        perror("listen failed"); exit(1);
    }

    printf("tracker up on port %d, storing files in %s\n", server_port, torrent_dir);

    while (1) {
        sock_child = accept(sockid, (struct sockaddr *)&client_addr, &clilen);
        if (sock_child == -1) {
            if (errno == EINTR) continue;
            perror("accept failed"); continue;
        }

        printf("connection from %s\n", inet_ntoa(client_addr.sin_addr));

        if ((pid = fork()) == 0) {
            // child - handle this peer then die
            close(sockid);
            peer_handler(sock_child);
            close(sock_child);
            exit(0);
        }
        // parent - done with this socket, child owns it now
        close(sock_child);
    }

    return 0;
}
