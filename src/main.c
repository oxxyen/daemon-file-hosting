#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define PID_FILE "/tmp/exchange-daemon.pid"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <limits.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <strings.h>
#include <signal.h>
#include <sys/param.h>
#include <limits.h>
#include <mongoc/mongoc.h>


// headers
#include "db/mongo_ops.h"


#define EXCHANGE_DIR "/home/just/mesh_proto/oxxyen_storage/file_dir/filetrade"
#define EVENT_BUFFER_SIZE (sizeof(struct inotify_event) + NAME_MAX + 1)


static volatile sig_atomic_t g_shutdown = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}
// глобальные переменные монго
mongoc_client_t *g_mongo_client = NULL;
mongoc_collection_t *g_collection = NULL;

static bool is_regular_file(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

static uint64_t get_file_size(const char *path) {

    struct stat st;
    return (stat(path, &st) == 0) ? (uint64_t)st.st_size : 0;

}

static const char *get_mime_type(const char *filename) {

    const char *dot = strrchr(filename, '.');
    if (!dot) return "application/octet-stream";

    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".png") == 0) return "image/png";

    if (strcasecmp(dot, ".gif") == 0) return "image/gif";

    if (strcasecmp(dot, ".txt") == 0) return "text/plain";

    if (strcasecmp(dot, ".pdf") == 0) return "application/pdf";


    return "application/octet-stream";
}

static void handle_file_created(const char *fullpath) {

    if (!is_regular_file(fullpath)) return;

    const char *filename = strrchr(fullpath, '/');

    if (!filename) filename = fullpath;

    else ++filename;

    uint64_t size = get_file_size(fullpath);
    const char *mime = get_mime_type(filename);

    if(!mongo_insert(filename, size, mime)) {
        
        fprintf(stderr, "failed to insert %s into MongoDb\n");

    }
    
    printf("INSERT: %s | size=%lu | mime=%s\n", filename, (unsigned long)size, mime);
}

static void handle_file_deleted(const char *filename) {
    // TODO: mongo_mark_deleted(filename);
    // TODO: signal_remote_delete(filename);
    printf("DELETE_SIGNAL: %s\n", filename);
}

int main(void) {
    FILE *pidfp = fopen(PID_FILE, "r");
    if(pidfp) {
        pid_t old_pid;
        if(fscanf(pidfp, "%d", &old_pid) == 1 && kill(old_pid, 0) == 0) {
            fclose(pidfp);
            fprintf(stderr, "daemon already running(PID %d)\n", (int)old_pid);
            return EXIT_FAILURE;
        }
        fclose(pidfp);
    }

    pidfp = fopen(PID_FILE, "w");
    if(!pidfp) return EXIT_FAILURE;
    fprintf(pidfp, "%d\n", (int)getpid());
    fclose(pidfp);

    if (daemon(1, 1) == -1) {
        perror("daemon");
        return EXIT_FAILURE;
    }

    freopen("/tmp/exchange-daemon.log", "a", stdout);
    freopen("/tmp/exchange-daemon.log", "a", stderr);

    struct sigaction sa = {0};

    mongoc_init();

    g_mongo_client = mongoc_client_new("mongodb://localhost:27017");
    
    if (!g_mongo_client) {

        fprintf(stderr, "Failed to connect to MongoDB\n");
        return EXIT_FAILURE;

    }

    g_collection = mongoc_client_get_collection(g_mongo_client, "file_exchange", "files");
    if (!g_collection) {

        fprintf(stderr, "Failed to get collection\n");
        mongoc_client_destroy(g_mongo_client);
        return EXIT_FAILURE;

    }

    printf("Connected to MongoDB, collection: files\n");
    fflush(stdout);

    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        return EXIT_FAILURE;
    }

    int inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd == -1) return EXIT_FAILURE;

    int wd = inotify_add_watch(inotify_fd, EXCHANGE_DIR,
        IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_DELETE_SELF);
    if (wd == -1) {
        close(inotify_fd);
        return EXIT_FAILURE;
    }

    char buffer[32768] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (!g_shutdown) {
        ssize_t len = read(inotify_fd, buffer, sizeof(buffer));
        if (len == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                usleep(10000); // 10ms
                continue;
            }
            break;
        }

        for (char *ptr = buffer; ptr < buffer + len;) {
            struct inotify_event *e = (struct inotify_event *)ptr;
            if (e->len == 0) {
                ptr += sizeof(struct inotify_event);
                continue;
            }

            if (e->mask & IN_MOVED_TO) {
                char fullpath[PATH_MAX];
                if (snprintf(fullpath, sizeof(fullpath), "%s/%s", EXCHANGE_DIR, e->name) < 0)
                    continue;
                handle_file_created(fullpath);
            } else if (e->mask & (IN_MOVED_FROM | IN_DELETE)) {
                handle_file_deleted(e->name);
            }

            ptr += sizeof(struct inotify_event) + e->len;
        }
    }

    inotify_rm_watch(inotify_fd, wd);

    close(inotify_fd);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);


    unlink(PID_FILE);
    return EXIT_SUCCESS;
}