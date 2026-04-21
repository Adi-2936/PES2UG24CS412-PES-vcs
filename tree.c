// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions: tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
// "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
// "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declarations
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────
#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 for the null terminator
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Recursive helper: builds a tree object for one directory level.
// - entries:  pointer into the sorted index entries array
// - count:    number of entries at this level and below
// - prefix:   the path prefix for this level, e.g. "" for root, "src/" for src/
// - id_out:   receives the hash of the written tree object
static int write_tree_level(IndexEntry *entries, int count,
                             const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        // Strip the prefix to get the relative path within this level
        const char *rel_path = entries[i].path + strlen(prefix);
        char *slash = strchr(rel_path, '/');

        if (!slash) {
            // --- Leaf file: add directly as a blob entry ---
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = entries[i].mode;
            strncpy(e->name, rel_path, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            e->hash = entries[i].hash;
            i++;
        } else {
            // --- Sub-directory: group all entries sharing this dir prefix ---
            char dir_name[256];
            size_t dir_len = (size_t)(slash - rel_path);
            if (dir_len >= sizeof(dir_name)) return -1;
            strncpy(dir_name, rel_path, dir_len);
            dir_name[dir_len] = '\0';

            // Build the full prefix for this sub-directory
            char sub_prefix[512];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);

            // Count how many entries belong to this sub-directory
            int j = i;
            while (j < count &&
                   strncmp(entries[j].path, sub_prefix, strlen(sub_prefix)) == 0) {
                j++;
            }

            // Recurse to build the sub-tree
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, sub_prefix, &sub_id) != 0)
                return -1;

            // Add the sub-tree as a directory entry
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = 0040000;
            strncpy(e->name, dir_name, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            e->hash = sub_id;

            i = j; // Skip past all entries we just processed
        }
    }

    // Serialize the tree and write it to the object store
    void *data;
    size_t data_len;
    if (tree_serialize(&tree, &data, &data_len) != 0) return -1;
    int rc = object_write(OBJ_TREE, data, data_len, id_out);
    free(data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;
    if (index.count == 0) {
        fprintf(stderr, "error: nothing to commit (index is empty)\n");
        return -1;
    }
    return write_tree_level(index.entries, index.count, "", id_out);
}