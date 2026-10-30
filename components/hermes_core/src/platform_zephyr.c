#include "hermes_lite.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(hl_zephyr, CONFIG_LOG_DEFAULT_LEVEL);

/* Zephyr backend: TLS sockets (zsock + mbedTLS) + littlefs.
 *
 * TLS credentials: register the server CA once at boot with
 * hl_zephyr_set_ca(); http:// URLs skip TLS for lab setups. */

static const char *s_ca_pem;

void hl_zephyr_set_ca(const char *pem)
{
#ifdef CONFIG_NET_SOCKETS_SOCKOPT_TLS
    s_ca_pem = pem;
    tls_credential_add(TLS_CREDENTIAL_CA_CERTIFICATE, 1, pem, strlen(pem) + 1);
#else
    (void)pem;
#endif
}

/* ---------------- HTTP over zsock ---------------- */

#define HL_HEAD_CAP 2048

typedef struct {
    int fd;
    int status;
    char *errbuf;
    size_t errcap, errlen;
    char *line;
    size_t linelen, linecap;
    hl_line_fn on_line;
    void *ud;
    int aborted;
} zhttp_t;

static int zconnect(const char *hostbuf, int is_tls)
{
    struct zsock_addrinfo *res = NULL;
    struct zsock_addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    const char *port = is_tls ? "443" : "80";
    if (zsock_getaddrinfo(hostbuf, port, &hints, &res) != 0 || !res)
        return -1;
#ifdef CONFIG_NET_SOCKETS_SOCKOPT_TLS
    int proto = is_tls ? IPPROTO_TLS_1_2 : IPPROTO_TCP;
    int fd = zsock_socket(res->ai_family, SOCK_STREAM, proto);
    if (fd < 0) {
        zsock_freeaddrinfo(res);
        return -1;
    }
    if (is_tls) {
        int verify = TLS_PEER_VERIFY_REQUIRED;
        zsock_setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
        zsock_setsockopt(fd, SOL_TLS, TLS_HOSTNAME, hostbuf, strlen(hostbuf));
    }
#else
    /* TLS-off build (lab/plain HTTP); see sample prj.conf note */
    (void)is_tls;
    int fd = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        zsock_freeaddrinfo(res);
        return -1;
    }
#endif
    int rc = zsock_connect(fd, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    if (rc < 0) {
        zsock_close(fd);
        return -1;
    }
    return fd;
}

static int zsend_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = zsock_send(fd, buf + off, len - off, 0);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static void zfeed(zhttp_t *h, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        char ch = data[i];
        if (ch == '\n') {
            h->line[h->linelen] = 0;
            if (h->linelen && h->line[h->linelen - 1] == '\r')
                h->line[h->linelen - 1] = 0;
            if (!strncmp(h->line, "data:", 5) && !h->aborted) {
                const char *payload = h->line + 5;
                if (*payload == ' ')
                    payload++;
                if (*payload && h->on_line(h->ud, payload))
                    h->aborted = 1;
            }
            h->linelen = 0;
            continue;
        }
        if (h->linelen + 1 >= h->linecap) {
            h->aborted = 1;
            return;
        }
        h->line[h->linelen++] = ch;
    }
}

