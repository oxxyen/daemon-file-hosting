gcc -o client client.c ../db/mongo_ops.c ../utils/utils.c -Wall -Wextra $(pkg-config --cflags --libs libmongoc-1.0) -lcrypto -lssl
   