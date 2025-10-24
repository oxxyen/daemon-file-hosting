#include <errno.h>
#include <linux/limits.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <mongoc/mongoc.h>

//* подмодули
#include "../db/mongo_ops.h"
#include "../../include/protocol.h"

//* определяем глобальные переменные

mongoc_client_t *g_mongo_client = NULL;
extern mongoc_collection_t *g_collection;

//* основные параметры
#define PORT             2222
#define RECV_BUFFER_SIZE 4096
#define MAX_EVENTS        10

//* структура для подключения
typedef struct {

    int client_socket;
    struct sockaddr_in client_addr;

} client_info_t;

void handle_upload_request(int client_fd, RequestHeader *req, const char *storage_dir );
void handle_list_request (int client_fd, mongoc_collection_t *collection);

//* обработка одного клиента(отдельный поток)
void *handle_client(void *arg) {

    client_info_t *info = (client_info_t *)arg;

    int client_fd = info->client_socket;


    printf("handing client on %d..\n", client_fd);

    //* монго создаем запись

    RequestHeader req;
    if(recv_all(client_fd, &req, sizeof(RequestHeader) ) <= 0) {

        perror("failed to receive request header!");
        close(client_fd);
        free(info);
        return NULL;

    }

    // обработка команд

    switch(req.command) {

        case CMD_UPLOAD:{

            printf("received UPLOAD command for file: %s (size: %lld)\n",req.filename, req.filesize);
            handle_upload_request(client_fd, &req, "../../filetrade");
            break;

        }

        case CMD_LIST: {

            printf("received LIST command\n");
            handle_list_request(client_fd, g_collection);
            break;

        }

        case CMD_DOWNLOAD:
            
            fprintf(stderr, "CMD_DOWNLOAD Not implement yet\n");
            break;
        
        default:
            
            fprintf(stderr, "unknow command: %d\n", req.command);
            break;

    }

    // mongoc_client_t *client = mongoc_client_new("mongodb://localhost:27017");

    // if(!client) {
    //     fprintf(stderr, "mongodb client create failed!");
    //     close(client_fd);
        
    //     free(info);

    // }

    // mongoc_collection_t *collection = mongoc_client_get_collection(client, "mydatabase", "file_overseer");

    // file_record_t record = {0};

    // snprintf(record.filename, sizeof(record.filename), "upload_from_%s", inet_ntoa(info->client_addr.sin_addr));
    // strncpy(record.extension, "bin", sizeof(record.extension) - 1);

    // record.filename[sizeof(record.filename) - 1] = '\0';

    // record.initial_size = 0;
    // record.actual_size = 0;

    // bson_t *doc = file_overseer_to_bson(&record);
    // bson_error_t error;

    // if(doc && !mongoc_collection_insert_one(collection, doc, NULL, NULL, &error)) {

    //     fprintf(stderr, "mongodb insert failed: %s\n", error.message);

    // } else {

    //     fprintf(stdout, "mongodb: collection logged for client %s\n", inet_ntoa(info->client_addr.sin_addr));

    // }

    // bson_destroy(doc);
    // mongoc_collection_destroy(collection);
    // mongoc_client_destroy(client);

    // char buffer[RECV_BUFFER_SIZE];

    // ssize_t bytes_received;

    // while((bytes_received = recv(client_fd, buffer, RECV_BUFFER_SIZE, 0)) > 0) {
    //     buffer[bytes_received] = '\0';

    //     printf("bytes received from client %d: %s\n", client_fd, buffer);

    //     send(client_fd, buffer, bytes_received, 0);
    // }

    // if(bytes_received == 0) {
    //     printf("client disconn %d", client_fd);
    // } else if(bytes_received == -1) {
    //     perror("recv");
    // }

    close(client_fd);
    free(info);

    return 0;
}

