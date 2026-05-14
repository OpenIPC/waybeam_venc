/*
 * json_cli.c — Style-preserving JSON config get/set/del tool
 *
 * Uses jsmn for tokenization and performs text-level surgery to preserve
 * the original file formatting (whitespace, indentation, comments).
 *
 * Usage:
 *   json_cli -g .telemetry.uart_device -i config.json
 *   json_cli -s .telemetry.uart_baud 115200 -i config.json
 *   json_cli -s .metrics.values[1] '"@rssi"' -i config.json
 *   json_cli -d .debug -i config.json
 *   json_cli -g .name -i config.json --raw
 *   json_cli -g .name -i config.json --json
 *
 * Path syntax:
 *   .key           — object member
 *   .key.subkey    — nested object member
 *   .key[N]        — array element by index
 *   .key[N].sub    — array element then object member
 *
 * Value types (auto-detected for set):
 *   123, -5, 3.14  — number
 *   true, false    — boolean
 *   null           — null
 *   "text"         — string (quotes required for explicit strings)
 *   {}/[]          — JSON object/array literals
 *   anything else  — auto-quoted as string
 *
 * Build:
 *   make json_cli
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>

#include "jsmn.h"

typedef enum {
    MODE_NONE = 0,
    MODE_GET,
    MODE_SET,
    MODE_DEL
} cli_mode_t;

typedef enum {
    GET_AUTO = 0,
    GET_RAW,
    GET_JSON
} get_mode_t;

typedef enum {
    SEG_KEY,
    SEG_INDEX
} path_seg_type_t;

typedef struct {
    path_seg_type_t type;
    char *key;
    int index;
} path_seg_t;

typedef struct {
    path_seg_t *v;
    int n;
    int cap;
} path_vec_t;

typedef struct {
    char *data;
    size_t len;
} buffer_t;

typedef struct {
    size_t start;
    size_t end;
} range_t;

static void usage(const char *prog)
{
    fprintf(stderr,
        "json-cli: style-preserving JSON get/set/del\n\n"
        "Options:\n"
        "  -s, --set <path> <value>  set value at path\n"
        "  -g, --get <path>          get value at path\n"
        "  -d, --del <path>          delete key at path\n"
        "  -h, --help                display this help\n"
        "  -i, --input <file>        input file (default: config.json)\n"
        "  -o, --output <file>       output file (default: same as input)\n"
        "      --raw                 raw/unquoted get output\n"
        "      --json               exact JSON token get output\n\n"
        "Path examples:\n"
        "  .name  .telemetry.uart_baud  .metrics.values[0]\n\n"
        "Value examples:\n"
        "  42  3.14  true  false  null  '\"hello\"'\n\n"
        "Usage:\n"
        "  %s -g .name -i config.json\n"
        "  %s -s .sync_port 9060 -i config.json\n"
        "  %s -d .debug.event_ring_depth -i config.json\n",
        prog, prog, prog);
}

static void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        perror("malloc");
        exit(1);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *r = realloc(p, n);
    if (!r) {
        perror("realloc");
        exit(1);
    }
    return r;
}

static char *xstrdup2(const char *s)
{
    size_t n = strlen(s);
    char *r = xmalloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

static void path_push(path_vec_t *pv, path_seg_t seg)
{
    if (pv->n == pv->cap) {
        pv->cap = pv->cap ? pv->cap * 2 : 8;
        pv->v = xrealloc(pv->v, pv->cap * sizeof(*pv->v));
    }
    pv->v[pv->n++] = seg;
}

static void path_free(path_vec_t *pv)
{
    for (int i = 0; i < pv->n; i++) {
        if (pv->v[i].type == SEG_KEY)
            free(pv->v[i].key);
    }
    free(pv->v);
    pv->v = NULL;
    pv->n = pv->cap = 0;
}

static int parse_path(const char *s, path_vec_t *out)
{
    size_t i = 0;

    if (!s || s[0] != '.') {
        fprintf(stderr, "Path must start with '.'\n");
        return -1;
    }

    while (s[i]) {
        if (s[i] == '.') {
            i++;
            if (!s[i])
                break;

            size_t start = i;
            while (s[i] && s[i] != '.' && s[i] != '[')
                i++;

            if (i == start) {
                fprintf(stderr, "Empty key in path\n");
                return -1;
            }

            char *k = xmalloc(i - start + 1);
            memcpy(k, s + start, i - start);
            k[i - start] = '\0';

            path_seg_t seg = { .type = SEG_KEY, .key = k, .index = -1 };
            path_push(out, seg);
        } else if (s[i] == '[') {
            i++;
            if (!isdigit((unsigned char)s[i])) {
                fprintf(stderr, "Invalid array index in path\n");
                return -1;
            }
            int idx = 0;
            while (isdigit((unsigned char)s[i])) {
                idx = idx * 10 + (s[i] - '0');
                i++;
            }
            if (s[i] != ']') {
                fprintf(stderr, "Missing closing ']'\n");
                return -1;
            }
            i++;

            path_seg_t seg = { .type = SEG_INDEX, .key = NULL, .index = idx };
            path_push(out, seg);
        } else {
            fprintf(stderr, "Unexpected path character '%c'\n", s[i]);
            return -1;
        }
    }

    if (out->n == 0) {
        fprintf(stderr, "Empty path\n");
        return -1;
    }

    return 0;
}

static buffer_t read_file(const char *path)
{
    buffer_t b = {0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open input file '%s': %s\n", path, strerror(errno));
        return b;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return b;
    }

    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return b;
    }

    rewind(f);
    b.data = xmalloc((size_t)sz + 1);
    b.len = (size_t)sz;

    if (b.len && fread(b.data, 1, b.len, f) != b.len) {
        fclose(f);
        free(b.data);
        b.data = NULL;
        b.len = 0;
        return b;
    }

    b.data[b.len] = '\0';
    fclose(f);
    return b;
}

static int write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open output file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "Failed to write output file '%s'\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int parse_json_tokens(const char *js, size_t len, jsmntok_t **out_tok, int *out_count)
{
    int tokcap = 256;

    for (;;) {
        jsmn_parser p;
        jsmntok_t *toks = xmalloc(sizeof(*toks) * tokcap);

        jsmn_init(&p);
        int r = jsmn_parse(&p, js, len, toks, tokcap);
        if (r == JSMN_ERROR_NOMEM) {
            free(toks);
            tokcap *= 2;
            continue;
        }
        if (r < 0) {
            free(toks);
            fprintf(stderr, "JSON parse failed: %d\n", r);
            return -1;
        }

        *out_tok = toks;
        *out_count = r;
        return 0;
    }
}

static int token_eq_str(const char *js, const jsmntok_t *tok, const char *s)
{
    int len = tok->end - tok->start;
    return ((int)strlen(s) == len && memcmp(js + tok->start, s, len) == 0);
}

static int token_total_span(const jsmntok_t *toks, int ntok, int idx)
{
    if (idx < 0 || idx >= ntok)
        return 0;

    int total = 1;
    const jsmntok_t *t = &toks[idx];

    if (t->type == JSMN_OBJECT) {
        int n = t->size * 2;
        int child = idx + 1;
        for (int i = 0; i < n; i++) {
            int sub = token_total_span(toks, ntok, child);
            total += sub;
            child += sub;
        }
    } else if (t->type == JSMN_ARRAY) {
        int child = idx + 1;
        for (int i = 0; i < t->size; i++) {
            int sub = token_total_span(toks, ntok, child);
            total += sub;
            child += sub;
        }
    }

    return total;
}

static int object_find_value(const char *js, const jsmntok_t *toks, int ntok, int obj_idx, const char *key)
{
    const jsmntok_t *obj = &toks[obj_idx];
    if (obj->type != JSMN_OBJECT)
        return -1;

    int child = obj_idx + 1;
    for (int i = 0; i < obj->size; i++) {
        int key_idx = child;
        int key_span = token_total_span(toks, ntok, key_idx);
        int val_idx = key_idx + key_span;
        int val_span = token_total_span(toks, ntok, val_idx);

        if (toks[key_idx].type == JSMN_STRING && token_eq_str(js, &toks[key_idx], key))
            return val_idx;

        child = val_idx + val_span;
    }

    return -1;
}

static int object_find_member(const char *js, const jsmntok_t *toks, int ntok, int obj_idx,
                              const char *key, int *out_key_idx, int *out_val_idx, int *out_member_pos)
{
    const jsmntok_t *obj = &toks[obj_idx];
    if (obj->type != JSMN_OBJECT)
        return -1;

    int child = obj_idx + 1;
    for (int i = 0; i < obj->size; i++) {
        int key_idx = child;
        int key_span = token_total_span(toks, ntok, key_idx);
        int val_idx = key_idx + key_span;
        int val_span = token_total_span(toks, ntok, val_idx);

        if (toks[key_idx].type == JSMN_STRING && token_eq_str(js, &toks[key_idx], key)) {
            if (out_key_idx) *out_key_idx = key_idx;
            if (out_val_idx) *out_val_idx = val_idx;
            if (out_member_pos) *out_member_pos = i;
            return 0;
        }

        child = val_idx + val_span;
    }

    return -1;
}

static int array_find_value(const jsmntok_t *toks, int ntok, int arr_idx, int wanted_index)
{
    const jsmntok_t *arr = &toks[arr_idx];
    if (arr->type != JSMN_ARRAY)
        return -1;
    if (wanted_index < 0 || wanted_index >= arr->size)
        return -1;

    int child = arr_idx + 1;
    for (int i = 0; i < arr->size; i++) {
        if (i == wanted_index)
            return child;
        child += token_total_span(toks, ntok, child);
    }

    return -1;
}

static int find_path_value(const char *js, const jsmntok_t *toks, int ntok, const path_vec_t *path)
{
    if (ntok <= 0)
        return -1;

    int cur = 0;

    for (int i = 0; i < path->n; i++) {
        path_seg_t *seg = &path->v[i];
        if (seg->type == SEG_KEY)
            cur = object_find_value(js, toks, ntok, cur, seg->key);
        else
            cur = array_find_value(toks, ntok, cur, seg->index);

        if (cur < 0)
            return -1;
    }

    return cur;
}

static int find_existing_prefix(const char *js, const jsmntok_t *toks, int ntok, const path_vec_t *path)
{
    int cur = 0;
    int last_ok = -1;

    for (int i = 0; i < path->n; i++) {
        int next = -1;
        if (path->v[i].type == SEG_KEY)
            next = object_find_value(js, toks, ntok, cur, path->v[i].key);
        else
            next = array_find_value(toks, ntok, cur, path->v[i].index);

        if (next < 0)
            return last_ok;

        cur = next;
        last_ok = i;
    }

    return last_ok;
}

static int path_is_object_only_suffix(const path_vec_t *path, int from_idx)
{
    for (int i = from_idx; i < path->n; i++) {
        if (path->v[i].type != SEG_KEY)
            return 0;
    }
    return 1;
}

static int skip_ws_forward(const char *js, size_t len, int pos)
{
    while ((size_t)pos < len && is_ws(js[pos]))
        pos++;
    return pos;
}

static int skip_ws_backward(const char *js, int min_pos, int pos)
{
    while (pos > min_pos && is_ws(js[pos - 1]))
        pos--;
    return pos;
}

static int has_newline_between(const char *js, int a, int b)
{
    for (int i = a; i < b; i++) {
        if (js[i] == '\n')
            return 1;
    }
    return 0;
}

static char *json_escape_string(const char *s)
{
    size_t cap = strlen(s) * 2 + 3;
    char *out = xmalloc(cap);
    size_t j = 0;
    out[j++] = '"';

    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (j + 8 >= cap) {
            cap *= 2;
            out = xrealloc(out, cap);
        }

        switch (c) {
        case '\"': out[j++] = '\\'; out[j++] = '\"'; break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\b': out[j++] = '\\'; out[j++] = 'b';  break;
        case '\f': out[j++] = '\\'; out[j++] = 'f';  break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:
            if (c < 0x20) {
                snprintf(out + j, cap - j, "\\u%04x", c);
                j += 6;
            } else {
                out[j++] = (char)c;
            }
            break;
        }
    }

    out[j++] = '"';
    out[j] = '\0';
    return out;
}

static int looks_like_json_value(const char *s)
{
    if (!s || !*s)
        return 0;

    if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0 || strcmp(s, "null") == 0)
        return 1;

    if (s[0] == '"' || s[0] == '{' || s[0] == '[')
        return 1;

    char *end = NULL;
    errno = 0;
    strtod(s, &end);
    if (errno == 0 && end && *end == '\0')
        return 1;

    return 0;
}

static int validate_json_fragment(const char *frag)
{
    jsmn_parser p;
    jsmntok_t toks[32];

    jsmn_init(&p);
    int r = jsmn_parse(&p, frag, strlen(frag), toks, 32);
    if (r < 1)
        return -1;
    return 0;
}

static char *normalize_set_value(const char *arg)
{
    if (looks_like_json_value(arg)) {
        if (validate_json_fragment(arg) == 0)
            return xstrdup2(arg);
    }
    return json_escape_string(arg);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void append_char(char **buf, size_t *len, size_t *cap, char c)
{
    if (*len + 2 > *cap) {
        *cap = *cap ? (*cap * 2) : 64;
        *buf = xrealloc(*buf, *cap);
    }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = '\0';
}

static void append_utf8(char **buf, size_t *len, size_t *cap, unsigned cp)
{
    if (cp <= 0x7F) {
        append_char(buf, len, cap, (char)cp);
    } else if (cp <= 0x7FF) {
        append_char(buf, len, cap, (char)(0xC0 | ((cp >> 6) & 0x1F)));
        append_char(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        append_char(buf, len, cap, (char)(0xE0 | ((cp >> 12) & 0x0F)));
        append_char(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
        append_char(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    } else {
        append_char(buf, len, cap, (char)(0xF0 | ((cp >> 18) & 0x07)));
        append_char(buf, len, cap, (char)(0x80 | ((cp >> 12) & 0x3F)));
        append_char(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
        append_char(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    }
}

static char *json_unescape_string_token(const char *js, const jsmntok_t *tok)
{
    const char *s = js + tok->start;
    size_t n = (size_t)(tok->end - tok->start);
    char *out = NULL;
    size_t len = 0, cap = 0;

    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\') {
            append_char(&out, &len, &cap, s[i]);
            continue;
        }

        i++;
        if (i >= n)
            break;

        switch (s[i]) {
        case '"': append_char(&out, &len, &cap, '"'); break;
        case '\\': append_char(&out, &len, &cap, '\\'); break;
        case '/': append_char(&out, &len, &cap, '/'); break;
        case 'b': append_char(&out, &len, &cap, '\b'); break;
        case 'f': append_char(&out, &len, &cap, '\f'); break;
        case 'n': append_char(&out, &len, &cap, '\n'); break;
        case 'r': append_char(&out, &len, &cap, '\r'); break;
        case 't': append_char(&out, &len, &cap, '\t'); break;
        case 'u':
            if (i + 4 < n) {
                int h1 = hexval(s[i + 1]);
                int h2 = hexval(s[i + 2]);
                int h3 = hexval(s[i + 3]);
                int h4 = hexval(s[i + 4]);
                if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
                    unsigned cp = ((unsigned)h1 << 12) |
                                  ((unsigned)h2 << 8) |
                                  ((unsigned)h3 << 4) |
                                  (unsigned)h4;
                    append_utf8(&out, &len, &cap, cp);
                    i += 4;
                } else {
                    append_char(&out, &len, &cap, '\\');
                    append_char(&out, &len, &cap, 'u');
                }
            } else {
                append_char(&out, &len, &cap, '\\');
                append_char(&out, &len, &cap, 'u');
            }
            break;
        default:
            append_char(&out, &len, &cap, s[i]);
            break;
        }
    }

    if (!out)
        out = xstrdup2("");
    return out;
}

static int print_token_value(const char *js, const jsmntok_t *tok, get_mode_t mode)
{
    int len = tok->end - tok->start;

    if (mode == GET_JSON) {
        if (tok->type == JSMN_STRING) {
            fputc('"', stdout);
            fwrite(js + tok->start, 1, len, stdout);
            fputc('"', stdout);
            fputc('\n', stdout);
            return 0;
        }
        fwrite(js + tok->start, 1, len, stdout);
        fputc('\n', stdout);
        return 0;
    }

    if (tok->type == JSMN_STRING) {
        char *u = json_unescape_string_token(js, tok);
        fputs(u, stdout);
        fputc('\n', stdout);
        free(u);
        return 0;
    }

    fwrite(js + tok->start, 1, len, stdout);
    fputc('\n', stdout);
    return 0;
}

static char *replace_range(const char *src, size_t src_len,
                           size_t start, size_t end,
                           const char *rep, size_t rep_len,
                           size_t *out_len)
{
    if (start > end || end > src_len)
        return NULL;

    size_t new_len = start + rep_len + (src_len - end);
    char *out = xmalloc(new_len + 1);

    memcpy(out, src, start);
    memcpy(out + start, rep, rep_len);
    memcpy(out + start + rep_len, src + end, src_len - end);
    out[new_len] = '\0';

    if (out_len)
        *out_len = new_len;
    return out;
}

static range_t compute_object_member_delete_range(const char *js, size_t len,
                                                  const jsmntok_t *toks,
                                                  int obj_idx, int key_idx, int val_idx,
                                                  int member_pos)
{
    range_t r = {0, 0};
    const jsmntok_t *obj = &toks[obj_idx];
    int obj_open = obj->start;

    int key_start = toks[key_idx].start - 1; /* include opening quote */
    int val_end = toks[val_idx].end;
    /* For string values, jsmn end is at the closing quote — include it */
    if (toks[val_idx].type == JSMN_STRING)
        val_end++;

    int next = skip_ws_forward(js, len, val_end);
    int prev = skip_ws_backward(js, obj_open + 1, key_start);

    bool has_comma_after = ((size_t)next < len && js[next] == ',');
    bool has_comma_before = (prev > obj_open && js[prev - 1] == ',');

    if (has_comma_after) {
        int end = next + 1;
        while ((size_t)end < len && is_ws(js[end]))
            end++;

        if (member_pos == 0) {
            r.start = (size_t)key_start;
            r.end = (size_t)end;
        } else {
            int start = key_start;
            while (start > obj_open + 1 &&
                   is_ws(js[start - 1]) &&
                   !has_newline_between(js, start - 1, start)) {
                start--;
            }
            r.start = (size_t)start;
            r.end = (size_t)end;
        }
        return r;
    }

    if (has_comma_before) {
        int start = prev - 1;
        while (start > obj_open + 1 && is_ws(js[start - 1]))
            start--;
        r.start = (size_t)start;
        r.end = (size_t)val_end;
        return r;
    }

    r.start = (size_t)key_start;
    r.end = (size_t)val_end;
    return r;
}

