#include <errno.h>
#include <linux/limits.h>
#include <openssl/err.h>
#include <stdint.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/rand.h>
#include <mongoc/mongoc.h>
#include <openssl/ssl.h>

#define BLAKE3_IMPLEMENTATION
#include "blake3.h"

//* подмодули
#include "../db/mongo_ops.h"
#include "../../include/protocol.h"
#include "../crypto/aes_gcm.h"

//* определяем глобальные указатели монго
mongoc_client_t *g_mongo_client = NULL;
mongoc_collection_t *g_collection = NULL;
mongoc_collection_t *collection;
SSL_CTX *g_ssl_ctx = NULL; //* Контекст OPENSSL для MTLS

//* константы параметры
#define PORT             7777       //* Порт прослушивания 
#define RECV_BUFFER_SIZE 4096      //* Размер буфера для обычного recv
#define BUFFER_SIZE      4096     //* Размер буфера для чтения файла

//* информация о подключившемся клиенте
typedef struct {

    int client_socket;
    struct sockaddr_in client_addr;

} client_info_t;

//* Контест шифрования файлов(aes-256-gcm)
//* используется глобальный ключ
typedef struct {

    uint8_t key[32];    //* 256 битный ключ
    int initialized;   //* флаг инициализации

} file_crypto_ctx_t;

static file_crypto_ctx_t g_file_crypto = {0};

//* объявления обработчиков команд
void handle_upload_request(SSL *ssl, RequestHeader *req, const char *storage_dir );
void handle_list_request(SSL *ssl, mongoc_collection_t *collection);
int ssl_recv_all(SSL *ssl, void *buffer, size_t len);

/*
* ssl write ensures all bytes are sent :3
*/

static int ssl_send_all(SSL *ssl, const void *buf, size_t len) {

    size_t sent = 0;

    while (sent < len) {

        int n = SSL_write(ssl, (const char *)buf + sent, len - sent);

        if ( n <= 0 ) {

            return -1;

        }
        sent += n;

    }

    return 0;

}

//* blake3 compute file

static void compute_buffer_blake3(const uint8_t *data, size_t len, uint8_t out_hash[BLAKE3_HASH_LEN]) {

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data,len);
    blake3_hasher_finalize(&hasher, out_hash, BLAKE3_HASH_LEN);

}

//* обработка одного клиента(отдельный поток)
void *handle_client(void *arg) {

    client_info_t *info = (client_info_t *)arg;

    int client_fd = info->client_socket;

    //* создаем ssl-обьект для данного соединения
    SSL *ssl = SSL_new(g_ssl_ctx);

    if (!ssl) {

        perror("ssl new fail\n");
        close(client_fd);
        free(info);
        return NULL;

    }

    //* Привязываем к сокету ССЛ объект
    SSL_set_fd(ssl, client_fd);

    if (SSL_accept(ssl) <= 0) {

        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(client_fd);
        free(info);
        return NULL;

    }

    printf("mhtls handshake success with %s:%d\n", inet_ntoa(info->client_addr.sin_addr), htons(info->client_addr.sin_port));


    printf("handing client on %d..\n", client_fd);

    //* монго создаем запись

    RequestHeader req;
    if (SSL_read(ssl, &req, sizeof(RequestHeader)) != sizeof(RequestHeader)) {

        perror("fail to receive req header ovet tls");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(client_fd);
        free(info);
        return NULL;

    }

    //* обработка команд
   // TODO: подключить ssl

    switch(req.command) {

        case CMD_UPLOAD:{

            printf("received UPLOAD command for file: %s (size: %lld)\n",req.filename, req.filesize);
            //* передаем ssl как сокет через каст
            handle_upload_request(ssl, &req, "../../filetrade");
            break;

        }

        case CMD_LIST: {

            printf("received LIST command\n");
            handle_list_request(ssl, g_collection);
            break;

        }

        case CMD_DOWNLOAD:
            
            printf("received download for '%s' (offset: %lld)\n", req.filename, req.offset);
            
            if(strstr(req.filename, "..") || strchr(req.filename, '/')) {

                ResponseHeader resp = { .status = RESP_PERMISSION_DENIED };
                ssl_send_all(ssl, &resp, sizeof(resp));
                break;

            }

            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "../../filetrade/%s", req.filename);

            struct stat st;

            if (stat(filepath, &st) != 0) {

                ResponseHeader resp = { .status = RESP_FILE_NOT_FOUND };

                ssl_send_all(ssl, &resp, sizeof(resp));
                break;

            }

            long long filesize = st.st_size;

            if (req.offset < 0 || req.offset > filesize) {

                ResponseHeader resp = { .status = RESP_INVALID_OFFSET };
                ssl_send_all(ssl, &req, sizeof(req));
                break;

            }

            FILE *fp = fopen(filepath, "rb");

            if (!fp) {

                ResponseHeader resp = { .status = RESP_ERROR };

                ssl_send_all(ssl, &req, sizeof(req));
                break;

            }

            if (fseek(fp, req.offset, SEEK_SET) != 0) {

                fclose(fp);
                ResponseHeader resp = { .status = RESP_ERROR };
                ssl_send_all(ssl, &req, sizeof(req));
                break;

            }

            ResponseHeader resp = { .status = RESP_SUCCESS, .filesize = filesize};

            ssl_send_all(ssl, &resp, sizeof(resp));

            long long bytes_left = filesize - req.offset;

            char buf[BUFFER_SIZE];

            while (bytes_left > 0) {

                size_t to_send = (bytes_left < BUFFER_SIZE) ? bytes_left : BUFFER_SIZE;
                
                if (fread(buf, 1, to_send, fp) != to_send) {

                    break;

                }

                if (ssl_send_all(ssl, buf, to_send) != 0) {


                    break;

                }

                bytes_left -= to_send;

            }

            fclose(fp);
            printf("sent %lld bytes of '%s' (from offset %lld)\n", filesize - req.offset, req.filename, req.offset);

            break;

        default:
            
            fprintf(stderr, "unknow command: %d\n", req.command);
            break;

    }

    SSL_shutdown(ssl);
    SSL_free(ssl);

    close(client_fd);
    free(info);

    return 0;
}

