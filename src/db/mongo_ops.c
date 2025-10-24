// db/mongo_ops.c
#include "mongo_ops.h"
#include <bits/time.h>
#include <mongoc/mongoc.h>
#include <bson/bson.h>
#include <string.h>

// mongoc_collection_t *g_collection = NULL;

bson_t* change_info_to_bson(const char *type, int64_t size_after) {

    bson_t* doc = bson_new();

    BSON_APPEND_UTF8(doc, "type_of_changes", type);
    BSON_APPEND_INT64(doc, "size_after", size_after);


    return doc;
}

bson_t* file_overseer_to_bson(const file_record_t *file) {
    bson_t *doc = bson_new();
    BSON_APPEND_UTF8(doc, "filename", file->filename);

    BSON_APPEND_UTF8(doc, "extension", file->extension);

    BSON_APPEND_INT64(doc, "initial_size", file->initial_size);

    BSON_APPEND_INT64(doc, "actual_size", file->actual_size);

    if (file->changes) {

        BSON_APPEND_DOCUMENT(doc, "changes", file->changes);

    }


    return doc;
}

bool mongo_update_or_insert(const char *filename, uint64_t size, const char *mime) {

    bson_t *query = bson_new();
    BSON_APPEND_UTF8(query, "filename", filename);

    bson_t *update = bson_new();

    bson_t *set_fields = bson_new();

    BSON_APPEND_UTF8(set_fields, "filename", filename);
    BSON_APPEND_INT64(set_fields, "size", (int64_t)size);
    BSON_APPEND_UTF8(set_fields, "mime_type", mime);

    struct timespec ts;
    
    clock_gettime(CLOCK_REALTIME, &ts);

    int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    BSON_APPEND_DATE_TIME(set_fields, "last_modified", now_ms);


    BSON_APPEND_DOCUMENT(update, "$set", set_fields);

    bson_destroy(set_fields);

    bson_t reply;

    bson_error_t error;

    bool success = mongoc_collection_update_one(

        g_collection,
        query,
        update,
        NULL,
        NULL,
        &error

    );

    bson_destroy(query);
    bson_destroy(update);
    bson_destroy(&reply);

    return success;

}

bool mongo_insert(const char *filename, uint64_t size, const char *mime_type) {
    if(!g_collection || !filename) return false;

    bson_t *doc = bson_new();

    BSON_APPEND_UTF8(doc, "filename", filename);
    BSON_APPEND_UTF8(doc, "mime_type", mime_type);

    BSON_APPEND_INT64(doc, "size", (int64_t)size);
    BSON_APPEND_BOOL(doc, "deleted", false);

    BSON_APPEND_DATE_TIME(doc, "created_at", bson_get_monotonic_time() / 1000);

    bson_error_t error;
    bool success = mongoc_collection_insert_one(g_collection, doc, NULL, NULL, &error);

    if(!success) {

        fprintf(stderr, "MongoDb insert failed: %s\n", error.message);

    }

    bson_destroy(doc);
    return success;
}