static range_t compute_array_delete_range(const char *js, size_t len,
                                          const jsmntok_t *toks,
                                          int arr_idx, int elem_idx)
{
    range_t r = {0, 0};
    const jsmntok_t *arr = &toks[arr_idx];
    int arr_open = arr->start;

    int start = toks[elem_idx].start;
    int end = toks[elem_idx].end;
    /* For string elements, jsmn boundaries exclude surrounding quotes */
    if (toks[elem_idx].type == JSMN_STRING) {
        start--;  /* include opening quote */
        end++;    /* include closing quote */
    }

    int next = skip_ws_forward(js, len, end);
    int prev = skip_ws_backward(js, arr_open + 1, start);

    bool has_comma_after = ((size_t)next < len && js[next] == ',');
    bool has_comma_before = (prev > arr_open && js[prev - 1] == ',');

    if (has_comma_after) {
        int del_end = next + 1;
        while ((size_t)del_end < len && is_ws(js[del_end]))
            del_end++;
        r.start = (size_t)start;
        r.end = (size_t)del_end;
        return r;
    }

    if (has_comma_before) {
        int del_start = prev - 1;
        while (del_start > arr_open + 1 && is_ws(js[del_start - 1]))
            del_start--;
        r.start = (size_t)del_start;
        r.end = (size_t)end;
        return r;
    }

    r.start = (size_t)start;
    r.end = (size_t)end;
    return r;
}

