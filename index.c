// index.c — Staging area implementation
#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++)
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("    staged: %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("    deleted: %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("    modified: %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes")  == 0) continue;
            if (strstr(ent->d_name, ".o")   != NULL) continue;
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("    untracked: %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("    (nothing to show)\n");
    printf("\n");
    return 0;
}

/* index_load: opens .pes/index, parses each line with fscanf.
   format: <mode-octal> <hex-hash> <mtime> <size> <path>
   returns 0 even if file missing (empty index is valid) */
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char hex[HASH_HEX_SIZE + 2];
    unsigned int mode;
    unsigned long long mtime;
    unsigned int size;
    char path[512];

    while (index->count < MAX_INDEX_ENTRIES &&
           fscanf(f, "%o %64s %llu %u %511s",
                  &mode, hex, &mtime, &size, path) == 5) {
        IndexEntry *e = &index->entries[index->count];
        e->mode      = (uint32_t)mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint32_t)size;
        if (hex_to_hash(hex, &e->hash) != 0) { fclose(f); return -1; }
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        index->count++;
    }
    fclose(f);
    return 0;
}

static int cmp_entry_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    // IMPORTANT: do NOT do "Index sorted = *index" here —
    // Index is ~5.4 MB and cmd_add already has one on the stack.
    // Two copies would overflow the 8 MB default Linux stack.
    // Allocate the sorted copy on the heap instead.
    IndexEntry *sorted = NULL;
    if (index->count > 0) {
        sorted = malloc(index->count * sizeof(IndexEntry));
        if (!sorted) return -1;
        memcpy(sorted, index->entries, index->count * sizeof(IndexEntry));
        qsort(sorted, (size_t)index->count, sizeof(IndexEntry), cmp_entry_path);
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < index->count; i++) {
        hash_to_hex(&sorted[i].hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                sorted[i].mode,
                hex,
                (unsigned long long)sorted[i].mtime_sec,
                sorted[i].size,
                sorted[i].path);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);

    if (rename(tmp_path, INDEX_FILE) != 0) return -1;
    return 0;
}

int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long fsize_signed = ftell(f);
    rewind(f);
    if (fsize_signed < 0) { fclose(f); return -1; }
    size_t usize = (size_t)fsize_signed;

    uint8_t *buf = malloc(usize > 0 ? usize : 1);
    if (!buf) { fclose(f); return -1; }
    if (usize > 0 && fread(buf, 1, usize, f) != usize) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, buf, usize, &id) != 0) {
        free(buf); return -1;
    }
    free(buf);

    struct stat st;
    if (stat(path, &st) != 0) return -1;
    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->hash      = id;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint32_t)st.st_size;
        existing->mode      = mode;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        IndexEntry *e = &index->entries[index->count++];
        e->mode      = mode;
        e->hash      = id;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint32_t)st.st_size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }
    return index_save(index);
}