//* Отправка списка файлов клиенту (в формате JSON)
void handle_list_request (SSL *ssl, mongoc_collection_t *collection) {
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

    ssl_send_all(ssl, &resp, sizeof(resp));

    //* отправляем данные списка

    if(full_list) {

        ssl_send_all(ssl, full_list, total_len);
        free(full_list);

    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(query);

}

void handle_upload_request(SSL *ssl, RequestHeader *req, const char *storage_dir) {

    if(!g_file_crypto.initialized) {

        fprintf(stderr, "error: crypto context not init");
        ResponseHeader resp = { .status = RESP_ERROR };

        ssl_send_all(ssl, &resp, sizeof(resp));

        return;

    }

    //* проверка пути

    if (strstr(req->filename, "..") || strchr(req->filename, '/')) {

        ResponseHeader resp = { .status = RESP_PERMISSION_DENIED };
        ssl_send_all(ssl, &resp, sizeof(resp));

        return;

    }

    char filepath[PATH_MAX];

    snprintf(filepath, sizeof(filepath), "%s/%s", storage_dir, req->filename);

    ResponseHeader resp = { .status = RESP_SUCCESS };
    ssl_send_all(ssl, &resp, sizeof(resp));

    uint8_t *plaintext = malloc(req->filesize);

    if (!plaintext) {

        perror("malloc plaintext");
        resp.status = RESP_ERROR;
        ssl_send_all(ssl, &resp, sizeof(resp));

        return;

    }

    long long remainig = req->filesize;
    uint8_t *ptr = plaintext;

    while (remainig > 0) {

        size_t to_read = (remainig < BUFFER_SIZE) ? remainig : BUFFER_SIZE;

        if (ssl_recv_all((SSL*)(intptr_t)ssl, ptr, to_read) != (ssize_t)to_read) {

            free(plaintext);
            resp.status = RESP_ERROR;

            ssl_send_all(ssl, &req, sizeof(req));
            return;

        }

        ptr += to_read;
        remainig -= to_read;

    }

    uint8_t computed_hash[BLAKE3_HASH_LEN];
    compute_buffer_blake3(plaintext, req->filesize, computed_hash);

    if (memcmp(computed_hash, req->file_hash, BLAKE3_HASH_LEN) != 0) {

        fprintf(stderr, "integrity check failed for '%s'\n", req->filename);
        ResponseHeader resp = { .status = RESP_INTEGRITY_ERROR };
        ssl_send_all(ssl, &resp, sizeof(resp));
        
        free(plaintext);

        return;

    }

    //* шифруем

    uint8_t *ciphertext = malloc(req->filesize + 16); 
    uint8_t iv[12];
    uint8_t tag[16];

    int ct_len = crypto_encrypt_aes_gcm(

        plaintext, req->filesize,
        g_file_crypto.key,
        ciphertext, iv, tag

    );

    free(plaintext);

    if (ct_len < 0) {

        free(ciphertext);
        fprintf(stderr, "encrypt fail\n");
        resp.status = RESP_ERROR;
        ssl_send_all(ssl, &resp, sizeof(resp));
        return;

    }

    //* записываем зашифрован., файл на диск

    FILE *fp = fopen(filepath, "wb");

    if (!fp) {

        free(ciphertext);
        perror("fopen encrypt file");
        resp.status = RESP_ERROR;
        ssl_send_all(ssl, &resp, sizeof(resp));
        return;

    }

    fwrite(ciphertext, 1, ct_len, fp);

    fclose(fp);
    free(ciphertext);

    //* mongo
    
    bson_t *doc = bson_new();

    BSON_APPEND_UTF8(doc, "filename", req->filename);
    BSON_APPEND_INT64(doc, "size", req->filesize);
    BSON_APPEND_BOOL(doc, "encrypted", true);
    BSON_APPEND_BINARY(doc, "iv", BSON_SUBTYPE_BINARY, iv, 12);
    BSON_APPEND_BINARY(doc, "tag", BSON_SUBTYPE_BINARY, tag, 16);
    BSON_APPEND_BOOL(doc, "deleted", false);
    BSON_APPEND_DATE_TIME(doc, "uploaded_at", bson_get_monotonic_time() / 1000);

    bson_error_t error;

    bool success = mongoc_collection_insert_one(g_collection, doc, NULL, NULL, &error);

    bson_destroy(doc);

    resp.status = success ? RESP_SUCCESS : RESP_ERROR;

    if (!success) {

        fprintf(stderr, "mongo db insert error: %s\n", error.message);

    }

    ssl_send_all(ssl, &resp, sizeof(resp));
   

}

int ssl_recv_all(SSL *ssl, void *buffer, size_t len) {
    size_t total = 0;
    int bytes;

    while (total < len) {

        bytes = SSL_read(ssl, (char*)buffer + total, len - total);
        
        if (bytes <= 0) return -1;
        
        total += bytes;

    }

    return total;
}

int main() {

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    
    SSL_CTX *ctx = SSL_CTX_new(method);

    if (!ctx) {

        perror("error fail create SSL CTX");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);

    }

    const char *cert_file = "../server-cert.pem";
    const char *key_file = "../server-key.pem";
    const char *ca_file = "../ca.pem";

    //* загрузка сертификата и приватного ключа
    if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {

        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);


    }
    //* загружаем приватный ключ
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0) {

        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);

    }

    //* проверка приватного ключа 

    if (!SSL_CTX_check_private_key(ctx)) {

        fprintf(stderr, "private key no cerf!\n");
        exit(EXIT_FAILURE);

    }

    //* загрузка СА сертификата для проверки клиента

    if (SSL_CTX_load_verify_locations(ctx, ca_file, NULL) <= 0) {

        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);

    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);

    g_ssl_ctx = ctx;

    mongoc_init();

    if (!RAND_bytes(g_file_crypto.key, 32)) {

        fprintf(stderr, "failed to generate encrypt key!\n");

        exit(EXIT_FAILURE);

    }

    g_file_crypto.initialized = 1;

    g_mongo_client = mongoc_client_new("mongodb://localhost:27017");

    if(!g_mongo_client) {

        fprintf(stderr, "error: connect failed");

        exit(EXIT_FAILURE);

    }

    g_collection = mongoc_client_get_collection(g_mongo_client, "exchange_file", "file_overseer");

    if(!g_collection) {

        fprintf(stderr, "error: failed get collection");
        mongoc_client_destroy(g_mongo_client);

        exit(EXIT_FAILURE);

    }

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

    mongoc_collection_destroy(g_collection);
    mongoc_client_destroy(g_mongo_client);

    mongoc_cleanup();

    explicit_bzero(g_file_crypto.key, 32);

    return 0;
}