static void detect_object_style(const char *js, const jsmntok_t *obj,
                                const char **indent_ptr, size_t *indent_len,
                                const char **colon_sep_ptr, size_t *colon_sep_len,
                                int *multiline)
{
    *indent_ptr = "  ";
    *indent_len = 2;
    *colon_sep_ptr = ": ";
    *colon_sep_len = 2;
    *multiline = 0;

    int open = obj->start;
    int close = obj->end - 1;

    for (int i = open + 1; i < close; i++) {
        if (js[i] == '\n') {
            *multiline = 1;
            int j = i + 1;
            while (j < close && (js[j] == ' ' || js[j] == '\t'))
                j++;
            *indent_ptr = js + i + 1;
            *indent_len = (size_t)(j - (i + 1));
            break;
        }
        if (!is_ws(js[i]))
            break;
    }

    for (int i = open + 1; i < close; i++) {
        if (js[i] == ':') {
            int j = i + 1;
            while (j < close && (js[j] == ' ' || js[j] == '\t'))
                j++;
            *colon_sep_ptr = js + i;
            *colon_sep_len = (size_t)(j - i);
            if (*colon_sep_len == 1) {
                *colon_sep_ptr = ":";
                *colon_sep_len = 1;
            }
            break;
        }
    }
}

static char *build_nested_object_json(const path_vec_t *path, int start_idx, const char *leaf_json)
{
    char *cur = xstrdup2(leaf_json);

    for (int i = path->n - 1; i >= start_idx; i--) {
        char *escaped_key = json_escape_string(path->v[i].key);
        size_t need = strlen(escaped_key) + strlen(cur) + 8;
        char *next = xmalloc(need);
        snprintf(next, need, "{%s:%s}", escaped_key, cur);
        free(escaped_key);
        free(cur);
        cur = next;
    }

    return cur;
}

