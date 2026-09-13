/*
 * Centralized filesystem access (Phase 14, PART AH).
 *
 * Every file read in the asset pipeline flows through la_fs_read;
 * path joins/splits through la_fs_join/la_fs_dirname. A future
 * engine package format replaces these three functions in one place.
 * Paths are UTF-8 (fopen as given); separators '/' and '\\' are both
 * accepted on input and normalized to '/' for cache keys by callers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luma_assets/luma_assets.h"
#include "internal/assets_internal.h"

la_result la_fs_read(const char *path, unsigned char **out_bytes,
                     size_t *out_size) {
    FILE *file = NULL;
    long length = 0;
    unsigned char *bytes = NULL;
    size_t got = 0;

    if (path == NULL || out_bytes == NULL || out_size == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    *out_bytes = NULL;
    *out_size = 0;
    if (path[0] == '\0') {
        return LA_ERROR_NOT_FOUND;
    }
#ifdef _MSC_VER
    {
        errno_t err = fopen_s(&file, path, "rb");

        if (err != 0 || file == NULL) {
            return LA_ERROR_NOT_FOUND;
        }
    }
#else
    file = fopen(path, "rb");
    if (file == NULL) {
        return LA_ERROR_NOT_FOUND;
    }
#endif
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return LA_ERROR_IMPORT;
    }
    length = ftell(file);
    if (length < 0) {
        fclose(file);
        return LA_ERROR_IMPORT;
    }
    rewind(file);
    /* Empty files are malformed inputs, not empty assets. */
    if (length == 0) {
        fclose(file);
        return LA_ERROR_IMPORT;
    }
    bytes = (unsigned char *)malloc((size_t)length > 0 ? (size_t)length : 1);
    if (bytes == NULL) {
        fclose(file);
        return LA_ERROR_OUT_OF_MEMORY;
    }
    got = fread(bytes, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        free(bytes);
        return LA_ERROR_IMPORT;
    }
    *out_bytes = bytes;
    *out_size = (size_t)length;
    return LA_SUCCESS;
}

void la_fs_free(unsigned char *bytes) {
    free(bytes);
}

la_result la_fs_join(const char *dir, const char *leaf, char **out_path) {
    size_t dir_len;
    size_t leaf_len;
    char *path;
    size_t i;

    if (dir == NULL || leaf == NULL || out_path == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    *out_path = NULL;
    /* Absolute leaves (either separator style, or Windows drive) and
     * empty dirs pass through untouched. */
    if (dir[0] == '\0' || leaf[0] == '/' || leaf[0] == '\\' ||
        (leaf[0] != '\0' && leaf[1] == ':')) {
        path = (char *)malloc(strlen(leaf) + 1);
        if (path == NULL) {
            return LA_ERROR_OUT_OF_MEMORY;
        }
        memcpy(path, leaf, strlen(leaf) + 1);
        *out_path = path;
        return LA_SUCCESS;
    }
    dir_len = strlen(dir);
    leaf_len = strlen(leaf);
    path = (char *)malloc(dir_len + 1 + leaf_len + 1);
    if (path == NULL) {
        return LA_ERROR_OUT_OF_MEMORY;
    }
    memcpy(path, dir, dir_len);
    path[dir_len] = '/';
    memcpy(path + dir_len + 1, leaf, leaf_len + 1);
    /* Normalize backslashes for stable cache keys. */
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '\\') {
            path[i] = '/';
        }
    }
    *out_path = path;
    return LA_SUCCESS;
}

la_result la_fs_dirname(const char *path, char **out_dir) {
    size_t len;
    size_t cut;
    char *dir;

    if (path == NULL || out_dir == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    *out_dir = NULL;
    len = strlen(path);
    cut = len;
    while (cut > 0 && path[cut - 1] != '/' && path[cut - 1] != '\\') {
        cut--;
    }
    /* No separator: relative to the process directory (""). A bare
     * separator root keeps one char so joins stay absolute. */
    if (cut == 0) {
        dir = (char *)malloc(1);
        if (dir == NULL) {
            return LA_ERROR_OUT_OF_MEMORY;
        }
        dir[0] = '\0';
        *out_dir = dir;
        return LA_SUCCESS;
    }
    if (cut == 1 && (path[0] == '/' || path[0] == '\\')) {
        dir = (char *)malloc(2);
        if (dir == NULL) {
            return LA_ERROR_OUT_OF_MEMORY;
        }
        dir[0] = '/';
        dir[1] = '\0';
        *out_dir = dir;
        return LA_SUCCESS;
    }
    /* Strip trailing separators (but keep "C:" style roots whole). */
    while (cut > 1 && (path[cut - 1] == '/' || path[cut - 1] == '\\') &&
           path[cut - 2] != ':') {
        cut--;
    }
    dir = (char *)malloc(cut + 1);
    if (dir == NULL) {
        return LA_ERROR_OUT_OF_MEMORY;
    }
    memcpy(dir, path, cut);
    dir[cut] = '\0';
    *out_dir = dir;
    return LA_SUCCESS;
}
