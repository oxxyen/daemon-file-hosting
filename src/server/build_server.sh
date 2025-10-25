# gcc -o server server.c \
#     -I/usr/include/libmongoc-1.0 \
#     -I/usr/include/libbson-1.0 \
#     -L/usr/lib \
#     -lmongoc-1.0 -lbson-1.0 -lssl -lcrypto -lsasl2 -lz
# gcc -o server server.c ../db/mongo_ops.c $(pkg-config --cflags --libs libmongoc-1.0) -lpthread

# Компилируем все .c файлы в .o файлы
gcc -c server.c -o server.o -Wall -Wextra $(pkg-config --cflags libmongoc-1.0)
gcc -c ../db/mongo_ops_server.c -o mongo_ops_server.o -Wall -Wextra $(pkg-config --cflags libmongoc-1.0)
gcc -c ../utils/utils.c -o utils.o -Wall -Wextra
gcc -c ../crypto/aes_gcm.c -o aes_gcm.o

# Линкуем все .o файлы и библиотеки
gcc -o server server.o mongo_ops_server.o utils.o aes_gcm.o $(pkg-config --libs libmongoc-1.0) -lssl -lcrypto