static char *build_new_member_text(const char *js, const jsmntok_t *obj,
                                   const char *key, const char *value_json)
{
    const char *indent, *colon_sep;
    size_t indent_len, colon_sep_len;
    int multiline;

    detect_object_style(js, obj, &indent, &indent_len, &colon_sep, &colon_sep_len, &multiline);

    char *ekey = json_escape_string(key);

    if (multiline) {
        int close = obj->end - 1;
        int line_start = close;
        while (line_start > obj->start && js[line_start - 1] != '\n')
            line_start--;

        size_t closing_indent_len = (size_t)(close - line_start);
        const char *closing_indent = js + line_start;

        size_t need = 1 + indent_len + strlen(ekey) + colon_sep_len +
                      strlen(value_json) + 1 + closing_indent_len + 1;
        char *out = xmalloc(need + 1);
        size_t p = 0;

        out[p++] = '\n';
        memcpy(out + p, indent, indent_len); p += indent_len;
        memcpy(out + p, ekey, strlen(ekey)); p += strlen(ekey);
        memcpy(out + p, colon_sep, colon_sep_len); p += colon_sep_len;
        memcpy(out + p, value_json, strlen(value_json)); p += strlen(value_json);
        out[p++] = '\n';
        memcpy(out + p, closing_indent, closing_indent_len); p += closing_indent_len;
        out[p] = '\0';

        free(ekey);
        return out;
    } else {
        size_t need = strlen(ekey) + colon_sep_len + strlen(value_json) + 1;
        char *out = xmalloc(need + 1);
        snprintf(out, need + 1, "%s%.*s%s", ekey, (int)colon_sep_len, colon_sep, value_json);
        free(ekey);
        return out;
    }
}

