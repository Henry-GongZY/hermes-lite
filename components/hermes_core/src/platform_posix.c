#include "hermes_lite.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

/* POSIX backend: libcurl (rustls-free, system TLS) + stdio filesystem. */

typedef struct {
    char *buf;
    size_t cap, len;
    int overflow;
} rx_t;

static size_t on_data(char *ptr, size_t sz, size_t nm, void *ud)
{
    rx_t *rx = ud;
    size_t total = sz * nm;
    size_t room = rx->cap > rx->len ? rx->cap - rx->len : 0;
    if (total > room) {
        memcpy(rx->buf + rx->len, ptr, room);
        rx->len = rx->cap;
        rx->overflow = 1;
    } else {
        memcpy(rx->buf + rx->len, ptr, total);
        rx->len += total;
    }
    return total; /* consume everything; overflow flagged separately */
}

static int http_post(const char *url, const char *auth_bearer,
                     const char *body, size_t body_len,
                     char *resp, size_t resp_cap, size_t *resp_len)
{
    static int inited;
    if (!inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        inited = 1;
    }
    rx_t rx = { resp, resp_cap, 0, 0 };
    CURL *c = curl_easy_init();
    if (!c)
        return -1;

    char auth[512];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", auth_bearer);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, auth);

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &rx);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        fprintf(stderr, "[hl] curl error: %s\n", curl_easy_strerror(rc));
        return -1;
    }
    if (rx.overflow)
        return -2;
    *resp_len = rx.len;
    return (int)code;
}

/* ---- SSE streaming ---- */

#define STREAM_LINE_CAP 65536

typedef struct {
    char line[STREAM_LINE_CAP];
    size_t len;
    int status;
    int aborted;
    int overflow;
    int (*on_line)(void *, const char *);
    void *ud;
    char *errbuf;
    size_t errcap, errlen;
} sctx_t;

static void sctx_dispatch(sctx_t *s)
{
    s->line[s->len] = 0;
    if (s->len > 0 && s->line[s->len - 1] == '\r')
        s->line[s->len - 1] = 0;
    s->len = 0;
    if (strncmp(s->line, "data:", 5) != 0)
        return; /* comments, event: lines, blanks */
    const char *payload = s->line + 5;
    if (*payload == ' ')
        payload++;
    if (!*payload)
        return;
    if (s->on_line(s->ud, payload))
        s->aborted = 1;
}

static size_t stream_write(char *ptr, size_t sz, size_t nm, void *vp)
{
    sctx_t *s = vp;
    size_t total = sz * nm;
    if (s->status != 200) {
        size_t room = s->errcap > s->errlen ? s->errcap - s->errlen : 0;
        size_t n = total > room ? room : total;
        memcpy(s->errbuf + s->errlen, ptr, n);
        s->errlen += n;
        return total;
    }
    for (size_t i = 0; i < total; i++) {
        char ch = ptr[i];
        if (ch == '\n') {
            sctx_dispatch(s);
            if (s->aborted)
                return i + 1; /* short write aborts the transfer */
            continue;
        }
        if (s->len + 1 >= STREAM_LINE_CAP) {
            s->overflow = 1;
            return i + 1;
        }
        s->line[s->len++] = ch;
    }
    return total;
}

static size_t stream_header(char *ptr, size_t sz, size_t nm, void *vp)
{
    sctx_t *s = vp;
    if (s->status == 0 && !strncmp(ptr, "HTTP/", 5)) {
        /* "HTTP/1.1 200 OK" and "HTTP/2 200" both parse */
        const char *p = ptr + 5;
        while (*p && *p != ' ')
            p++;
        while (*p == ' ')
            p++;
        s->status = atoi(p);
    }
    return sz * nm;
}

static int http_post_stream(const char *url, const char *auth_bearer,
                            const char *body, size_t body_len,
                            int (*on_line)(void *, const char *), void *ud,
                            char *errbuf, size_t errcap)
{
    static int inited;
    if (!inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        inited = 1;
    }
    sctx_t s = { 0 };
    s.on_line = on_line;
    s.ud = ud;
    s.errbuf = errbuf;
    s.errcap = errcap;

    CURL *c = curl_easy_init();
    if (!c)
        return -1;
    char auth[512];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", auth_bearer);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, stream_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &s);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(c);
    long code = s.status;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (s.overflow)
        return -2;
    if (s.aborted)
        return -3;
    if (rc != CURLE_OK) {
        fprintf(stderr, "[hl] curl stream error: %s (code=%ld, got %zu bytes)\n",
                curl_easy_strerror(rc), code, s.len);
        if (code == 0 || s.len == 0)
            return -1;
        /* partial transfer: hand back whatever arrived with its status */
        return (int)code;
    }
    if (code == 0)
        return -1;
    return (int)code;
}

/* ---- HTTPS GET (OTA skill sync) ---- */

static int http_get(const char *url, const char *auth_bearer,
                    char *resp, size_t resp_cap, size_t *resp_len)
{
    static int inited;
    if (!inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        inited = 1;
    }
    rx_t rx = { resp, resp_cap, 0, 0 };
    CURL *c = curl_easy_init();
    if (!c)
        return -1;
    char auth[512];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", auth_bearer);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &rx);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        fprintf(stderr, "[hl] curl error: %s\n", curl_easy_strerror(rc));
        return -1;
    }
    if (rx.overflow)
        return -2;
    *resp_len = rx.len;
    return (int)code;
}

static int fs_read(const char *path, char *buf, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    *len = fread(buf, 1, cap, f);
    fclose(f);
    return 0;
}

static int fs_write(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

static int fs_append(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "ab");
    if (!f)
        return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

typedef struct {
    int (*cb)(void *, const char *);
    void *ud;
    int rc;
} list_t;

static int list_cb(void *ud, const char *name)
{
    list_t *l = ud;
    if (l->cb(l->ud, name)) {
        l->rc = 1;
        return 1;
    }
    return 0;
}

static int fs_list(const char *dir, int (*cb)(void *, const char *), void *ud)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    list_t l = { cb, ud, 0 };
    struct dirent *e;
    while ((e = readdir(d)) && !list_cb(&l, e->d_name))
        ;
    closedir(d);
    return 0;
}

static unsigned long time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000);
}

static void delay_ms(unsigned long ms)
{
    struct timespec ts = {
        .tv_sec = (time_t)(ms / 1000),
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static void log_fn(int level, const char *msg)
{
    const char *tag = level == HL_LOG_ERROR ? "E" : level == HL_LOG_DEBUG ? "D" : "I";
    fprintf(stderr, "[hl:%s] %s\n", tag, msg);
}

static const hl_platform_t g_platform = {
    .http_post = http_post,
    .http_post_stream = http_post_stream,
    .http_get = http_get,
    .fs_read = fs_read,
    .fs_write = fs_write,
    .fs_append = fs_append,
    .fs_list = fs_list,
    .time_ms = time_ms,
    .delay_ms = delay_ms,
    .log = log_fn,
};

const hl_platform_t *hl_platform_posix(void)
{
    return &g_platform;
}
