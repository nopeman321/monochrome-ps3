/*
  Lightweight hardened cJSON parser implementation for PS3 GameOS
*/

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include "cJSON.h"

const char *cJSON_GetErrorPtr(void) {
    return NULL;
}

static int cjson_strcasecmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return (s1 == s2) ? 0 : 1;
    while (*s1 && *s2) {
        int d = tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
        if (d != 0) return d;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

static void *(*cJSON_malloc)(size_t sz) = malloc;
static void (*cJSON_free)(void *ptr) = free;

void cJSON_InitHooks(cJSON_Hooks* hooks) {
    if (!hooks) {
        cJSON_malloc = malloc;
        cJSON_free = free;
        return;
    }
    cJSON_malloc = (hooks->malloc_fn) ? hooks->malloc_fn : malloc;
    cJSON_free = (hooks->free_fn) ? hooks->free_fn : free;
}

static cJSON *cJSON_New_Item(void) {
    cJSON* node = (cJSON*)cJSON_malloc(sizeof(cJSON));
    if (node) memset(node, 0, sizeof(cJSON));
    return node;
}

void cJSON_Delete(cJSON *c) {
    cJSON *next;
    while (c) {
        next = c->next;
        if (!(c->type & cJSON_IsReference) && c->child) cJSON_Delete(c->child);
        if (!(c->type & cJSON_IsReference) && c->valuestring) cJSON_free(c->valuestring);
        if (!(c->type & cJSON_StringIsConst) && c->string) cJSON_free(c->string);
        cJSON_free(c);
        c = next;
    }
}

static const char *skip(const char *in) {
    while (in && *in && (unsigned char)*in <= 32) in++;
    return in;
}

static const char *parse_number(cJSON *item, const char *num) {
    double n = 0, sign = 1, scale = 0;
    int subscale = 0, signsubscale = 1;

    if (*num == '-') sign = -1, num++;
    if (*num == '0') num++;
    if (*num >= '1' && *num <= '9') {
        do { n = (n * 10.0) + (*num++ - '0'); } while (*num >= '0' && *num <= '9');
    }
    if (*num == '.' && num[1] >= '0' && num[1] <= '9') {
        num++;
        do { n = (n * 10.0) + (*num++ - '0'); scale--; } while (*num >= '0' && *num <= '9');
    }
    if (*num == 'e' || *num == 'E') {
        num++;
        if (*num == '+') num++;
        else if (*num == '-') signsubscale = -1, num++;
        while (*num >= '0' && *num <= '9') subscale = (subscale * 10) + (*num++ - '0');
    }

    n = sign * n * pow(10.0, (scale + subscale * signsubscale));
    item->valuedouble = n;
    if (isnan(n)) {
        item->valueint = 0;
    } else if (n >= (double)INT_MAX) {
        item->valueint = INT_MAX;
    } else if (n <= (double)INT_MIN) {
        item->valueint = INT_MIN;
    } else {
        item->valueint = (int)n;
    }
    item->type = cJSON_Number;
    return num;
}

static int parse_hex4(const char *str, unsigned *out) {
    unsigned h = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)str[i];
        h <<= 4;
        if (c >= '0' && c <= '9') {
            h |= (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            h |= (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            h |= (unsigned)(c - 'A' + 10);
        } else {
            return 0;
        }
    }
    *out = h;
    return 1;
}

static char *encode_utf8(char *dst, unsigned uc) {
    if (uc < 0x80) {
        *dst++ = (char)uc;
    } else if (uc < 0x800) {
        *dst++ = (char)(0xC0 | (uc >> 6));
        *dst++ = (char)(0x80 | (uc & 0x3F));
    } else if (uc < 0x10000) {
        *dst++ = (char)(0xE0 | (uc >> 12));
        *dst++ = (char)(0x80 | ((uc >> 6) & 0x3F));
        *dst++ = (char)(0x80 | (uc & 0x3F));
    } else if (uc <= 0x10FFFF) {
        *dst++ = (char)(0xF0 | (uc >> 18));
        *dst++ = (char)(0x80 | ((uc >> 12) & 0x3F));
        *dst++ = (char)(0x80 | ((uc >> 6) & 0x3F));
        *dst++ = (char)(0x80 | (uc & 0x3F));
    } else {
        /* Replacement character U+FFFD for out-of-range codepoint */
        *dst++ = (char)0xEF;
        *dst++ = (char)0xBF;
        *dst++ = (char)0xBD;
    }
    return dst;
}

static const char *parse_string(cJSON *item, const char *str, const char **err_out) {
    const char *ptr = str + 1;
    char *ptr2;
    char *out;
    size_t len = 0;

    if (*str != '\"') {
        if (err_out) *err_out = str;
        return NULL;
    }

    while (*ptr != '\"' && *ptr) {
        if (*ptr == '\\') {
            ptr++;
            if (!*ptr) {
                if (err_out) *err_out = ptr;
                return NULL;
            }
        }
        ptr++;
        len++;
    }
    if (*ptr != '\"') {
        if (err_out) *err_out = ptr;
        return NULL;
    }

    out = (char*)cJSON_malloc(len + 1);
    if (!out) return NULL;

    ptr = str + 1;
    ptr2 = out;
    while (*ptr != '\"' && *ptr) {
        if (*ptr != '\\') {
            *ptr2++ = *ptr++;
        } else {
            ptr++;
            switch (*ptr) {
                case 'b': *ptr2++ = '\b'; ptr++; break;
                case 'f': *ptr2++ = '\f'; ptr++; break;
                case 'n': *ptr2++ = '\n'; ptr++; break;
                case 'r': *ptr2++ = '\r'; ptr++; break;
                case 't': *ptr2++ = '\t'; ptr++; break;
                case '\"': *ptr2++ = '\"'; ptr++; break;
                case '\\': *ptr2++ = '\\'; ptr++; break;
                case '/': *ptr2++ = '/'; ptr++; break;
                case 'u': {
                    unsigned uc = 0;
                    if (!parse_hex4(ptr + 1, &uc)) {
                        cJSON_free(out);
                        if (err_out) *err_out = ptr;
                        return NULL;
                    }
                    ptr += 5;
                    if (uc >= 0xD800 && uc <= 0xDBFF) {
                        unsigned uc2 = 0;
                        if (ptr[0] == '\\' && ptr[1] == 'u' && parse_hex4(ptr + 2, &uc2) &&
                            uc2 >= 0xDC00 && uc2 <= 0xDFFF) {
                            uc = 0x10000U + (((uc & 0x3FFU) << 10) | (uc2 & 0x3FFU));
                            ptr += 6;
                        } else {
                            uc = 0xFFFD;
                        }
                    } else if (uc >= 0xDC00 && uc <= 0xDFFF) {
                        uc = 0xFFFD;
                    }
                    ptr2 = encode_utf8(ptr2, uc);
                    break;
                }
                default:
                    *ptr2++ = *ptr++;
                    break;
            }
        }
    }
    *ptr2 = '\0';
    if (*ptr == '\"') ptr++;
    item->valuestring = out;
    item->type = cJSON_String;
    return ptr;
}

static const char *parse_value(cJSON *item, const char *value, int depth, const char **err_out);

static const char *parse_array(cJSON *item, const char *value, int depth, const char **err_out) {
    cJSON *child;
    if (*value != '[') {
        if (err_out) *err_out = value;
        return NULL;
    }

    item->type = cJSON_Array;
    value = skip(value + 1);
    if (!value) return NULL;
    if (*value == ']') return value + 1;

    item->child = child = cJSON_New_Item();
    if (!item->child) return NULL;
    value = skip(parse_value(child, skip(value), depth + 1, err_out));
    if (!value) return NULL;

    while (*value == ',') {
        cJSON *new_item = cJSON_New_Item();
        if (!new_item) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_value(child, skip(value + 1), depth + 1, err_out));
        if (!value) return NULL;
    }

    if (*value == ']') return value + 1;
    if (err_out) *err_out = value;
    return NULL;
}