static char *insert_into_object(const char *src, size_t src_len, const jsmntok_t *obj,
                                const char *member_text, size_t *out_len)
{
    int close = obj->end - 1;
    bool empty = true;

    for (int i = obj->start + 1; i < close; i++) {
        if (!is_ws(src[i])) {
            empty = false;
            break;
        }
    }

    if (empty) {
        return replace_range(src, src_len, (size_t)close, (size_t)close,
                             member_text, strlen(member_text), out_len);
    }

    /* Non-empty object: prepend comma before new member */
    size_t rep_len = 1 + strlen(member_text);
    char *rep = xmalloc(rep_len + 1);
    rep[0] = ',';
    memcpy(rep + 1, member_text, strlen(member_text));
    rep[rep_len] = '\0';

    char *out = replace_range(src, src_len, (size_t)close, (size_t)close,
                              rep, rep_len, out_len);
    free(rep);
    return out;
}

static int find_parent_container(const char *js, const jsmntok_t *toks, int ntok,
                                 const path_vec_t *path, int *out_parent_idx, int *out_last_is_key)
{
    if (path->n == 0)
        return -1;

    if (path->n == 1) {
        *out_parent_idx = 0;
        *out_last_is_key = (path->v[0].type == SEG_KEY);
        return 0;
    }

    int cur = 0;
    for (int i = 0; i < path->n - 1; i++) {
        path_seg_t *seg = &path->v[i];
        if (seg->type == SEG_KEY)
            cur = object_find_value(js, toks, ntok, cur, seg->key);
        else
            cur = array_find_value(toks, ntok, cur, seg->index);

        if (cur < 0)
            return -1;
    }

    *out_parent_idx = cur;
    *out_last_is_key = (path->v[path->n - 1].type == SEG_KEY);
    return 0;
}

