#include "test_nfc_file_parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* trim_dup(const char* start, const char* end) {
    while(start < end && isspace((unsigned char)*start)) start++;
    while(end > start && isspace((unsigned char)*(end - 1))) end--;
    size_t len = (size_t)(end - start);
    char* out = malloc(len + 1);
    if(!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static bool grow(NfcFile* file) {
    if(file->count < file->capacity) return true;
    size_t new_cap = file->capacity ? file->capacity * 2 : 8;
    NfcFileEntry* new_entries = realloc(file->entries, new_cap * sizeof(*new_entries));
    if(!new_entries) return false;
    file->entries = new_entries;
    file->capacity = new_cap;
    return true;
}

bool nfc_file_parse(const char* text, NfcFile* out) {
    out->entries = NULL;
    out->count = 0;
    out->capacity = 0;

    const char* line = text;
    while(*line) {
        const char* end = line;
        while(*end && *end != '\n') end++;

        // Skip blank lines and comments.
        const char* p = line;
        while(p < end && isspace((unsigned char)*p)) p++;
        if(p >= end || *p == '#') {
            line = (*end == '\n') ? end + 1 : end;
            continue;
        }

        const char* colon = p;
        while(colon < end && *colon != ':') colon++;
        if(colon == end) {
            // Malformed line: no colon.
            nfc_file_free(out);
            return false;
        }

        char* key = trim_dup(p, colon);
        char* value = trim_dup(colon + 1, end);
        if(!key || !value) {
            free(key);
            free(value);
            nfc_file_free(out);
            return false;
        }

        if(!grow(out)) {
            free(key);
            free(value);
            nfc_file_free(out);
            return false;
        }

        out->entries[out->count].key = key;
        out->entries[out->count].value = value;
        out->count++;

        line = (*end == '\n') ? end + 1 : end;
    }

    return true;
}

void nfc_file_free(NfcFile* file) {
    if(!file) return;
    for(size_t i = 0; i < file->count; i++) {
        free(file->entries[i].key);
        free(file->entries[i].value);
    }
    free(file->entries);
    file->entries = NULL;
    file->count = 0;
    file->capacity = 0;
}

const char* nfc_file_get(const NfcFile* file, const char* key) {
    for(size_t i = 0; i < file->count; i++) {
        if(strcmp(file->entries[i].key, key) == 0) {
            return file->entries[i].value;
        }
    }
    return NULL;
}

static char* str_dup(const char* s) {
    size_t len = strlen(s) + 1;
    char* out = malloc(len);
    if(out) memcpy(out, s, len);
    return out;
}

bool nfc_file_set(NfcFile* file, const char* key, const char* value) {
    for(size_t i = 0; i < file->count; i++) {
        if(strcmp(file->entries[i].key, key) == 0) {
            char* v = str_dup(value);
            if(!v) return false;
            free(file->entries[i].value);
            file->entries[i].value = v;
            return true;
        }
    }
    if(!grow(file)) return false;
    char* k = str_dup(key);
    char* v = str_dup(value);
    if(!k || !v) {
        free(k);
        free(v);
        return false;
    }
    file->entries[file->count].key = k;
    file->entries[file->count].value = v;
    file->count++;
    return true;
}

bool nfc_file_serialize(const NfcFile* file, char* out, size_t out_cap, size_t* out_len) {
    size_t pos = 0;
    for(size_t i = 0; i < file->count; i++) {
        size_t key_len = strlen(file->entries[i].key);
        size_t val_len = strlen(file->entries[i].value);
        // Need room for "key: value\n" plus NUL.
        if(pos + key_len + 2 + val_len + 1 + 1 > out_cap) return false;
        memcpy(out + pos, file->entries[i].key, key_len);
        pos += key_len;
        out[pos++] = ':';
        out[pos++] = ' ';
        memcpy(out + pos, file->entries[i].value, val_len);
        pos += val_len;
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    if(out_len) *out_len = pos;
    return true;
}

bool nfc_file_parse_hex(
    const NfcFile* file,
    const char* key,
    uint8_t* out,
    size_t* out_len,
    size_t out_max) {
    const char* value = nfc_file_get(file, key);
    if(!value) return false;

    size_t len = strlen(value);
    size_t bytes = 0;
    for(size_t i = 0; i < len; i++) {
        if(isspace((unsigned char)value[i])) continue;
        if(i + 1 >= len || !isxdigit((unsigned char)value[i]) ||
           !isxdigit((unsigned char)value[i + 1])) {
            return false;
        }
        if(bytes >= out_max) return false;
        unsigned int byte = 0;
        sscanf(value + i, "%2x", &byte);
        out[bytes++] = (uint8_t)byte;
        i++; // skip second hex digit
    }

    if(out_len) *out_len = bytes;
    return true;
}
