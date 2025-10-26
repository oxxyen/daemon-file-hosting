# 🗄️ Директория `db` — Работа с базой данных MongoDB

Эта директория содержит модули для взаимодействия с **MongoDB** через официальный C-драйвер. Включает клиентскую обёртку, операции CRUD, а также утилиты для тестирования и сборки.

---

## 📁 Содержимое

- **`build.sh`** — Скрипт для компиляции и сборки модулей работы с MongoDB.  
  Запускает `gcc` с нужными флагами и линкует библиотеки (`libmongoc`, `libbson`).  
  *Пример использования:*  
  ```bash
  ./build.sh
  
  - **`mongo_client.c`** — Реализация основного клиента MongoDB.  
  Содержит функции для подключения к серверу, создания сессии, выполнения запросов.

- **`mongo_client.h`** — Заголовочный файл клиента.  
  Определяет структуры: `mongo_connection_t`, `mongo_result_t`, а также публичные функции:  
  - `mongo_connect()`  
  - `mongo_disconnect()`  
  - `mongo_get_collection()`

- **`mongo_ops_server.c`** — Операции, специфичные для серверной части (например, управление пользователями, индексами, репликацией).  
  Может включать функции типа `mongo_create_user()`, `mongo_add_index()` и т.д.

- **`mongo_ops.h`** — Общий интерфейс операций над коллекциями и документами.  
  Объявляет функции:  
  - `mongo_insert_document()`  
  - `mongo_find_documents()`  
  - `mongo_update_document()`  
  - `mongo_delete_document()`

- **`test_mongo.c`** — Тестовый модуль для проверки корректности работы с MongoDB.  
  Использует `assert()` или простые проверки для валидации:  
  - Подключения  
  - Вставки/поиска документов  
  - Ошибок при неверных параметрах

## 🧩 Как использовать

1. Убедитесь, что установлены зависимости:
```bash
sudo apt install libmongoc-1.0-dev libbson-1.0-dev
```
2. Подключите заголовки:
```C
#include "mongo_client.h"
#include "mongo_ops.h"
```
3. Пример подключения и вставки 
```C
mongo_connection_t *conn = mongo_connect("mongodb://localhost:27017");
if (!conn) { /* обработка ошибки */ }

bson_t *doc = BCON_NEW("name", BCON_UTF8("Alice"), "age", BCON_INT32(30));
mongo_insert_document(conn, "mydb.users", doc);

mongo_disconnect(conn);
bson_destroy(doc);
```

## ⚙️ Сборка

### Вы можете собрать всё вручную:
```bash
gcc -std=c99 -Wall -I/usr/include/libmongoc-1.0 -I/usr/include/libbson-1.0 \
    mongo_client.c mongo_ops.c test_mongo.c \
    -lmongoc-1.0 -lbson-1.0 -o test_mongo
```

### Или использовать скрипт:
```bash
./buld.sh
```
> 💡 Скрипт build.sh может автоматически определять пути к библиотекам через pkg-config.
