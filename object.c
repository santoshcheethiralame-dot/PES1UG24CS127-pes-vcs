// object.c — Content-addressable object store
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
    const char *type_str = (type == OBJ_BLOB) ? "blob" :
                           (type == OBJ_TREE) ? "tree" : "commit";
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    size_t total = hlen + len;
    uint8_t *full = malloc(total);
    if (!full) return -1;
    memcpy(full, header, hlen);
    memcpy(full + hlen, data, len);

    ObjectID id;
    compute_hash(full, total, &id);
    if (id_out) *id_out = id;

    if (object_exists(&id)) { free(full); return 0; }

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(dir, 0755);

    char tmp_path[512], final_path[512];
    snprintf(tmp_path,   sizeof(tmp_path),   "%s/%.2s/.tmp_%s", OBJECTS_DIR, hex, hex + 2);
    snprintf(final_path, sizeof(final_path), "%s/%.2s/%s",      OBJECTS_DIR, hex, hex + 2);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(full); return -1; }
    write(fd, full, total);
    fsync(fd);
    close(fd);
    free(full);

    if (rename(tmp_path, final_path) != 0) return -1;

    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }

    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    size_t fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(fsize);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, fsize, f);
    fclose(f);

    ObjectID computed;
    compute_hash(buf, fsize, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) { free(buf); return -1; }

    uint8_t *null_pos = memchr(buf, '\0', fsize);
    if (!null_pos) { free(buf); return -1; }

    if      (strncmp((char*)buf, "blob",   4) == 0) *type_out = OBJ_BLOB;
    else if (strncmp((char*)buf, "tree",   4) == 0) *type_out = OBJ_TREE;
    else if (strncmp((char*)buf, "commit", 6) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    size_t data_offset = (null_pos - buf) + 1;
    size_t data_len = fsize - data_offset;
    void *out = malloc(data_len + 1);
    if (!out) { free(buf); return -1; }
    memcpy(out, buf + data_offset, data_len);
    free(buf);

    *data_out = out;
    *len_out = data_len;
    return 0;
}