void handle_list_request (int client_fd, mongoc_collection_t *collection) {
    bson_t *query = NULL;
    bson_t *opts = NULL;
    mongoc_cursor_t *cursor = NULL;
    const bson_t *doc;
    char *json_str;
    size_t total_len = 0;

    bson_error_t error;

    //* собираем весь список в памяти

    char *full_list = NULL;

    //* создаем пустой запрос
    query = bson_new();
    cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);


    if(!cursor) {

        fprintf(stderr, "error request: %s\n", error.message);
        bson_destroy(query);    

    }

    //* получаем все файлы (deleted=false)

    // BSON_APPEND_BOOL(query, "deleted", false);
    // cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    //* вычисляем общую длину списка
    while(mongoc_cursor_next(cursor, &doc)) {

        json_str = bson_as_legacy_extended_json(doc, NULL);
        size_t len = strlen(json_str);

        full_list = realloc(full_list, total_len + len + 2);

        memcpy(full_list + total_len, json_str, len);

        full_list[total_len + len] = '\0';
        full_list[total_len + len + 1] = '\0';

        total_len += len + 1;
        bson_free(json_str);

    }

    //* отправяем заголовок ответа

    ResponseHeader resp = {0};
    resp.status = RESP_SUCCESS;
    resp.filesize = total_len;

    send_all(client_fd, &resp, sizeof(resp));

    //* отправляем данные списка

    if(full_list) {

        send_all(client_fd, full_list, total_len);
        free(full_list);

    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(query);

}

void handle_upload_request(int client_fd, RequestHeader *req, const char *storage_dir) {

    char filepath[PATH_MAX];

    mongoc_collection_t *collection;

    snprintf(filepath, sizeof(filepath), "%s/%s", storage_dir, req->filename);

    if(strstr(req->filename, "..") || strchr(req->filename, '/')) {

        ResponseHeader resp = { .status = RESP_PERMISSION_DENIED };

        send_all(client_fd, &resp, sizeof(resp));
        return;

    }

    FILE *fp = fopen(filepath, "w");

    if(!fp) {

        ResponseHeader resp = { .status = RESP_ERROR };

        send_all(client_fd, &resp, sizeof(resp));

        return;
    }
    //* подтвеждаем готовность
    ResponseHeader resp = { .status = RESP_SUCCESS };

    send_all(client_fd, &resp, sizeof(resp));

    //* читаем и пишем файл

    char buffer[BUFFER_SIZE];
    long long remaning = req->filesize;

    while(remaning > 0) {

        size_t to_read = (remaning < BUFFER_SIZE) ? remaning : BUFFER_SIZE;
        
        recv_all(client_fd, buffer, to_read);

        fwrite(buffer, 1, to_read, fp);

        remaning -= to_read;

    }

    fclose(fp);

    //* запись в монго

    bson_t *doc = bson_new();

    BSON_APPEND_UTF8(doc, "filename", req->filename);
    BSON_APPEND_INT64(doc, "size", req->filesize);
    BSON_APPEND_BOOL(doc, "deleted", false);
    BSON_APPEND_DATE_TIME(doc, "uploaded_at", bson_get_monotonic_time() / 1000);

    bson_error_t error;

    if(collection == NULL) {

        fprintf(stderr, "collection is null before mongoc_collection_insert_one\n");

    } else {

        fprintf(stderr, "collection is not null before mongoc_collection_insert_one\n");

    }

    bool success = mongoc_collection_insert_one(collection, doc, NULL, NULL, &error);

    bson_destroy(doc);

    resp.status = success ? RESP_SUCCESS : RESP_ERROR;

    send_all(client_fd, &resp, sizeof(resp));

}

int main() {

    mongoc_init();

    int server_fd;
    struct sockaddr_in serv_addr;

    if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("error create socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(server_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if(listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    
    printf("server listening on port %d\n", PORT);

    //* главный цикл обработки событий

    while(1) {

        struct sockaddr_in client_addr;

        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if(client_fd == -1) {

            perror("error accept!");

            continue;
        }

        printf("new client conn: %s:%d\n", inet_ntoa(client_addr.sin_addr), htons(client_addr.sin_port));

        client_info_t *info = malloc(sizeof(client_info_t));

        if(!info) {

            perror("malloc!");
            close(client_fd);

            continue;

        }

        info->client_socket = client_fd;
        info->client_addr = client_addr;

        pthread_t tid;

        if (pthread_create(&tid, NULL, handle_client, info) != 0) {
            
            perror("error pthread!");
            free(info);
            close(client_fd);
            
            continue;
        }

        pthread_detach(tid);
        
    }

    close(server_fd);

    mongoc_cleanup();

    return 0;
}
