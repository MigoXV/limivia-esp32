#include "v3.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static atomic_uint allocation_failures;
static void *json_alloc(size_t n) {
    void *p = malloc(n);
    if (!p)
        atomic_fetch_add(&allocation_failures, 1);
    return p;
}
void v3_json_init(void) {
    cJSON_Hooks hooks = {.malloc_fn = json_alloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
}
unsigned v3_json_failures(void) { return atomic_load(&allocation_failures); }
bool v3_put(cJSON *object, const char *key, cJSON *item) {
    if (!item)
        return false;
    if (cJSON_AddItemToObject(object, key, item))
        return true;
    cJSON_Delete(item);
    return false;
}
bool v3_push(cJSON *array, cJSON *item) {
    if (!item)
        return false;
    if (cJSON_AddItemToArray(array, item))
        return true;
    cJSON_Delete(item);
    return false;
}
bool v3_replace(cJSON *object, const char *key, cJSON *item) {
    if (!item)
        return false;
    if (cJSON_ReplaceItemInObjectCaseSensitive(object, key, item))
        return true;
    cJSON_Delete(item);
    return false;
}

const cJSON *v3_get(const cJSON *p, const char *k) {
    return cJSON_GetObjectItemCaseSensitive(p, k);
}
bool v3_integer(const cJSON *p, double min, double max) {
    return cJSON_IsNumber(p) && isfinite(p->valuedouble) && p->valuedouble >= min &&
           p->valuedouble <= max && floor(p->valuedouble) == p->valuedouble;
}
int v3_int(const cJSON *p, const char *k, int d) {
    const cJSON *v = v3_get(p, k);
    return cJSON_IsNumber(v) ? v->valueint : d;
}
double v3_num(const cJSON *p, const char *k, double d) {
    const cJSON *v = v3_get(p, k);
    return cJSON_IsNumber(v) ? v->valuedouble : d;
}
bool v3_bool(const cJSON *p, const char *k, bool d) {
    const cJSON *v = v3_get(p, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : d;
}
const char *v3_str(const cJSON *p, const char *k, const char *d) {
    const cJSON *v = v3_get(p, k);
    return cJSON_IsString(v) ? v->valuestring : d;
}
bool v3_keys(const cJSON *p, const char *keys) {
    if (!cJSON_IsObject(p))
        return false;
    for (const cJSON *v = p->child; v; v = v->next) {
        bool found = false;
        for (const char *k = keys; *k;) {
            const char *end = strchr(k, ' ');
            size_t n = end ? (size_t)(end - k) : strlen(k);
            if (strlen(v->string) == n && !memcmp(v->string, k, n))
                found = true;
            k += n;
            if (*k)
                ++k;
        }
        if (!found)
            return false;
    }
    return true;
}
static bool utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], count, cp, min;
        if (!c)
            return false;
        if (c < 128)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            cp = c & 31;
            min = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            cp = c & 15;
            min = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            cp = c & 7;
            min = 0x10000;
        } else
            return false;
        if (i + count > n)
            return false;
        while (count--) {
            unsigned b = s[i++];
            if ((b & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (b & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
static bool lexical(const char *s, size_t n) {
    if (!utf8((const unsigned char *)s, n))
        return false;
    unsigned depth = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '"') {
            bool closed = false;
            while (++i < n) {
                if ((unsigned char)s[i] < 32)
                    return false;
                if (s[i] == '"') {
                    closed = true;
                    break;
                }
                if (s[i] == '\\') {
                    if (++i >= n)
                        return false;
                    if (s[i] == 'u') {
                        if (i + 4 >= n || !memcmp(s + i + 1, "0000", 4))
                            return false;
                        i += 4;
                    }
                }
            }
            if (!closed)
                return false;
        } else if (s[i] == '{' || s[i] == '[') {
            if (++depth > 16)
                return false;
        } else if (s[i] == '}' || s[i] == ']') {
            if (!depth)
                return false;
            --depth;
        } else if (s[i] == '-' || (s[i] >= '0' && s[i] <= '9')) {
            size_t j = i;
            if (s[j] == '-')
                ++j;
            if (j >= n)
                return false;
            if (s[j] == '0')
                ++j;
            else {
                if (s[j] < '1' || s[j] > '9')
                    return false;
                do {
                    ++j;
                } while (j < n && s[j] >= '0' && s[j] <= '9');
            }
            if (j < n && s[j] == '.') {
                size_t a = ++j;
                while (j < n && s[j] >= '0' && s[j] <= '9')
                    ++j;
                if (a == j)
                    return false;
            }
            if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                ++j;
                if (j < n && (s[j] == '+' || s[j] == '-'))
                    ++j;
                size_t a = j;
                while (j < n && s[j] >= '0' && s[j] <= '9')
                    ++j;
                if (a == j)
                    return false;
            }
            if (j < n && !strchr(" \t\r\n,]}", s[j]))
                return false;
            i = j - 1;
        }
    }
    return depth == 0;
}
static bool unique(const cJSON *p) {
    if (cJSON_IsNumber(p) && !isfinite(p->valuedouble))
        return false;
    for (const cJSON *a = p->child; a; a = a->next) {
        if (cJSON_IsObject(p))
            for (const cJSON *b = a->next; b; b = b->next)
                if (!strcmp(a->string, b->string))
                    return false;
        if (!unique(a))
            return false;
    }
    return true;
}
cJSON *v3_parse(const char *s, size_t n) {
    if (!n || !lexical(s, n))
        return NULL;
    // Length-limited input does not require access to s[n].
    const char *end = NULL;
    cJSON *p = cJSON_ParseWithLengthOpts(s, n, &end, false);
    if (!p)
        return NULL;
    while (end < s + n && strchr(" \r\n\t", *end))
        ++end;
    if (end != s + n || !unique(p)) {
        cJSON_Delete(p);
        return NULL;
    }
    return p;
}
static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int digit(unsigned char c) {
    const char *p = c ? strchr(alphabet, c) : NULL;
    return p ? (int)(p - alphabet) : -1;
}
bool v3_decode64(const char *s, uint8_t *out, size_t cap, size_t *used) {
    size_t n = strlen(s), k = 0;
    *used = 0;
    if (n % 4)
        return false;
    for (size_t i = 0; i < n; i += 4) {
        int a = digit(s[i]), b = digit(s[i + 1]), c = digit(s[i + 2]), d = digit(s[i + 3]);
        bool p = s[i + 2] == '=', q = s[i + 3] == '=';
        if (a < 0 || b < 0 || (!p && c < 0) || (!q && d < 0) || (p && !q) ||
            ((p || q) && i + 4 != n))
            return false;
        if ((p && (b & 15)) || (!p && q && (c & 3)))
            return false; // canonical pad bits
        size_t count = p ? 1 : q ? 2 : 3;
        if (k + count > cap)
            return false;
        out[k++] = (uint8_t)((a << 2) | (b >> 4));
        if (!p)
            out[k++] = (uint8_t)((b << 4) | (c >> 2));
        if (!q)
            out[k++] = (uint8_t)((c << 6) | d);
    }
    *used = k;
    return true;
}
char *v3_encode64(const uint8_t *s, size_t n) {
    if (n > V3_BUFFER_MAX)
        return NULL;
    char *p = malloc(4 * ((n + 2) / 3) + 1);
    if (!p)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned a = s[i], b = i + 1 < n ? s[i + 1] : 0, c = i + 2 < n ? s[i + 2] : 0;
        p[k++] = alphabet[a >> 2];
        p[k++] = alphabet[((a & 3) << 4) | (b >> 4)];
        p[k++] = i + 1 < n ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        p[k++] = i + 2 < n ? alphabet[c & 63] : '=';
    }
    p[k] = 0;
    return p;
}
size_t v3_frame(const cJSON *p, char *out, size_t cap) {
    if (cap < 3 || !cJSON_PrintPreallocated((cJSON *)p, out, (int)cap - 2, false))
        return 0;
    size_t n = strlen(out);
    if (n > LV_RESPONSE_MAX)
        return 0;
    out[n++] = '\n';
    out[n] = 0;
    return n;
}
void v3_add_u32(cJSON *p, const char *k, uint32_t v) { cJSON_AddNumberToObject(p, k, v); }