static int zhttp_run(const char *url, const char *auth_bearer,
                     const char *body, size_t body_len,
                     hl_line_fn on_line, void *ud,
                     char *resp, size_t resp_cap, size_t *resp_len,
                     char *errbuf, size_t errcap)
{
    /* url: [http(s)://]host[:port]/path */
    const char *proto = strstr(url, "://");
    int is_tls = (proto && !strncmp(url, "https", 5));
    const char *host = proto ? proto + 3 : url;
    const char *path = strchr(host, '/');
    if (!path)
        path = "/";
    size_t hostlen = (size_t)(path - host);
    char hostbuf[128];
    if (hostlen == 0 || hostlen >= sizeof hostbuf)
        return -1;
    memcpy(hostbuf, host, hostlen);
    hostbuf[hostlen] = 0;

    char head[HL_HEAD_CAP];
    int head_len = snprintf(head, sizeof head,
                            "POST %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "Content-Type: application/json\r\n"
                            "Authorization: Bearer %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Accept: text/event-stream\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            path, hostbuf, auth_bearer, body_len);
    if (head_len < 0 || (size_t)head_len >= sizeof head)
        return -1;

    zhttp_t h = { 0 };
    h.on_line = on_line;
    h.ud = ud;
    h.errbuf = errbuf;
    h.errcap = errcap;
    h.linecap = 8192;
    h.line = malloc(h.linecap);
    if (!h.line)
        return -1;

    h.fd = zconnect(hostbuf, is_tls);
    if (h.fd < 0) {
        free(h.line);
        return -1;
    }
    if (zsend_all(h.fd, head, (size_t)head_len) < 0 ||
        zsend_all(h.fd, body, body_len) < 0) {
        zsock_close(h.fd);
        free(h.line);
        return -1;
    }

    /* header accumulator: wait for CRLFCRLF, then route the remainder */
    char hacc[HL_HEAD_CAP];
    size_t hlen = 0;
    int head_done = 0;
    char rbuf[2048];

    for (;;) {
        int n = zsock_recv(h.fd, rbuf, sizeof rbuf, 0);
        if (n <= 0)
            break;
        const char *rest = rbuf;
        int rest_len = n;

        if (!head_done) {
            for (int i = 0; i < n; i++) {
                if (hlen + 1 < HL_HEAD_CAP)
                    hacc[hlen++] = rbuf[i];
                if (hlen >= 4 && !memcmp(hacc + hlen - 4, "\r\n\r\n", 4)) {
                    head_done = 1;
                    hacc[hlen - 2] = 0; /* drop final CRLF */
                    for (const char *q = hacc; q + 8 <= hacc + hlen; q++) {
                        if (!strncmp(q, "HTTP/1.", 7)) {
                            h.status = atoi(q + 9);
                            break;
                        }
                    }
                    rest = rbuf + i + 1;
                    rest_len = n - (i + 1);
                    break;
                }
            }
            if (!head_done)
                continue;
        }

        if (rest_len <= 0)
            continue;
        if (h.status != 200) {
            size_t room = h.errcap > h.errlen ? h.errcap - h.errlen : 0;
            size_t cp = (size_t)rest_len > room ? room : (size_t)rest_len;
            memcpy(h.errbuf + h.errlen, rest, cp);
            h.errlen += cp;
        } else if (on_line) {
            zfeed(&h, rest, rest_len);
            if (h.aborted)
                break;
        } else {
            size_t room = resp_cap > *resp_len ? resp_cap - *resp_len : 0;
            size_t cp = (size_t)rest_len > room ? room : (size_t)rest_len;
            memcpy(resp + *resp_len, rest, cp);
            *resp_len += cp;
            if (*resp_len >= resp_cap)
                break;
        }
    }
    zsock_close(h.fd);
    free(h.line);

    if (h.status == 0)
        return -1;
    if (h.aborted)
        return -2;
    return h.status;
}

static int http_post(const char *url, const char *auth_bearer,
                     const char *body, size_t body_len,
                     char *resp, size_t resp_cap, size_t *resp_len)
{
    *resp_len = 0;
    return zhttp_run(url, auth_bearer, body, body_len, NULL, NULL,
                     resp, resp_cap, resp_len, NULL, 0);
}

static int http_post_stream(const char *url, const char *auth_bearer,
                            const char *body, size_t body_len,
                            hl_line_fn on_line, void *ud,
                            char *errbuf, size_t errcap)
{
    return zhttp_run(url, auth_bearer, body, body_len, on_line, ud,
                     NULL, 0, NULL, errbuf, errcap);
}

/* ---------------- littlefs ---------------- */

#define HL_MOUNT "/hl"
static struct fs_littlefs s_lfs;
static bool s_fs_ready;

int hl_zephyr_fs_init(void)
{
#ifdef FLASH_AREA_STORAGE_OFFSET
    const struct flash_area *fa;
    if (flash_area_open(FLASH_AREA_ID(storage), &fa) < 0)
        return -1;
    s_lfs.area = fa;
    struct fs_mount_t mnt = {
        .type = FS_LITTLEFS,
        .fs_data = &s_lfs,
        .storage_dev = NULL,
        .mnt_point = HL_MOUNT,
    };
    int rc = fs_mount(&mnt);
    s_fs_ready = rc == 0;
    return s_fs_ready ? 0 : -1;
#else
    LOG_WRN("no storage flash area; filesystem disabled");
    return -1;
#endif
}

static char zpath[256];
static const char *zfull(const char *path)
{
    snprintf(zpath, sizeof zpath, "%s/%s", HL_MOUNT, path);
    return zpath;
}

static int zfs_read(const char *path, char *buf, size_t cap, size_t *len)
{
    if (!s_fs_ready)
        return -1;
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, zfull(path), FS_O_READ) < 0)
        return -1;
    *len = fs_read(&f, buf, cap);
    fs_close(&f);
    return 0;
}

static int zfs_write(const char *path, const char *data, size_t len)
{
    if (!s_fs_ready)
        return -1;
    fs_unlink(zfull(path)); /* littlefs: rewrite fresh instead of truncate */
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, zfull(path), FS_O_WRITE | FS_O_CREATE) < 0)
        return -1;
    ssize_t n = fs_write(&f, data, len);
    fs_close(&f);
    return (n >= 0 && (size_t)n == len) ? 0 : -1;
}

static int zfs_append(const char *path, const char *data, size_t len)
{
    if (!s_fs_ready)
        return -1;
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, zfull(path), FS_O_WRITE | FS_O_CREATE | FS_O_APPEND) < 0)
        return -1;
    ssize_t n = fs_write(&f, data, len);
    fs_close(&f);
    return (n >= 0 && (size_t)n == len) ? 0 : -1;
}

static int zfs_list(const char *dir, int (*cb)(void *, const char *), void *ud)
{
    if (!s_fs_ready)
        return -1;
    struct fs_dir_t d;
    fs_dir_t_init(&d);
    if (fs_opendir(&d, zfull(dir)) < 0)
        return -1;
    struct fs_dirent ent;
    for (;;) {
        if (fs_readdir(&d, &ent) < 0)
            break;
        if (ent.name[0] == 0)
            break;
        if (cb(ud, ent.name))
            break;
    }
    fs_closedir(&d);
    return 0;
}

static unsigned long time_ms(void)
{
    return (unsigned long)k_uptime_get();
}

static void log_fn(int level, const char *msg)
{
    printk("[hl:%c] %s\n",
           level == HL_LOG_ERROR ? 'E' : level == HL_LOG_DEBUG ? 'D' : 'I', msg);
}

static const hl_platform_t g_platform = {
    .http_post = http_post,
    .http_post_stream = http_post_stream,
    .fs_read = zfs_read,
    .fs_write = zfs_write,
    .fs_append = zfs_append,
    .fs_list = zfs_list,
    .time_ms = time_ms,
    .log = log_fn,
};

const hl_platform_t *hl_platform_zephyr(void)
{
    return &g_platform;
}
