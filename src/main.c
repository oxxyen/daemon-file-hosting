// TODO: 

//* daemon script
#include <bits/types/struct_timeval.h>
#include <linux/limits.h>
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define PID_FILE "/tmp/exchange-daemon.pid"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bson/bson.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <limits.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/time.h>
#include <unistd.h>
#include <strings.h>
#include <signal.h>
#include <sys/param.h>
#include <limits.h>
#include <curl/curl.h>
#include <mongoc/mongoc.h>


//* локуальные заголовки
#include "db/mongo_ops.h"

//* директория за которой ведется наблюдение. 
#define EXCHANGE_DIR "/home/just/mesh_proto/oxxyen_storage/file_dir/filetrade"

//* размер буфера для чтения событий инотифай
#define EVENT_BUFFER_SIZE (sizeof(struct inotify_event) + NAME_MAX + 1)

#define MONGODB_URI "mongodb://localhost:27017"
#define DATABASE_NAME "file_exchange"
#define DELETE_REQUESTS_COLLECTION "file_groups"

//* флаг завершения работы, изменяемый обработчиком сигнала.
static volatile sig_atomic_t g_shutdown = 0;

static char* get_next_proc_key(mongoc_collection_t *coll, const char *file_id);

/**
 * * обработчик сигналов завершения(sigint, sigterm).
 * * устанавливает флаг g_shutdown, чтобы основной цикл корректно завершил работу
 */
static void signal_handler(int sig) {
    (void)sig; //* подавляем предупреждение о неиспользуемом параметре
    g_shutdown = 1;
}

//* глобальные указатели на монго ресы
mongoc_client_t *g_mongo_client = NULL;
// mongoc_collection_t *g_collection = NULL;

