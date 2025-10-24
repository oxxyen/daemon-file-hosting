#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h> // Для stat и получения размера файла
#include <errno.h>

#include "../../include/protocol.h"

#define DEFAULT_PORT 2222
#define BUFFER_SIZE 4096 // Размер буфера для чтения/записи файлов
#define FILENAME_MAX_LEN 256 // Максимальная длина имени файла

/**
 * @brief Отправляет файл на сервер.
 * @param sockfd Файловый дескриптор сокета.
 * @param local_filepath Путь к локальному файлу для отправки.
 * @param remote_filename Имя файла на сервере.
 * @return 0 в случае успеха, -1 в случае ошибки.
 */
int upload_file(int sockfd, const char *local_filepath, const char *remote_filename) {
    FILE *fp = NULL;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    struct stat st;
    RequestHeader header;
    ResponseHeader response;

    // 1. Получаем размер файла
    if (stat(local_filepath, &st) == -1) {
        perror("stat");
        fprintf(stderr, "Error: Could not get file size for %s\n", local_filepath);
        return -1;
    }
    long long filesize = st.st_size;

    // 2. Открываем локальный файл
    fp = fopen(local_filepath, "rb"); // "rb" для бинарного чтения
    if (fp == NULL) {
        perror("fopen");
        fprintf(stderr, "Error: Could not open file %s for reading.\n", local_filepath);
        return -1;
    }

    // 3. Формируем и отправляем заголовок запроса
    header.command = CMD_UPLOAD;
    strncpy(header.filename, remote_filename, FILENAME_MAX_LEN - 1);
    header.filename[FILENAME_MAX_LEN - 1] = '\0';
    header.filesize = filesize;

    printf("Uploading '%s' (%lld bytes) as '%s'...\n", local_filepath, filesize, remote_filename);
    if (send_all(sockfd, &header, sizeof(RequestHeader)) == -1) {
        fclose(fp);
        return -1;
    }

    // 4. Получаем ответ от сервера о готовности к приему
    if (recv_all(sockfd, &response, sizeof(ResponseHeader)) == -1) {
        fclose(fp);
        return -1;
    }

    if (response.status != RESP_SUCCESS) {
        fprintf(stderr, "Server rejected upload: Status %d\n", response.status);
        fclose(fp);
        return -1;
    }

    printf("Server ready for upload. Sending file data...\n");

    // 5. Отправляем данные файла
    long long total_sent = 0;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
        if (send_all(sockfd, buffer, bytes_read) == -1) {
            fprintf(stderr, "Failed to send file data.\n");
            fclose(fp);
            return -1;
        }
        total_sent += bytes_read;
        // Можно добавить индикатор прогресса: printf("\rSent %lld / %lld bytes", total_sent, filesize); fflush(stdout);
    }
    printf("\n"); // Для новой строки после прогресса

    if (ferror(fp)) {
        perror("fread");
        fprintf(stderr, "Error reading from local file %s.\n", local_filepath);
        fclose(fp);
        return -1;
    }

    printf("File data sent. Total: %lld bytes.\n", total_sent);

    // 6. Получаем окончательный ответ от сервера
    if (recv_all(sockfd, &response, sizeof(ResponseHeader)) == -1) {
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

/**
 * @brief Запрашивает и получает файл от сервера.
 * @param sockfd Файловый дескриптор сокета.
 * @param remote_filename Имя файла на сервере для скачивания.
 * @param local_filepath Путь для сохранения локального файла.
 * @return 0 в случае успеха, -1 в случае ошибки.
 */
int download_file(int sockfd, const char *remote_filename, const char *local_filepath) {
    FILE *fp = NULL;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    RequestHeader header;
    ResponseHeader response;

    // 1. Формируем и отправляем заголовок запроса
    header.command = CMD_DOWNLOAD;
    strncpy(header.filename, remote_filename, FILENAME_MAX_LEN - 1);
    header.filename[FILENAME_MAX_LEN - 1] = '\0';
    header.filesize = 0; // Не используется для запроса скачивания

    printf("Requesting download of '%s' to '%s'...\n", remote_filename, local_filepath);
    if (send_all(sockfd, &header, sizeof(RequestHeader)) == -1) {
        return -1;
    }

    // 2. Получаем ответ от сервера
    if (recv_all(sockfd, &response, sizeof(ResponseHeader)) == -1) {
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

    // 3. Открываем локальный файл для записи
    fp = fopen(local_filepath, "wb"); // "wb" для бинарной записи
    if (fp == NULL) {
        perror("fopen");
        fprintf(stderr, "Error: Could not open file %s for writing.\n", local_filepath);
        return -1;
    }

    // 4. Получаем и записываем данные файла
    long long total_received = 0;
    while (total_received < filesize) {
        size_t bytes_to_read = (filesize - total_received < BUFFER_SIZE) ? (filesize - total_received) : BUFFER_SIZE;
        bytes_received = recv(sockfd, buffer, bytes_to_read, 0);

        if (bytes_received == -1) {
            perror("recv");
            fclose(fp);
            return -1;
        }
        if (bytes_received == 0) {
            fprintf(stderr, "Server closed connection unexpectedly during download.\n");
            fclose(fp);
            return -1;
        }

        if (fwrite(buffer, 1, bytes_received, fp) != bytes_received) {
            perror("fwrite");
            fprintf(stderr, "Error writing to local file %s.\n", local_filepath);
            fclose(fp);
            return -1;
        }
        total_received += bytes_received;
        // Можно добавить индикатор прогресса
        // printf("\rReceived %lld / %lld bytes", total_received, filesize); fflush(stdout);
    }
    printf("\n"); // Для новой строки после прогресса

    if (ferror(fp)) {
        perror("ferror");
        fprintf(stderr, "Error during file writing to %s.\n", local_filepath);
        fclose(fp);
        return -1;
    }

    printf("Download completed successfully! Saved to '%s'. Total: %lld bytes.\n", local_filepath, total_received);
    fclose(fp);
    return 0;
}

/**
 * @brief Запрашивает список файлов с сервера.
 * @param sockfd Файловый дескриптор сокета.
 * @return 0 в случае успеха, -1 в случае ошибки.
 */
int list_files(int sockfd) {
    RequestHeader header;
    ResponseHeader response;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    // 1. Формируем и отправляем заголовок запроса
    header.command = CMD_LIST;
    header.filename[0] = '\0'; // Имя файла не нужно для LIST
    header.filesize = 0; // Размер файла не нужен для LIST

    printf("Requesting file list from server...\n");
    if (send_all(sockfd, &header, sizeof(RequestHeader)) == -1) {
        return -1;
    }

    // 2. Получаем ответ от сервера
    if (recv_all(sockfd, &response, sizeof(ResponseHeader)) == -1) {
        return -1;
    }

    if (response.status != RESP_SUCCESS) {
        fprintf(stderr, "Server rejected list request: Status %d\n", response.status);
        return -1;
    }

    long long list_len = response.filesize; // Здесь filesize используется для длины списка
    if (list_len <= 0) {
        printf("No files found on server.\n");
        return 0;
    }

    printf("File list from server (%lld bytes):\n", list_len);

    // 3. Получаем и выводим список файлов
    long long total_received = 0;
    while (total_received < list_len) {

        size_t bytes_to_read = (list_len - total_received < BUFFER_SIZE) ? (list_len - total_received) : BUFFER_SIZE;
        bytes_received = recv(sockfd, buffer, bytes_to_read, 0);

        if (bytes_received == -1) {

            perror("recv");
            return -1;

        }
        if (bytes_received == 0) {

            fprintf(stderr, "Server closed connection unexpectedly during list receive.\n");
            return -1;

        }

        fwrite(buffer, 1, bytes_received, stdout); // Выводим сразу в stdout
        total_received += bytes_received;
    }

    printf("\n"); // Добавляем новую строку в конце списка

    return 0;
}


int main(int argc, char *argv[]) {

    int sock = 0;
    struct sockaddr_in serv_addr;
    char *server_ip = "127.0.0.1"; // IP сервера по умолчанию
    int port = DEFAULT_PORT;

    if (argc < 2) {

        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
        fprintf(stderr, "Commands:\n");
        fprintf(stderr, "  %s upload <local_filepath> <remote_filename>\n", argv[0]);
        fprintf(stderr, "  %s download <remote_filename> <local_filepath>\n", argv[0]);
        fprintf(stderr, "  %s list\n", argv[0]);
        fprintf(stderr, "Optional: Specify server IP and port using --ip <ip> --port <port>\n");

        return EXIT_FAILURE;
    }

    // Парсинг аргументов командной строки для IP и порта
    for (int i = 1; i < argc; ++i) {

        if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {

            server_ip = argv[++i];

        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {

            port = atoi(argv[++i]);

        }
    }


    // 1. Создаем сокет
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {

        perror("Socket creation error");
        return EXIT_FAILURE;

    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // 2. Преобразуем IP-адрес из строкового в бинарный формат
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {

        perror("Invalid address/ Address not supported");
        close(sock);

        return EXIT_FAILURE;

    }

    // 3. Подключаемся к серверу
    printf("Connecting to %s:%d...\n", server_ip, port);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {

        perror("Connection Failed");
        close(sock);
        
        return EXIT_FAILURE;

    }

    printf("Connected to server.\n");

    // 4. Обрабатываем команды
    CommandType cmd = CMD_UNKNOWN;
    char *cmd_str = argv[1];

    if (strcmp(cmd_str, "upload") == 0) {
        cmd = CMD_UPLOAD;

        if (argc < 4) { // argv[0] client, argv[1] upload, argv[2] local_path, argv[3] remote_name

            fprintf(stderr, "Usage: %s upload <local_filepath> <remote_filename>\n", argv[0]);
            close(sock);

            return EXIT_FAILURE;

        }

        if (upload_file(sock, argv[2], argv[3]) != 0) {

            fprintf(stderr, "Upload operation failed.\n");
            close(sock);
            
            return EXIT_FAILURE;

        }

    } else if (strcmp(cmd_str, "download") == 0) {
        cmd = CMD_DOWNLOAD;

        if (argc < 4) { // argv[0] client, argv[1] download, argv[2] remote_name, argv[3] local_path

            fprintf(stderr, "Usage: %s download <remote_filename> <local_filepath>\n", argv[0]);
            close(sock);
            
            return EXIT_FAILURE;
            
        }

        if (download_file(sock, argv[2], argv[3]) != 0) {

            fprintf(stderr, "Download operation failed.\n");
            close(sock);

            return EXIT_FAILURE;
        }

    } else if (strcmp(cmd_str, "list") == 0) {
        cmd = CMD_LIST;

        if (list_files(sock) != 0) {

            fprintf(stderr, "List operation failed.\n");
            close(sock);
            return EXIT_FAILURE;

        }

    } else {

        fprintf(stderr, "Unknown command: %s\n", cmd_str);
        close(sock);
        
        return EXIT_FAILURE;

    }

    // 5. Закрываем сокет
    close(sock);
    return EXIT_SUCCESS;
}
