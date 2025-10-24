# gcc -o server server.c \
#     -I/usr/include/libmongoc-1.0 \
#     -I/usr/include/libbson-1.0 \
#     -L/usr/lib \
#     -lmongoc-1.0 -lbson-1.0 -lssl -lcrypto -lsasl2 -lz
# gcc -o server server.c ../db/mongo_ops.c $(pkg-config --cflags --libs libmongoc-1.0) -lpthread

    gcc -c server.c -Wall -Wextra $(pkg-config --cflags libmongoc-1.0)
    gcc -c ../db/mongo_ops.c -Wall -Wextra $(pkg-config --cflags libmongoc-1.0)
    gcc -c ../utils/utils.c -Wall -Wextra
    gcc -o server server.o mongo_ops.o utils.o $(pkg-config --libs libmongoc-1.0)
    