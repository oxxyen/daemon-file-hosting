/*
 * File Exchange Client with mTLS
 * 
 * This client establishes a mutual TLS connection to the server,
 * enabling authenticated and encrypted file operations.
 * 
 * Dependencies:
 *   - OpenSSL (for mTLS)
 *   - protocol.h (shared command definitions)
 * 
 * Required files in working directory:
 *   - ca.pem        : CA certificate to verify server
 *   - client-cert.pem : Client certificate for mTLS
 *   - client-key.pem  : Client private key
 */

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <errno.h>

#include "../../include/protocol.h"

#define DEFAULT_PORT 3131
#define BUFFER_SIZE 4096
#define FILENAME_MAX_LEN 256

/*
 * Initialize OpenSSL client context for mTLS.
 * Loads CA, client certificate, and private key.
 * Exits on failure to ensure secure operation.
 */
static SSL_CTX *init_client_ssl_ctx(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    /* Load CA certificate to verify server identity */
    if (SSL_CTX_load_verify_locations(ctx, "../ca.pem", NULL) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    /* Load client certificate and private key for mTLS */
    if (SSL_CTX_use_certificate_file(ctx, "../client-cert.pem", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, "../client-key.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    /* Enforce server certificate verification */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    return ctx;
}

/*
 * Reliable SSL write: ensures all bytes are sent.
 */
static int ssl_send_all(SSL *ssl, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = SSL_write(ssl, (const char *)buf + sent, len - sent);
        if (n <= 0) {
            return -1;
        }
        sent += n;
    }
    return 0;
}

/*
 * Reliable SSL read: ensures all expected bytes are received.
 */
static int ssl_recv_all(SSL *ssl, void *buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        int n = SSL_read(ssl, (char *)buf + received, len - received);
        if (n <= 0) {
            return -1;
        }
        received += n;
    }
    return 0;
}

/*
 * Upload a file to the server over mTLS.
 * Uses the secure SSL channel for all communication.
 */
static int upload_file_ssl(SSL *ssl, const char *local_filepath, const char *remote_filename) {
    FILE *fp = NULL;
    char buffer[BUFFER_SIZE];
    struct stat st;
    RequestHeader header;
    ResponseHeader response;

    if (stat(local_filepath, &st) == -1) {
        perror("stat");
        fprintf(stderr, "Error: Could not get file size for %s\n", local_filepath);
        return -1;
    }
    long long filesize = st.st_size;

    fp = fopen(local_filepath, "rb");
    if (!fp) {
        perror("fopen");
        fprintf(stderr, "Error: Could not open file %s for reading.\n", local_filepath);
        return -1;
    }

    /* Send upload request header */
    header.command = CMD_UPLOAD;
    strncpy(header.filename, remote_filename, FILENAME_MAX_LEN - 1);
    header.filename[FILENAME_MAX_LEN - 1] = '\0';
    header.filesize = filesize;

    printf("Uploading '%s' (%lld bytes) as '%s'...\n", local_filepath, filesize, remote_filename);
    if (ssl_send_all(ssl, &header, sizeof(RequestHeader)) == -1) {
        fclose(fp);
        return -1;
    }

    /* Wait for server readiness confirmation */
    if (ssl_recv_all(ssl, &response, sizeof(ResponseHeader)) == -1) {
        fclose(fp);
        return -1;
    }

    if (response.status != RESP_SUCCESS) {
        fprintf(stderr, "Server rejected upload: Status %d\n", response.status);
        fclose(fp);
        return -1;
    }

    printf("Server ready for upload. Sending file data...\n");

    /* Send file content */
    long long total_sent = 0;
    ssize_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
        if (ssl_send_all(ssl, buffer, bytes_read) == -1) {
            fprintf(stderr, "Failed to send file data.\n");
            fclose(fp);
            return -1;
        }
        total_sent += bytes_read;
    }

    if (ferror(fp)) {
        perror("fread");
        fprintf(stderr, "Error reading from local file %s.\n", local_filepath);
        fclose(fp);
        return -1;
    }

    printf("File data sent. Total: %lld bytes.\n", total_sent);

    /* Receive final upload status */
    if (ssl_recv_all(ssl, &response, sizeof(ResponseHeader)) == -1) {
        fclose(fp);
        return -1;
    }

    if (response.status == RESP_SUCCESS) {
        printf("Upload completed successfully!\n");
    } else {
        fprintf(stderr, "Upload failed on server: Status %d\n", response.status);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/*
 * Download a file from the server over mTLS.
 */
static int download_file_ssl(SSL *ssl, const char *remote_filename, const char *local_filepath) {
    FILE *fp = NULL;
    char buffer[BUFFER_SIZE];
    RequestHeader header;
    ResponseHeader response;

    /* Send download request */
    header.command = CMD_DOWNLOAD;
    strncpy(header.filename, remote_filename, FILENAME_MAX_LEN - 1);
    header.filename[FILENAME_MAX_LEN - 1] = '\0';
    header.filesize = 0;

    printf("Requesting download of '%s' to '%s'...\n", remote_filename, local_filepath);
    if (ssl_send_all(ssl, &header, sizeof(RequestHeader)) == -1) {
        return -1;
    }

    /* Receive file metadata */
    if (ssl_recv_all(ssl, &response, sizeof(ResponseHeader)) == -1) {
        return -1;
    }

    if (response.status != RESP_SUCCESS) {
        fprintf(stderr, "Server rejected download request: Status %d\n", response.status);
        return -1;
    }

    long long filesize = response.filesize;
    if (filesize <= 0) {
        fprintf(stderr, "Server reported invalid file size (%lld) for download.\n", filesize);
        return -1;
    }

    printf("Server has file '%s' (%lld bytes). Starting download...\n", remote_filename, filesize);

    fp = fopen(local_filepath, "wb");
    if (!fp) {
        perror("fopen");
        fprintf(stderr, "Error: Could not open file %s for writing.\n", local_filepath);
        return -1;
    }

    /* Receive file content */
    long long total_received = 0;
    while (total_received < filesize) {
        size_t bytes_to_read = (filesize - total_received < BUFFER_SIZE) ? 
                               (size_t)(filesize - total_received) : BUFFER_SIZE;
        int bytes_received = SSL_read(ssl, buffer, bytes_to_read);
        if (bytes_received <= 0) {
            perror("SSL_read");
            fclose(fp);
            return -1;
        }

        if (fwrite(buffer, 1, bytes_received, fp) != (size_t)bytes_received) {
            perror("fwrite");
            fprintf(stderr, "Error writing to local file %s.\n", local_filepath);
            fclose(fp);
            return -1;
        }
        total_received += bytes_received;
    }

    if (ferror(fp)) {
        perror("ferror");
        fprintf(stderr, "Error during file writing to %s.\n", local_filepath);
        fclose(fp);
        return -1;
    }

    printf("Download completed successfully! Saved to '%s'. Total: %lld bytes.\n", 
           local_filepath, total_received);
    fclose(fp);
    return 0;
}

/*
 * Request file list from server over mTLS.
 */
static int list_files_ssl(SSL *ssl) {
    RequestHeader header;
    ResponseHeader response;

    /* Send list request */
    header.command = CMD_LIST;
    header.filename[0] = '\0';
    header.filesize = 0;

    printf("Requesting file list from server...\n");
    if (ssl_send_all(ssl, &header, sizeof(RequestHeader)) == -1) {
        return -1;
    }

    /* Receive list metadata */
    if (ssl_recv_all(ssl, &response, sizeof(ResponseHeader)) == -1) {
        return -1;
    }

    if (response.status != RESP_SUCCESS) {
        fprintf(stderr, "Server rejected list request: Status %d\n", response.status);
        return -1;
    }

    long long list_len = response.filesize;
    if (list_len <= 0) {
        printf("No files found on server.\n");
        return 0;
    }

    printf("File list from server (%lld bytes):\n", list_len);

    /* Receive and display file list */
    long long total_received = 0;
    char buffer[BUFFER_SIZE];
    while (total_received < list_len) {
        size_t bytes_to_read = (list_len - total_received < BUFFER_SIZE) ? 
                               (size_t)(list_len - total_received) : BUFFER_SIZE;
        int bytes_received = SSL_read(ssl, buffer, bytes_to_read);
        if (bytes_received <= 0) {
            perror("SSL_read");
            return -1;
        }

        fwrite(buffer, 1, bytes_received, stdout);
        total_received += bytes_received;
    }

    printf("\n");
    return 0;
}

int main(int argc, char *argv[]) {
    struct sockaddr_in serv_addr;
    char *server_ip = "127.0.0.1";
    int port = DEFAULT_PORT;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
        fprintf(stderr, "Commands:\n");
        fprintf(stderr, "  %s upload <local_filepath> <remote_filename>\n", argv[0]);
        fprintf(stderr, "  %s download <remote_filename> <local_filepath>\n", argv[0]);
        fprintf(stderr, "  %s list\n", argv[0]);
        fprintf(stderr, "Optional: --ip <ip> --port <port>\n");
        return EXIT_FAILURE;
    }

    /* Parse command-line arguments */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            server_ip = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
    }

    /* Create TCP socket */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation error");
        return EXIT_FAILURE;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        close(sock);
        return EXIT_FAILURE;
    }

    /* Connect to server */
    printf("Connecting to %s:%d...\n", server_ip, port);
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        close(sock);
        return EXIT_FAILURE;
    }

    /* Initialize mTLS */
    SSL_CTX *ctx = init_client_ssl_ctx();
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        ERR_print_errors_fp(stderr);
        close(sock);
        return EXIT_FAILURE;
    }

    SSL_set_fd(ssl, sock);
    if (SSL_connect(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(sock);
        return EXIT_FAILURE;
    }

    printf("mTLS handshake successful.\n");

    /* Execute requested command */
    char *cmd_str = argv[1];
    int result = EXIT_FAILURE;

    if (strcmp(cmd_str, "upload") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s upload <local_filepath> <remote_filename>\n", argv[0]);
        } else {
            result = upload_file_ssl(ssl, argv[2], argv[3]) ? EXIT_FAILURE : EXIT_SUCCESS;
        }
    } else if (strcmp(cmd_str, "download") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s download <remote_filename> <local_filepath>\n", argv[0]);
        } else {
            result = download_file_ssl(ssl, argv[2], argv[3]) ? EXIT_FAILURE : EXIT_SUCCESS;
        }
    } else if (strcmp(cmd_str, "list") == 0) {
        result = list_files_ssl(ssl) ? EXIT_FAILURE : EXIT_SUCCESS;
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd_str);
    }

    /* Cleanup */
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sock);
    return result;
}
