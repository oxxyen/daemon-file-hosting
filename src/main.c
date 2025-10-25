// TODO: 

//* daemon script
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


//* локуальные заголовки
#include "db/mongo_ops.h"

//* директория за которой ведется наблюдение. 
#define EXCHANGE_DIR "/home/just/mesh_proto/oxxyen_storage/file_dir/filetrade"

//* размер буфера для чтения событий инотифай
#define EVENT_BUFFER_SIZE (sizeof(struct inotify_event) + NAME_MAX + 1)

//* флаг завершения работы, изменяемый обработчиком сигнала.
static volatile sig_atomic_t g_shutdown = 0;


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
mongoc_collection_t *g_collection = NULL;

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