static const char *parse_object(cJSON *item, const char *value, int depth, const char **err_out) {
    cJSON *child;
    if (*value != '{') {
        if (err_out) *err_out = value;
        return NULL;
    }

    item->type = cJSON_Object;
    value = skip(value + 1);
    if (!value) return NULL;
    if (*value == '}') return value + 1;

    item->child = child = cJSON_New_Item();
    if (!item->child) return NULL;
    value = skip(parse_string(child, skip(value), err_out));
    if (!value) return NULL;
    child->string = child->valuestring;
    child->valuestring = NULL;
    if (*value != ':') {
        if (err_out) *err_out = value;
        return NULL;
    }
    value = skip(parse_value(child, skip(value + 1), depth + 1, err_out));
    if (!value) return NULL;

    while (*value == ',') {
        cJSON *new_item = cJSON_New_Item();
        if (!new_item) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_string(child, skip(value + 1), err_out));
        if (!value) return NULL;
        child->string = child->valuestring;
        child->valuestring = NULL;
        if (*value != ':') {
            if (err_out) *err_out = value;
            return NULL;
        }
        value = skip(parse_value(child, skip(value + 1), depth + 1, err_out));
        if (!value) return NULL;
    }

    if (*value == '}') return value + 1;
    if (err_out) *err_out = value;
    return NULL;
}