static bool is_regular_file(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

/**
 * Возвращает размер файла в байтах.
 *
 * @param path Путь к файлу.
 * @return Размер файла в байтах, либо 0 в случае ошибки (файл не существует, нет прав и т.д.).
 *
 * @warning Не различает "файл размером 0" и "ошибка". Для точной диагностики требуется
 *          отдельная проверка существования файла.
 */

static uint64_t get_file_size(const char *path) {

    struct stat st;
    return (stat(path, &st) == 0) ? (uint64_t)st.st_size : 0;

}


/**
 * Определяет MIME-тип файла по расширению.
 *
 * @param filename Имя файла (без пути).
 * @return Указатель на строку с MIME-типом. Строка статическая — не требует освобождения.
 *
 * @note Поддерживает только ограниченный набор типов. Все остальные помечаются как
 *       "application/octet-stream" — безопасное поведение по умолчанию.
 * @note Использует strcasecmp() для регистронезависимого сравнения расширений.
 */

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

char* get_file_extension(const char *full_path) {

    if (!full_path) return NULL;

    const char *filename = strrchr(full_path, '/');
    if (!filename) filename = full_path;
    else filename++;

    const char *dot = strrchr(filename, '.');

    if (!dot || dot == filename) {

        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;

    }

    size_t ext_len = strlen(dot);

    char *extension = malloc(ext_len + 1);

    if (!extension) {

        perror("malloc fail in get file exntesion");
        return NULL;

    }

    strcpy(extension, dot);
    return extension;

}


/**
 * Обрабатывает событие создания (или перемещения в директорию) файла.
 *
 * Проверяет, что путь указывает на обычный файл, извлекает имя, размер и MIME-тип,
 * затем сохраняет метаданные в MongoDB.
 *
 * @param fullpath Полный путь к файлу (включая EXCHANGE_DIR).
 *
 * @note Имя файла извлекается через strrchr(), что безопасно даже при отсутствии '/'.
 * @note При ошибке вставки в MongoDB выводится сообщение об ошибке, но выполнение продолжается.
 */

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

int signal_remote_delete(const char *filename) {

    if (!filename || !g_mongo_client) {

        return -1;

    }

    mongoc_database_t *database = mongoc_client_get_database(g_mongo_client, DATABASE_NAME);

    if (!database) {
        return -1;
    }    

    mongoc_collection_t *collection = mongoc_database_get_collection(database, DELETE_REQUESTS_COLLECTION);

    if (!collection) {

        mongoc_database_destroy(database);
        return -1;

    }

    char *file_id = get_next_proc_key(collection, filename);

    if (!file_id) {

        mongoc_collection_destroy(collection);
        mongoc_database_destroy(database);
        return -1;

    }

    bson_t *doc = bson_new();

    struct timeval tv;

    gettimeofday(&tv, NULL);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;


    BSON_APPEND_UTF8(doc, "_id", filename);
    BSON_APPEND_UTF8(doc, "file_id", file_id);
    BSON_APPEND_BOOL(doc, "deleted", true);
    BSON_APPEND_DATE_TIME(doc, "timestamp", now_ms);

    bson_error_t error;

    bool success = mongoc_collection_insert_one(collection, doc, NULL, NULL, &error);

    if (!success) {

        fprintf(stderr, "failed to insert deltete request for %s: %s\n", filename, error.message);

    }

    bson_destroy(doc);

    free(file_id);

    mongoc_collection_destroy(collection);

    mongoc_database_destroy(database);

    return success ? 0 : -1;
}


/**
 * Обрабатывает событие удаления файла из директории.
 *
 * На данный момент только выводит сообщение. В будущем здесь должна быть реализована
 * логика пометки файла как удалённого в MongoDB и, возможно, уведомление удалённых узлов.
 *
 * @param filename Имя удалённого файла (без пути).
 */

static void handle_file_deleted(const char *filename) {
    // TODO: mongo_mark_deleted(filename);
    // TODO: signal_remote_delete(filename);
    printf("DELETE_SIGNAL: %s\n", filename);
}

/**
 * Обновляет или вставляет запись в коллекцию file_groups
 * Использует _id = filename для уникальности
 */
static char* get_next_proc_key(mongoc_collection_t *coll, const char *file_id) {

    bson_t *query = bson_new();
    BSON_APPEND_UTF8(query, "_id", file_id);

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(coll, query, NULL, NULL);
    const bson_t *doc;
    int64_t max_key = 0;

    if (mongoc_cursor_next(cursor, &doc)) {
        
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "proc") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {

            bson_iter_t child;
            bson_iter_recurse(&iter, &child);

            while (bson_iter_next(&child)) {

                const char *key = bson_iter_key(&child);
                char *end;
                long num = strtol(key, &end, 10);

                if (*end == '\0' && num > max_key) {

                    max_key = num;

                }

            }
        }
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(query);

    char *next_key = malloc(32);
    snprintf(next_key, 32, "%" PRId64, max_key + 1);
    return next_key; // free будет вызван вызывающей стороной
}

//* убираем  расширение ('.') в файле(отправляем только название)
char* get_filename_without_extension_no_dot(const char* full_filename) {

    const char *filename = strrchr(full_filename, '/');

    if (filename == NULL) {

        filename = strrchr(full_filename, '\\');

        if (filename == NULL) {

            filename = full_filename;

        } else filename++;

    } else filename++;

    const char *dt = strrchr(filename, '.');
    size_t filename_length;

    if (dt == NULL) {

        filename_length = strlen(filename);

        char *filename_without_extension = (char*)malloc(filename_length + 1);

        if (filename_without_extension == NULL) {

            perror("error in memory");
            return NULL;

        }

        strncpy(filename_without_extension, filename, filename_length);
        filename_without_extension[filename_length] = '\0';

        return filename_without_extension;

    } else {
        filename_length = dt - filename;

        char *filename_without_extension = (char*)malloc(filename_length + 1);

        if (filename_without_extension == NULL) {
            
            perror("error in memory");
            return NULL;
        }

        strncpy(filename_without_extension, filename, filename_length);
        filename_without_extension[filename_length] = '\0';

        return filename_without_extension;
    }

}

static bool update_file_group_v2(const char *full_filename, const char *change_type, const char *status, int64_t load_tag) {
    printf("TEST: update_file_group_v2 called for %s\n", full_filename);

    // Просто создаем документ с фиксированным содержимым
    mongoc_collection_t *coll = mongoc_client_get_collection(g_mongo_client, DATABASE_NAME, DELETE_REQUESTS_COLLECTION);
    if (!coll) {
        printf("ERROR: can't get collection\n");
        return false;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    //*  название файла без расширения
    char *filename = get_filename_without_extension_no_dot(full_filename);
    char *extension = get_file_extension(full_filename);
    bson_t *doc = bson_new();
    BSON_APPEND_UTF8(doc, "_id", full_filename);
    BSON_APPEND_UTF8(doc, "filename", filename);
    BSON_APPEND_UTF8(doc, "extension", extension);
    bson_t proc;
    bson_init(&proc);
    bson_t entry;
    bson_init(&entry);
    BSON_APPEND_DATE_TIME(&entry, "date", now_ms);
    bson_t info;
    bson_init(&info);
    BSON_APPEND_UTF8(&info, "type_of_changes", change_type);
    BSON_APPEND_UTF8(&info, "status", status);
    BSON_APPEND_NULL(&info, "load_tag");
    BSON_APPEND_DOCUMENT(&entry, "info", &info);
    BSON_APPEND_DOCUMENT(&proc, "1", &entry);
    BSON_APPEND_DOCUMENT(doc, "proc", &proc);

    bson_error_t error;
    bool success = mongoc_collection_insert_one(coll, doc, NULL, NULL, &error);

    if (!success) {
        printf("ERROR: insert failed: %s\n", error.message);
    } else {
        printf("SUCCESS: inserted document for %s\n", full_filename);
    }

    bson_destroy(&info);
    bson_destroy(&entry);
    bson_destroy(&proc);
    bson_destroy(doc);
    mongoc_collection_destroy(coll);

    return success;
}

/*
 * Точка входа демона обмена файлами.
 *
 * Выполняет следующие этапы:
 * 1. Проверка на уже запущенный экземпляр через PID-файл.
 * 2. Запись собственного PID в файл.
 * 3. Демонизация процесса.
 * 4. Перенаправление stdout/stderr в лог-файл.
 * 5. Инициализация MongoDB-клиента и коллекции.
 * 6. Настройка обработчиков сигналов завершения.
 * 7. Инициализация inotify и установка наблюдения за EXCHANGE_DIR.
 * 8. Основной цикл обработки событий файловой системы.
 * 9. Корректное освобождение ресурсов при завершении.
 *
 * @return EXIT_SUCCESS при нормальном завершении, EXIT_FAILURE — при ошибке.
 */

int main(void) {

    //* проверка на уже запущенный экземляр демона через пид-файл
    FILE *pidfp = fopen(PID_FILE, "r");
    if(pidfp) {

        pid_t old_pid;

        //* если сканф успешно прочитан пид и процесс с таким пид уже существуеt(kill(pid, 0) == 0)
        if(fscanf(pidfp, "%d", &old_pid) == 1 && kill(old_pid, 0) == 0) {

            fclose(pidfp);
            fprintf(stderr, "daemon already running(PID %d)\n", (int)old_pid);

            return EXIT_FAILURE;

        }

        fclose(pidfp);
    }

    //* записываем текущий пид в файл
    pidfp = fopen(PID_FILE, "w");

    if(!pidfp) return EXIT_FAILURE;

    fprintf(pidfp, "%d\n", (int)getpid());
    fclose(pidfp);

    
    //* демонизируем процесс закрываем stdin, отсоединяем от терминала
    if (daemon(1, 1) == -1) {
        
        perror("daemon");
        return EXIT_FAILURE;

    }
    
    //* перенаправляем стандарт., потоки в лог-файл для откладки 
    freopen("/tmp/exchange-daemon.log", "a", stdout);
    freopen("/tmp/exchange-daemon.log", "a", stderr);


    //* настройки обработчиков сигналов
    struct sigaction sa = {0};


    //* инициализация монгодб с драйвер
    mongoc_init();

    g_mongo_client = mongoc_client_new(MONGODB_URI);
    
    if (!g_mongo_client) {

        fprintf(stderr, "Failed to connect to MongoDB\n");
        return EXIT_FAILURE;

    }

    g_collection = mongoc_client_get_collection(g_mongo_client, DATABASE_NAME, DELETE_REQUESTS_COLLECTION);
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
    IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF);
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

            if (e->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) {

                char fullpath[PATH_MAX];
                int res = snprintf(fullpath, sizeof(fullpath), "%s/%s", EXCHANGE_DIR, e->name);
                if(res < 0 || (size_t)res >= sizeof(fullpath)) {
                     
                    fprintf(stderr," path to long or snprintf error: %s/%s\n", EXCHANGE_DIR, e->name);
                    continue;

                }

                handle_file_created(fullpath);
                update_file_group_v2(e->name, "add", "success", -1);

            }

            else if (e->mask & (IN_MOVED_TO | IN_DELETE)) {
                
                handle_file_deleted(e->name);
                update_file_group_v2(e->name, "deleted", "n/a", -1);

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