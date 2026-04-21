// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions: object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(id_out->hash, &ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // Step 1: Build header string e.g. "blob 16"
    const char *type_str = (type == OBJ_BLOB)   ? "blob"   :
                           (type == OBJ_TREE)   ? "tree"   : "commit";
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);

    // Full object = header + '\0' + data
    size_t obj_len = (size_t)header_len + 1 + len;
    uint8_t *obj = malloc(obj_len);
    if (!obj) return -1;
    memcpy(obj, header, header_len);
    obj[header_len] = '\0';
    memcpy(obj + header_len + 1, data, len);

    // Step 2: Compute SHA-256 of the full object
    compute_hash(obj, obj_len, id_out);

    // Step 3: Deduplication — if already stored, nothing to do
    if (object_exists(id_out)) {
        free(obj);
        return 0;
    }

    // Step 4: Create shard directory (.pes/objects/XX/)
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    char shard_dir[256];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755); // OK if already exists

    // Step 5: Write to a temporary file in the shard directory
    char path[512], tmp_path[520];
    object_path(id_out, path, sizeof(path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(obj); return -1; }

    ssize_t written = write(fd, obj, obj_len);
    free(obj);
    if (written < 0 || (size_t)written != obj_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // Step 6: fsync to ensure data reaches disk
    fsync(fd);
    close(fd);

    // Step 7: Atomic rename
    if (rename(tmp_path, path) != 0) return -1;

    // Step 8: fsync the shard directory to persist the rename
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // Step 1: Build file path
    char path[512];
    object_path(id, path, sizeof(path));

    // Step 2: Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size_l = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size_l < 0) { fclose(f); return -1; }
    size_t file_size = (size_t)file_size_l;

    uint8_t *buf = malloc(file_size);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, file_size, f) != file_size) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    // Step 3: Verify integrity — recompute hash and compare to filename
    ObjectID computed;
    compute_hash(buf, file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1; // Corrupted object
    }

    // Step 4: Parse header — find '\0' separator between header and data
    uint8_t *null_ptr = memchr(buf, '\0', file_size);
    if (!null_ptr) { free(buf); return -1; }

    // Step 5: Determine object type from header prefix
    if      (strncmp((char*)buf, "blob",   4) == 0) *type_out = OBJ_BLOB;
    else if (strncmp((char*)buf, "tree",   4) == 0) *type_out = OBJ_TREE;
    else if (strncmp((char*)buf, "commit", 6) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // Step 6: Extract the data portion (everything after the '\0')
    size_t header_len = (size_t)(null_ptr - buf);
    size_t data_len   = file_size - header_len - 1;

    *data_out = malloc(data_len + 1); // +1 for safe null termination
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, null_ptr + 1, data_len);
    ((uint8_t*)*data_out)[data_len] = '\0';
    *len_out = data_len;

    free(buf);
    return 0;
}