int main(int argc, char **argv)
{
    cli_mode_t mode = MODE_NONE;
    get_mode_t get_mode = GET_AUTO;
    const char *input = NULL;
    const char *output = NULL;
    const char *path_arg = NULL;
    const char *set_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--get")) {
            mode = MODE_GET;
            if (++i >= argc) die("Missing path for --get");
            path_arg = argv[i];
        } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--set")) {
            mode = MODE_SET;
            if (++i >= argc) die("Missing path for --set");
            path_arg = argv[i];
            if (++i >= argc) die("Missing value for --set");
            set_arg = argv[i];
        } else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--del")) {
            mode = MODE_DEL;
            if (++i >= argc) die("Missing path for --del");
            path_arg = argv[i];
        } else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--input")) {
            if (++i >= argc) die("Missing file for --input");
            input = argv[i];
        } else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
            if (++i >= argc) die("Missing file for --output");
            output = argv[i];
        } else if (!strcmp(argv[i], "--raw")) {
            get_mode = GET_RAW;
        } else if (!strcmp(argv[i], "--json")) {
            get_mode = GET_JSON;
        } else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--canonical")) {
            /* compatibility only */
        } else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--unicode")) {
            /* compatibility only */
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (mode == MODE_NONE)
        die("No mode specified, use -s, -g, or -d");

    if (!input)
        input = "config.json";
    if (!output)
        output = input;

    path_vec_t path = {0};
    if (parse_path(path_arg, &path) != 0) {
        path_free(&path);
        return 1;
    }

    buffer_t buf = read_file(input);
    if (!buf.data) {
        path_free(&path);
        return 1;
    }

    jsmntok_t *toks = NULL;
    int ntok = 0;
    if (parse_json_tokens(buf.data, buf.len, &toks, &ntok) != 0) {
        free(buf.data);
        path_free(&path);
        return 1;
    }

    if (ntok <= 0 || toks[0].type != JSMN_OBJECT) {
        fprintf(stderr, "Root JSON must be an object\n");
        free(toks);
        free(buf.data);
        path_free(&path);
        return 1;
    }

    if (mode == MODE_GET) {
        int val_idx = find_path_value(buf.data, toks, ntok, &path);
        if (val_idx < 0) {
            fprintf(stderr, "Key not found\n");
            free(toks);
            free(buf.data);
            path_free(&path);
            return 1;
        }
        print_token_value(buf.data, &toks[val_idx], get_mode);
    } else if (mode == MODE_SET) {
        int val_idx = find_path_value(buf.data, toks, ntok, &path);

        if (val_idx >= 0) {
            char *new_value = normalize_set_value(set_arg);
            /* For string tokens, jsmn start/end are inside the quotes.
             * Expand range to include surrounding quotes so the replacement
             * (which includes its own quotes for strings) doesn't double-quote. */
            size_t rep_start = (size_t)toks[val_idx].start;
            size_t rep_end = (size_t)toks[val_idx].end;
            if (toks[val_idx].type == JSMN_STRING) {
                rep_start--;  /* include opening quote */
                rep_end++;    /* include closing quote */
            }
            size_t out_len = 0;
            char *out = replace_range(buf.data, buf.len,
                                      rep_start, rep_end,
                                      new_value, strlen(new_value),
                                      &out_len);
            free(new_value);

            if (!out) {
                fprintf(stderr, "Failed to apply replacement\n");
                free(toks);
                free(buf.data);
                path_free(&path);
                return 1;
            }

            if (write_file(output, out, out_len) != 0) {
                free(out);
                free(toks);
                free(buf.data);
                path_free(&path);
                return 1;
            }
            free(out);
        } else {
            int prefix = find_existing_prefix(buf.data, toks, ntok, &path);
            int missing_from = prefix + 1;

            if (missing_from < 0 || missing_from >= path.n) {
                fprintf(stderr, "Internal path resolution error\n");
                free(toks);
                free(buf.data);
                path_free(&path);
                return 1;
            }

            if (!path_is_object_only_suffix(&path, missing_from)) {
                fprintf(stderr, "Creating missing array elements is not supported\n");
                free(toks);
                free(buf.data);
                path_free(&path);
                return 1;
            }

            int parent_idx;
            if (prefix < 0) {
                parent_idx = 0;
            } else {
                path_vec_t prefix_path = {
                    .v = path.v,
                    .n = prefix + 1,
                    .cap = path.cap
                };
                parent_idx = find_path_value(buf.data, toks, ntok, &prefix_path);
            }

            if (parent_idx < 0 || toks[parent_idx].type != JSMN_OBJECT) {
                fprintf(stderr, "Missing path parent is not an object\n");
                free(toks);
                free(buf.data);
                path_free(&path);
                return 1;
            }

            char *insert_value = NULL;
            if (missing_from == path.n - 1) {
                insert_value = normalize_set_value(set_arg);
            } else {
                char *leaf = normalize_set_value(set_arg);
                insert_value = build_nested_object_json(&path, missing_from + 1, leaf);
                free(leaf);
            }

            char *member_text = build_new_member_text(buf.data, &toks[parent_idx],
                                                      path.v[missing_from].key, insert_value);
            free(insert_value);

            size_t out_len = 0;
            char *out = insert_into_object(buf.data, buf.len, &toks[parent_idx],
                                           member_text, &out_len);
            free(member_text);

            if (!out) {
                fprintf(stderr, "Failed to insert new member\n");
                free(toks);
                free(buf.data);
                path_free(&path);
                return 1;
            }

            if (write_file(output, out, out_len) != 0) {
                free(out);
                free(toks);
                free(buf.data);
                path_free(&path);
                return 1;
            }
            free(out);
        }
    } else if (mode == MODE_DEL) {
        int parent_idx, last_is_key;
        if (find_parent_container(buf.data, toks, ntok, &path, &parent_idx, &last_is_key) != 0) {
            fprintf(stderr, "Key not found\n");
            free(toks);
            free(buf.data);
            path_free(&path);
            return 1;
        }

        range_t del = {0, 0};

        if (last_is_key) {
            int key_idx, val_idx, member_pos;
            if (object_find_member(buf.data, toks, ntok, parent_idx,
                                   path.v[path.n - 1].key,
                                   &key_idx, &val_idx, &member_pos) != 0) {
                fprintf(stderr, "Key not found\n");
                free(toks);
                free(buf.data);
                path_free(&path);
                return 1;
            }

            del = compute_object_member_delete_range(buf.data, buf.len, toks,
                                                     parent_idx, key_idx, val_idx, member_pos);
        } else {
            int elem_idx = array_find_value(toks, ntok, parent_idx, path.v[path.n - 1].index);
            if (elem_idx < 0) {
                fprintf(stderr, "Index not found\n");
                free(toks);
                free(buf.data);
                path_free(&path);
                return 1;
            }

            del = compute_array_delete_range(buf.data, buf.len, toks,
                                             parent_idx, elem_idx);
        }

        if (del.end < del.start || del.end > buf.len) {
            fprintf(stderr, "Failed to compute delete range\n");
            free(toks);
            free(buf.data);
            path_free(&path);
            return 1;
        }

        size_t out_len = 0;
        char *out = replace_range(buf.data, buf.len, del.start, del.end, "", 0, &out_len);
        if (!out) {
            fprintf(stderr, "Failed to delete range\n");
            free(toks);
            free(buf.data);
            path_free(&path);
            return 1;
        }

        if (write_file(output, out, out_len) != 0) {
            free(out);
            free(toks);
            free(buf.data);
            path_free(&path);
            return 1;
        }

        free(out);
    }

    free(toks);
    free(buf.data);
    path_free(&path);
    return 0;
}