static const char *parse_value(cJSON *item, const char *value, int depth, const char **err_out) {
    if (!value) return NULL;
    if (depth >= CJSON_MAX_DEPTH) {
        if (err_out) *err_out = value;
        return NULL;
    }
    if (!strncmp(value, "null", 4))  { item->type = cJSON_NULL; return value + 4; }
    if (!strncmp(value, "false", 5)) { item->type = cJSON_False; return value + 5; }
    if (!strncmp(value, "true", 4))  { item->type = cJSON_True; item->valueint = 1; return value + 4; }
    if (*value == '\"')              { return parse_string(item, value, err_out); }
    if (*value == '-' || (*value >= '0' && *value <= '9')) { return parse_number(item, value); }
    if (*value == '[')              { return parse_array(item, value, depth, err_out); }
    if (*value == '{')              { return parse_object(item, value, depth, err_out); }

    if (err_out) *err_out = value;
    return NULL;
}

cJSON *cJSON_ParseWithError(const char *value, const char **err_out) {
    const char *local_err = NULL;
    if (err_out) *err_out = NULL;
    if (!value) return NULL;

    cJSON *c = cJSON_New_Item();
    if (!c) return NULL;
    if (!parse_value(c, skip(value), 0, &local_err)) {
        if (err_out) *err_out = local_err;
        cJSON_Delete(c);
        return NULL;
    }
    return c;
}

cJSON *cJSON_Parse(const char *value) {
    return cJSON_ParseWithError(value, NULL);
}

int cJSON_GetArraySize(const cJSON *array) {
    cJSON *c = array ? array->child : NULL;
    int i = 0;
    while (c) i++, c = c->next;
    return i;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index) {
    cJSON *c = (array && index >= 0) ? array->child : NULL;
    while (c && index > 0) index--, c = c->next;
    return c;
}

cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string) {
    cJSON *c = object ? object->child : NULL;
    if (!string) return NULL;
    while (c && (!c->string || cjson_strcasecmp(c->string, string))) c = c->next;
    return c;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string) {
    cJSON *c = object ? object->child : NULL;
    if (!string) return NULL;
    while (c && (!c->string || strcmp(c->string, string))) c = c->next;
    return c;
}

int cJSON_HasObjectItem(const cJSON *object, const char *string) {
    return cJSON_GetObjectItem(object, string) != NULL;
}
