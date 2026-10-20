/*
 * main.c —— 武汉科技大学（WUST）校园网自动认证工具
 * ============================================================================
 * 功能：
 *   1. 自动检测网络是否已连通（HTTP GET 探测），已连通则直接提示“已在线”；
 *   2. 未连通时，向深澜认证接口 POST 学号/密码完成认证；
 *   3. 认证成功后再次探测外网，确认“网络已连通，可以正常上网”；
 *   4. 账号密码保存在可执行文件同目录的 wust_config.ini 中，绝不写死在代码里；
 *   5. 支持 -a 静默启动，可配置成开机自启。
 *
 * 编译（要求无警告）：
 *   Windows(MinGW-w64): gcc -std=c99 -Wall -Wextra -pedantic -O2 -o wust-auth.exe main.c -lws2_32
 *   Linux            : gcc -std=c99 -Wall -Wextra -pedantic -O2 -o wust-auth        main.c
 *
 * 说明：
 *   - 为了实现“零依赖、拷贝即用”的分发，这里没有使用 libcurl，
 *     而是用标准 socket 自行实现了一个最小可用的 HTTP/1.1 客户端；
 *   - 认证接口是明文 http:// ，因此不涉及 TLS/证书问题。
 * ============================================================================
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* 平台相关的套接字封装                                                */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601      /* getaddrinfo / inet_ntop 需要 */
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <direct.h>
#  include <sys/stat.h>
#  define WUST_SOCKET             SOCKET
#  define WUST_INVALID_SOCKET     INVALID_SOCKET
#  define wust_close_socket(s)    closesocket(s)
#  define wust_sock_errno()       WSAGetLastError()
#  define WUST_ERR_WOULDBLOCK(e)  ((e) == WSAEWOULDBLOCK)
#  define WUST_ERR_INPROGRESS(e)  ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#  define WUST_ERR_INTR(e)        ((e) == WSAEINTR)
#  define WUST_ERR_TIMEOUT(e)     ((e) == WSAETIMEDOUT)
#  define WUST_SOCKOPT_CAST(p)    ((const char *)(p))
#  define WUST_SOCKOPT_CAST_MUT(p) ((char *)(p))
#  define WUST_SELECT_NFDS(s)     0
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <termios.h>
#  define WUST_SOCKET             int
#  define WUST_INVALID_SOCKET     (-1)
#  define wust_close_socket(s)    close(s)
#  define wust_sock_errno()       errno
#  define WUST_ERR_WOULDBLOCK(e)  ((e) == EWOULDBLOCK || (e) == EAGAIN)
#  define WUST_ERR_INPROGRESS(e)  ((e) == EINPROGRESS || WUST_ERR_WOULDBLOCK(e))
#  define WUST_ERR_INTR(e)        ((e) == EINTR)
#  define WUST_ERR_TIMEOUT(e)     ((e) == 0)   /* POSIX 用 EAGAIN 表示超时 */
#  define WUST_SOCKOPT_CAST(p)    ((const void *)(p))
#  define WUST_SOCKOPT_CAST_MUT(p) ((void *)(p))
#  define WUST_SELECT_NFDS(s)     ((int)(s) + 1)
#endif

/* ------------------------------------------------------------------ */
/* 常量                                                                */
/* ------------------------------------------------------------------ */
#define APP_NAME            "wust-auth"
#define APP_VERSION         "1.2"
#define CONFIG_FILE_NAME    "wust_config.ini"
#define LOG_FILE_NAME       "wust-auth.log"
/* config_load 在没有文件时给出的原因；首次运行时不必当成错误展示 */
#define CONFIG_WHY_NOFILE   "文件不存在"

/* 深澜认证接口（明文 HTTP，POST 表单） */
#define DEFAULT_LOGIN_URL   "http://59.68.177.9/api/account/login"
/* 联网探测用的地址 */
#define DEFAULT_PROBE_URL   "http://www.baidu.com"

#define LOGIN_TIMEOUT_SEC   8
#define PROBE_TIMEOUT_SEC   5
#define MAX_RESPONSE_BYTES  (2u * 1024u * 1024u)

/* 退出码 */
#define EXIT_OK             0
#define EXIT_FAIL           1
#define EXIT_CONFIG         2
#define EXIT_USAGE          3

/* ------------------------------------------------------------------ */
/* 动态字符串缓冲（避免任何 snprintf 截断告警，也便于拼接表单）        */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} strbuf;

static void sb_init(strbuf *sb)
{
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

static void sb_free(strbuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

static void sb_reset(strbuf *sb)
{
    sb->len = 0;
    if (sb->data != NULL) {
        sb->data[0] = '\0';
    }
}

static int sb_reserve(strbuf *sb, size_t extra)
{
    size_t need;
    size_t cap;
    char  *p;

    need = sb->len + extra + 1u;
    if (need <= sb->cap) {
        return 0;
    }
    cap = (sb->cap == 0u) ? 256u : sb->cap;
    while (cap < need) {
        cap *= 2u;
    }
    p = (char *)realloc(sb->data, cap);
    if (p == NULL) {
        return -1;
    }
    sb->data = p;
    sb->cap  = cap;
    if (sb->len == 0u) {
        sb->data[0] = '\0';
    }
    return 0;
}

static int sb_addn(strbuf *sb, const char *s, size_t n)
{
    if (sb_reserve(sb, n) != 0) {
        return -1;
    }
    if (n > 0u) {
        memcpy(sb->data + sb->len, s, n);
        sb->len += n;
    }
    sb->data[sb->len] = '\0';
    return 0;
}

static int sb_add(strbuf *sb, const char *s)
{
    return sb_addn(sb, s, strlen(s));
}

static int sb_addc(strbuf *sb, char c)
{
    return sb_addn(sb, &c, 1u);
}

static int sb_addf(strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;
    int     n;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return -1;
    }
    if (sb_reserve(sb, (size_t)n) != 0) {
        va_end(ap2);
        return -1;
    }
    vsnprintf(sb->data + sb->len, (size_t)n + 1u, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */
static void copy_str(char *dst, size_t dstlen, const char *src)
{
    size_t n;

    if (dstlen == 0u) {
        return;
    }
    n = strlen(src);
    if (n > dstlen - 1u) {
        n = dstlen - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static char *ltrim(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0u && isspace((unsigned char)s[n - 1u])) {
        s[--n] = '\0';
    }
}

static void trim(char *s)
{
    char *p = ltrim(s);
    if (p != s) {
        memmove(s, p, strlen(p) + 1u);
    }
    rtrim(s);
}

static int ci_equal_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0u; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

static void msleep(unsigned ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
#endif
}

/* 终端关闭回显（用于输入密码），在不支持的终端上静默退化 */
static void set_echo(int on)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  mode = 0;
    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        return;
    }
    if (!GetConsoleMode(h, &mode)) {
        return;
    }
    if (on) {
        mode |= (DWORD)ENABLE_ECHO_INPUT;
    } else {
        mode &= ~((DWORD)ENABLE_ECHO_INPUT);
    }
    (void)SetConsoleMode(h, mode);
#else
    struct termios t;
    if (!isatty(fileno(stdin))) {
        return;
    }
    if (tcgetattr(fileno(stdin), &t) != 0) {
        return;
    }
    if (on) {
        t.c_lflag |= ECHO;
    } else {
        t.c_lflag &= (tcflag_t)~((tcflag_t)ECHO);
    }
    (void)tcsetattr(fileno(stdin), TCSANOW, &t);
#endif
}

/* 读取一行；hide=1 时关闭回显。返回 0 成功，-1 表示 EOF/出错 */
static int read_line(const char *prompt, char *buf, size_t buflen, int hide)
{
    size_t n;

    if (buflen == 0u) {
        return -1;
    }
    if (prompt != NULL) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    if (hide) {
        set_echo(0);
    }
    if (fgets(buf, (int)buflen, stdin) == NULL) {
        if (hide) {
            set_echo(1);
            fputc('\n', stdout);
        }
        return -1;
    }
    if (hide) {
        set_echo(1);
        fputc('\n', stdout);
    }
    n = strlen(buf);
    while (n > 0u && (buf[n - 1u] == '\n' || buf[n - 1u] == '\r')) {
        buf[--n] = '\0';
    }
    return 0;
}

static void setup_console(void)
{
#ifdef _WIN32
    /* 让 Windows 控制台按 UTF-8 输出中文，避免乱码 */
    (void)SetConsoleOutputCP(65001u);
    (void)SetConsoleCP(65001u);
#endif
}

/* ------------------------------------------------------------------ */
/* URL 处理                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char host[256];
    char port[16];
    char path[512];
} url_parts;

static int url_parse(const char *url, url_parts *u)
{
    const char *p;
    const char *slash;
    const char *colon;
    size_t      hostlen;
    char        hostport[280];

    if (strncmp(url, "http://", 7u) != 0) {
        return -1;                     /* 只支持明文 HTTP */
    }
    p = url + 7;
    slash = strchr(p, '/');
    hostlen = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
    if (hostlen == 0u || hostlen >= sizeof(hostport)) {
        return -1;
    }
    memcpy(hostport, p, hostlen);
    hostport[hostlen] = '\0';

    colon = strrchr(hostport, ':');
    if (colon != NULL) {
        size_t hl = (size_t)(colon - hostport);
        if (hl == 0u || hl >= sizeof(u->host)) {
            return -1;
        }
        memcpy(u->host, hostport, hl);
        u->host[hl] = '\0';
        copy_str(u->port, sizeof(u->port), colon + 1);
        if (u->port[0] == '\0') {
            return -1;
        }
    } else {
        copy_str(u->host, sizeof(u->host), hostport);
        copy_str(u->port, sizeof(u->port), "80");
    }

    if (slash != NULL) {
        if (strlen(slash) >= sizeof(u->path)) {
            return -1;
        }
        copy_str(u->path, sizeof(u->path), slash);
    } else {
        copy_str(u->path, sizeof(u->path), "/");
    }
    return 0;
}

/* application/x-www-form-urlencoded 编码：密码里的 & % = 空格等必须转义 */
static int url_encode(const char *src, strbuf *out)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p;

    for (p = (const unsigned char *)src; *p != '\0'; p++) {
        unsigned char c = *p;
        if (isalnum((int)c) || c == (unsigned char)'-' || c == (unsigned char)'_'
            || c == (unsigned char)'.' || c == (unsigned char)'~') {
            if (sb_addc(out, (char)c) != 0) {
                return -1;
            }
        } else {
            if (sb_reserve(out, 3u) != 0) {
                return -1;
            }
            out->data[out->len++] = '%';
            out->data[out->len++] = hex[(c >> 4) & 0x0Fu];
            out->data[out->len++] = hex[c & 0x0Fu];
            out->data[out->len]   = '\0';
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 最小 HTTP/1.1 客户端                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    int    ok;                    /* 1 = 收到了 HTTP 响应 */
    int    status;                /* HTTP 状态码，0 表示没解析出来 */
    char  *body;                  /* 响应正文（堆内存，可含 \0 之前的二进制） */
    size_t body_len;
    char   location[512];         /* Location 响应头 */
    char   error[512];            /* 传输层错误描述 */
} http_response;

static void http_response_init(http_response *r)
{
    r->ok        = 0;
    r->status    = 0;
    r->body      = NULL;
    r->body_len  = 0;
    r->location[0] = '\0';
    r->error[0]    = '\0';
}

static void http_response_free(http_response *r)
{
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

static int wust_set_nonblocking(WUST_SOCKET fd, int on)
{
#ifdef _WIN32
    u_long mode = (u_long)(on ? 1u : 0u);
    return ioctlsocket(fd, (long)FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (on) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags);
#endif
}

static void wust_set_socket_timeout(WUST_SOCKET fd, int sec)
{
#ifdef _WIN32
    DWORD ms = (DWORD)sec * 1000u;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, WUST_SOCKOPT_CAST(&ms), (int)sizeof(ms));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, WUST_SOCKOPT_CAST(&ms), (int)sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec  = sec;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, WUST_SOCKOPT_CAST(&tv), (socklen_t)sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, WUST_SOCKOPT_CAST(&tv), (socklen_t)sizeof(tv));
#endif
}

/* 带超时的 TCP 连接（非阻塞 connect + select） */
static WUST_SOCKET wust_tcp_connect(const char *host, const char *port, int timeout_sec,
                                    char *errbuf, size_t errlen)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    WUST_SOCKET      fd = WUST_INVALID_SOCKET;
    int              rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || res == NULL) {
#ifdef _WIN32
        snprintf(errbuf, errlen, "无法解析服务器地址 %s（错误码 %d）", host, rc);
#else
        snprintf(errbuf, errlen, "无法解析服务器地址 %s：%s", host, gai_strerror(rc));
#endif
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return WUST_INVALID_SOCKET;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == WUST_INVALID_SOCKET) {
            continue;
        }
        if (wust_set_nonblocking(fd, 1) != 0) {
            wust_close_socket(fd);
            fd = WUST_INVALID_SOCKET;
            continue;
        }
        rc = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc != 0) {
            int err = wust_sock_errno();
            if (!WUST_ERR_INPROGRESS(err)) {
                wust_close_socket(fd);
                fd = WUST_INVALID_SOCKET;
                continue;
            }
            {
                fd_set         wfds;
                struct timeval tv;
                int            soerr = 0;
                socklen_t      slen = (socklen_t)sizeof(soerr);

                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                tv.tv_sec  = timeout_sec;
                tv.tv_usec = 0;
                rc = select(WUST_SELECT_NFDS(fd), NULL, &wfds, NULL, &tv);
                if (rc <= 0) {
                    wust_close_socket(fd);
                    fd = WUST_INVALID_SOCKET;
                    continue;
                }
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, WUST_SOCKOPT_CAST_MUT(&soerr), &slen) != 0
                    || soerr != 0) {
                    wust_close_socket(fd);
                    fd = WUST_INVALID_SOCKET;
                    continue;
                }
            }
        }
        if (wust_set_nonblocking(fd, 0) != 0) {
            wust_close_socket(fd);
            fd = WUST_INVALID_SOCKET;
            continue;
        }
        break;                                  /* 连接成功 */
    }
    freeaddrinfo(res);

    if (fd == WUST_INVALID_SOCKET) {
        snprintf(errbuf, errlen, "无法连接到 %s 端口 %s（连接超时或被拒绝）", host, port);
    }
    return fd;
}

static int wust_send_all(WUST_SOCKET fd, const char *buf, size_t len)
{
    size_t sent = 0u;
    int    retry = 0;

    while (sent < len) {
        size_t chunk = len - sent;
        int    n;

        if (chunk > 65536u) {
            chunk = 65536u;
        }
        n = (int)send(fd, buf + sent, (int)chunk, 0);
        if (n > 0) {
            sent += (size_t)n;
            retry = 0;
            continue;
        }
        {
            int err = wust_sock_errno();
            if (n < 0 && WUST_ERR_INTR(err)) {
                continue;
            }
            if (n < 0 && (WUST_ERR_WOULDBLOCK(err) || WUST_ERR_TIMEOUT(err)) && retry < 3) {
                retry++;
                continue;
            }
        }
        return -1;
    }
    return 0;
}

/* 读到连接关闭为止（请求里带了 Connection: close） */
static int wust_recv_all(WUST_SOCKET fd, strbuf *out)
{
    char tmp[4096];

    for (;;) {
        int n = (int)recv(fd, tmp, (int)sizeof(tmp), 0);
        if (n > 0) {
            if (out->len + (size_t)n > MAX_RESPONSE_BYTES) {
                return 0;                        /* 响应过大，直接截断 */
            }
            if (sb_addn(out, tmp, (size_t)n) != 0) {
                return -1;
            }
            continue;
        }
        if (n == 0) {
            return 0;                            /* 服务端关闭连接 */
        }
        {
            int err = wust_sock_errno();
            if (WUST_ERR_INTR(err)) {
                continue;
            }
            if (WUST_ERR_WOULDBLOCK(err) || WUST_ERR_TIMEOUT(err)) {
                /* 读超时：已经拿到数据就认为响应结束 */
                return (out->len > 0u) ? 0 : -1;
            }
        }
        return -1;
    }
}

/* 在响应头里查找某个字段（大小写不敏感） */
static int header_find(const char *headers, const char *name, char *out, size_t outlen)
{
    size_t      nlen = strlen(name);
    const char *p = headers;

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t      linelen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        if (linelen >= nlen + 1u && ci_equal_n(p, name, nlen) && p[nlen] == ':') {
            const char *v = p + nlen + 1u;
            size_t      vlen = linelen - nlen - 1u;
            while (vlen > 0u && (*v == ' ' || *v == '\t')) {
                v++;
                vlen--;
            }
            while (vlen > 0u && (v[vlen - 1u] == '\r' || v[vlen - 1u] == ' '
                                 || v[vlen - 1u] == '\t')) {
                vlen--;
            }
            if (vlen > outlen - 1u) {
                vlen = outlen - 1u;
            }
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 0;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return -1;
}

/*
 * 发起一次 HTTP 请求。
 * 返回 0 表示收到了 HTTP 响应（正文在 resp->body 里），-1 表示传输层失败。
 */
static int http_request(const char *method, const char *url,
                        const char *content_type, const char *body,
                        int timeout_sec, http_response *resp)
{
    url_parts   u;
    WUST_SOCKET fd;
    strbuf      req;
    strbuf      raw;
    char        errbuf[512];
    int         rc = -1;
    size_t      sep;
    size_t      hdrlen;

    http_response_init(resp);

    if (url_parse(url, &u) != 0) {
        copy_str(resp->error, sizeof(resp->error), "不支持的请求地址（只支持 http:// 开头）");
        return -1;
    }

    fd = wust_tcp_connect(u.host, u.port, timeout_sec, errbuf, sizeof(errbuf));
    if (fd == WUST_INVALID_SOCKET) {
        copy_str(resp->error, sizeof(resp->error), errbuf);
        return -1;
    }
    wust_set_socket_timeout(fd, timeout_sec);

    sb_init(&req);
    sb_init(&raw);

    if (sb_addf(&req, "%s %s HTTP/1.1\r\n", method, u.path) != 0) {
        goto out_of_memory;
    }
    if (strcmp(u.port, "80") == 0) {
        if (sb_addf(&req, "Host: %s\r\n", u.host) != 0) {
            goto out_of_memory;
        }
    } else {
        if (sb_addf(&req, "Host: %s:%s\r\n", u.host, u.port) != 0) {
            goto out_of_memory;
        }
    }
    if (sb_add(&req, "User-Agent: " APP_NAME "/" APP_VERSION "\r\n") != 0
        || sb_add(&req, "Accept: */*\r\n") != 0
        || sb_add(&req, "Connection: close\r\n") != 0) {
        goto out_of_memory;
    }
    if (body != NULL) {
        if (sb_addf(&req, "Content-Type: %s\r\n",
                    (content_type != NULL) ? content_type
                                           : "application/x-www-form-urlencoded;charset=UTF-8") != 0) {
            goto out_of_memory;
        }
        if (sb_addf(&req, "Content-Length: %lu\r\n", (unsigned long)strlen(body)) != 0) {
            goto out_of_memory;
        }
    }
    if (sb_add(&req, "\r\n") != 0) {
        goto out_of_memory;
    }
    if (body != NULL && sb_add(&req, body) != 0) {
        goto out_of_memory;
    }

    if (wust_send_all(fd, req.data, req.len) != 0) {
        copy_str(resp->error, sizeof(resp->error), "发送 HTTP 请求失败");
        goto cleanup;
    }
    if (wust_recv_all(fd, &raw) != 0 || raw.len == 0u) {
        copy_str(resp->error, sizeof(resp->error),
                 (raw.len == 0u) ? "服务器没有返回任何数据（可能被网络拦截或服务器无响应）"
                                 : "读取 HTTP 响应失败");
        goto cleanup;
    }

    /* --- 解析状态行 --- */
    if (raw.len >= 5u && strncmp(raw.data, "HTTP/", 5u) == 0) {
        const char *sp = strchr(raw.data, ' ');
        if (sp != NULL) {
            resp->status = (int)strtol(sp + 1, NULL, 10);
        }
    } else {
        copy_str(resp->error, sizeof(resp->error), "服务器返回的内容不是合法的 HTTP 响应");
        goto cleanup;
    }

    /* --- 分离响应头与响应正文 --- */
    sep = 0u;
    {
        const char *p = strstr(raw.data, "\r\n\r\n");
        if (p != NULL) {
            sep = (size_t)(p - raw.data) + 4u;
        } else {
            p = strstr(raw.data, "\n\n");
            if (p != NULL) {
                sep = (size_t)(p - raw.data) + 2u;
            }
        }
    }
    if (sep == 0u) {
        hdrlen = raw.len;
    } else {
        hdrlen = sep;
    }

    {
        char saved = raw.data[hdrlen];
        raw.data[hdrlen] = '\0';
        (void)header_find(raw.data, "Location", resp->location, sizeof(resp->location));
        raw.data[hdrlen] = saved;
    }

    if (sep > 0u && raw.len > sep) {
        size_t blen = raw.len - sep;
        resp->body = (char *)malloc(blen + 1u);
        if (resp->body == NULL) {
            goto out_of_memory;
        }
        memcpy(resp->body, raw.data + sep, blen);
        resp->body[blen] = '\0';
        resp->body_len = blen;
    }

    resp->ok = 1;
    rc = 0;

cleanup:
    wust_close_socket(fd);
    sb_free(&req);
    sb_free(&raw);
    return rc;

out_of_memory:
    copy_str(resp->error, sizeof(resp->error), "内存不足");
    wust_close_socket(fd);
    sb_free(&req);
    sb_free(&raw);
    return -1;
}

/* ------------------------------------------------------------------ */
/* JSON 取值（够用即可，不引入第三方库）                               */
/* ------------------------------------------------------------------ */
static const char *json_find_value(const char *json, const char *key)
{
    size_t      klen = strlen(key);
    const char *p = json;

    while ((p = strchr(p, '"')) != NULL) {
        p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
            const char *q = p + klen + 1u;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
                q++;
            }
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
                    q++;
                }
                return q;
            }
        }
    }
    return NULL;
}

static int json_get_int(const char *json, const char *key, long *out)
{
    const char *v;
    char       *end = NULL;
    long        n;

    v = json_find_value(json, key);
    if (v == NULL) {
        return -1;
    }
    if (*v == '"') {                       /* 兼容 "code": "0" 这种写法 */
        v++;
    }
    n = strtol(v, &end, 10);
    if (end == v) {
        return -1;
    }
    *out = n;
    return 0;
}

static int json_get_str(const char *json, const char *key, char *out, size_t outlen)
{
    const char *v;
    size_t      o = 0u;

    if (outlen == 0u) {
        return -1;
    }
    out[0] = '\0';
    v = json_find_value(json, key);
    if (v == NULL) {
        return -1;
    }
    if (*v == '"') {
        v++;
        while (*v != '\0' && *v != '"') {
            if (*v == '\\' && v[1] != '\0') {
                v++;
            }
            if (o + 1u < outlen) {
                out[o++] = *v;
            }
            v++;
        }
        out[o] = '\0';
        return 0;
    }
    while (*v != '\0' && *v != ',' && *v != '}' && *v != '\r' && *v != '\n') {
        if (o + 1u < outlen) {
            out[o++] = *v;
        }
        v++;
    }
    out[o] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* 配置文件                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char username[128];
    char password[256];
} credentials;

/* 拼路径：dir + 分隔符 + name */
static int join_path(const char *dir, const char *name, char *out, size_t outlen)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);

    if (dlen == 0u || dlen + 1u + nlen >= outlen) {
        return -1;
    }
    memcpy(out, dir, dlen);
    out[dlen] = '/';
#ifdef _WIN32
    out[dlen] = '\\';
#endif
    memcpy(out + dlen + 1u, name, nlen + 1u);
    return 0;
}

/* 程序自身所在目录 */
static void get_exe_dir(char *out, size_t outlen)
{
    out[0] = '\0';
#ifdef _WIN32
    {
        DWORD n = GetModuleFileNameA(NULL, out, (DWORD)outlen);
        if (n > 0u && (size_t)n < outlen) {
            char *slash = strrchr(out, '\\');
            if (slash == NULL) {
                slash = strrchr(out, '/');
            }
            if (slash != NULL) {
                *slash = '\0';
            } else {
                copy_str(out, outlen, ".");
            }
        } else {
            copy_str(out, outlen, ".");
        }
    }
#else
    {
        ssize_t n = readlink("/proc/self/exe", out, outlen - 1u);
        if (n > 0) {
            char *slash;
            out[(size_t)n] = '\0';
            slash = strrchr(out, '/');
            if (slash != NULL) {
                *slash = '\0';
            } else {
                copy_str(out, outlen, ".");
            }
        } else {
            copy_str(out, outlen, ".");
        }
    }
#endif
}

/*
 * 每个用户自己的配置目录。
 * Windows: %APPDATA%\wust-auth
 * Linux  : $XDG_CONFIG_HOME/wust-auth 或 ~/.config/wust-auth
 * 用于程序所在目录不可写时（例如程序被放在 C:\ 根目录或 Program Files）。
 */
static void get_user_config_dir(char *out, size_t outlen)
{
    const char *base = NULL;

    out[0] = '\0';
#ifdef _WIN32
    base = getenv("APPDATA");
    if (base == NULL || *base == '\0') {
        base = getenv("USERPROFILE");
    }
    if (base == NULL || *base == '\0') {
        base = getenv("TEMP");
    }
#else
    base = getenv("XDG_CONFIG_HOME");
    if (base == NULL || *base == '\0') {
        base = getenv("HOME");
        if (base != NULL && *base != '\0') {
            if (join_path(base, ".config", out, outlen) != 0) {
                out[0] = '\0';
                return;
            }
            if (join_path(out, "wust-auth", out, outlen) != 0) {
                out[0] = '\0';
            }
            return;
        }
    }
#endif
    if (base == NULL || *base == '\0') {
        return;
    }
    if (join_path(base, "wust-auth", out, outlen) != 0) {
        out[0] = '\0';
    }
}

/* 逐级创建目录（已存在不算失败） */
static int ensure_dir(const char *path)
{
    char   tmp[600];
    size_t i;

    if (path[0] == '\0' || strlen(path) >= sizeof(tmp)) {
        return -1;
    }
    copy_str(tmp, sizeof(tmp), path);
    for (i = 1u; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
#ifdef _WIN32
            (void)_mkdir(tmp);
#else
            (void)mkdir(tmp, (mode_t)0700);
#endif
            tmp[i] = saved;
        }
    }
#ifdef _WIN32
    if (_mkdir(tmp) == 0) {
        return 0;
    }
#else
    if (mkdir(tmp, (mode_t)0700) == 0) {
        return 0;
    }
#endif
    /* 已经存在也算成功；用“能否打开目录里的文件”来判断 */
    {
        char probe[700];
        FILE *f;
        if (join_path(tmp, ".wust_dir_probe.tmp", probe, sizeof(probe)) != 0) {
            return -1;
        }
        f = fopen(probe, "wb");
        if (f == NULL) {
            return -1;
        }
        fclose(f);
        (void)remove(probe);
    }
    return 0;
}

/* 目录是否真的存在（区分“不存在”和“存在但不可写”） */
static int path_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
#ifdef _WIN32
    return (st.st_mode & S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode) ? 1 : 0;
#endif
}

/* 目录是否可写（真的建一个临时文件再删掉，不留痕迹） */
static int dir_writable(const char *dir)
{
    char  probe[700];
    FILE *f;

    if (dir[0] == '\0' || join_path(dir, ".wust_write_probe.tmp", probe, sizeof(probe)) != 0) {
        return 0;
    }
    f = fopen(probe, "wb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    (void)remove(probe);
    return 1;
}

/*
 * 配置查找顺序：
 *   1. 环境变量 WUST_CONFIG（显式指定，最高优先级）
 *   2. 当前目录下已存在的 wust_config.ini
 *   3. 程序所在目录下已存在的 wust_config.ini
 *   4. 用户配置目录下已存在的 wust_config.ini
 *   都不存在时，选一个“接下来能写进去”的位置：
 *   程序所在目录可写就用它，否则用用户配置目录（C:\ 根目录、Program Files
 *   这类地方普通用户没有写权限，必须退到用户目录）。
 */
static void resolve_config_path(char *out, size_t outlen)
{
    const char *env = getenv("WUST_CONFIG");
    char        exedir[512];
    char        userdir[512];
    char        cand[600];

    if (env != NULL && *env != '\0') {
        copy_str(out, outlen, env);
        return;
    }
    if (file_exists(CONFIG_FILE_NAME)) {
        copy_str(out, outlen, CONFIG_FILE_NAME);
        return;
    }

    get_exe_dir(exedir, sizeof(exedir));
    get_user_config_dir(userdir, sizeof(userdir));

    if (join_path(exedir, CONFIG_FILE_NAME, cand, sizeof(cand)) == 0
        && file_exists(cand)) {
        copy_str(out, outlen, cand);
        return;
    }
    if (userdir[0] != '\0'
        && join_path(userdir, CONFIG_FILE_NAME, cand, sizeof(cand)) == 0
        && file_exists(cand)) {
        copy_str(out, outlen, cand);
        return;
    }

    if (dir_writable(exedir)
        && join_path(exedir, CONFIG_FILE_NAME, cand, sizeof(cand)) == 0) {
        copy_str(out, outlen, cand);
        return;
    }
    if (userdir[0] != '\0' && ensure_dir(userdir) == 0
        && join_path(userdir, CONFIG_FILE_NAME, cand, sizeof(cand)) == 0) {
        copy_str(out, outlen, cand);
        return;
    }
    copy_str(out, outlen, CONFIG_FILE_NAME);
}

/* 把整个文件读进内存：便于处理 BOM、UTF-16 和超长行 */
static int read_whole_file(const char *path, strbuf *out, char *why, size_t whylen)
{
    FILE  *f;
    char   chunk[4096];
    size_t n;

    errno = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        if (errno == ENOENT) {
            copy_str(why, whylen, CONFIG_WHY_NOFILE);
        } else {
            snprintf(why, whylen,
                     "无法打开（错误码 %d：可能是没有读取权限、文件被占用，或该路径是个目录）",
                     errno);
        }
        return -1;
    }
    while ((n = fread(chunk, 1u, sizeof(chunk), f)) > 0u) {
        if (sb_addn(out, chunk, n) != 0) {
            fclose(f);
            copy_str(why, whylen, "内存不足");
            return -1;
        }
    }
    if (ferror(f)) {
        fclose(f);
        copy_str(why, whylen, "读取文件时发生错误");
        return -1;
    }
    fclose(f);
    return 0;
}

/* 按 UTF-8 追加一个码点 */
static int append_utf8(strbuf *out, unsigned int cp)
{
    if (cp < 0x80u) {
        return sb_addc(out, (char)cp);
    }
    if (cp < 0x800u) {
        if (sb_addc(out, (char)(0xC0u | (cp >> 6))) != 0) {
            return -1;
        }
        return sb_addc(out, (char)(0x80u | (cp & 0x3Fu)));
    }
    if (cp < 0x10000u) {
        if (sb_addc(out, (char)(0xE0u | (cp >> 12))) != 0) {
            return -1;
        }
        if (sb_addc(out, (char)(0x80u | ((cp >> 6) & 0x3Fu))) != 0) {
            return -1;
        }
        return sb_addc(out, (char)(0x80u | (cp & 0x3Fu)));
    }
    if (sb_addc(out, (char)(0xF0u | (cp >> 18))) != 0) {
        return -1;
    }
    if (sb_addc(out, (char)(0x80u | ((cp >> 12) & 0x3Fu))) != 0) {
        return -1;
    }
    if (sb_addc(out, (char)(0x80u | ((cp >> 6) & 0x3Fu))) != 0) {
        return -1;
    }
    return sb_addc(out, (char)(0x80u | (cp & 0x3Fu)));
}

/*
 * 统一文本编码：
 *   - UTF-8 带 BOM（记事本“UTF-8”）→ 去掉 BOM；
 *   - UTF-16 LE/BE 带 BOM（记事本“Unicode”）→ 转成 UTF-8；
 *   - 含 0 字节的其它情况 → 报错，明确告诉用户怎么另存。
 */
static int normalize_text(strbuf *b, char *why, size_t whylen)
{
    const unsigned char *p = (const unsigned char *)b->data;

    if (b->len >= 2u
        && ((p[0] == 0xFFu && p[1] == 0xFEu) || (p[0] == 0xFEu && p[1] == 0xFFu))) {
        int    be = (p[0] == 0xFEu) ? 1 : 0;
        strbuf out;
        size_t i;

        sb_init(&out);
        for (i = 2u; i + 1u < b->len; i += 2u) {
            unsigned int u = be ? (((unsigned int)p[i] << 8) | (unsigned int)p[i + 1u])
                                : (((unsigned int)p[i + 1u] << 8) | (unsigned int)p[i]);
            if (u >= 0xD800u && u <= 0xDBFFu && (i + 3u) < b->len) {
                unsigned int lo = be ? (((unsigned int)p[i + 2u] << 8) | (unsigned int)p[i + 3u])
                                     : (((unsigned int)p[i + 3u] << 8) | (unsigned int)p[i + 2u]);
                if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                    u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 2u;
                }
            }
            if (append_utf8(&out, u) != 0) {
                sb_free(&out);
                copy_str(why, whylen, "内存不足");
                return -1;
            }
        }
        sb_free(b);
        *b = out;
        return 0;
    }

    if (b->len >= 3u && p[0] == 0xEFu && p[1] == 0xBBu && p[2] == 0xBFu) {
        memmove(b->data, b->data + 3u, b->len - 2u);   /* 连同结尾的 '\0' 一起前移 */
        b->len -= 3u;
    }

    if (memchr(b->data, '\0', b->len) != NULL) {
        copy_str(why, whylen,
                 "文件内容里含 0 字节，像是“无 BOM 的 UTF-16”或二进制文件，请用记事本另存为 UTF-8");
        return -1;
    }
    return 0;
}

/*
 * 读取配置。返回 0 表示学号密码都拿到了；
 * 失败时把“到底为什么读不到”写进 why，便于用户自己排查。
 */
static int config_load(const char *path, credentials *c, char *why, size_t whylen)
{
    strbuf raw;
    char  *p;

    c->username[0] = '\0';
    c->password[0] = '\0';
    if (why != NULL && whylen > 0u) {
        why[0] = '\0';
    }

    sb_init(&raw);
    if (read_whole_file(path, &raw, why, whylen) != 0) {
        sb_free(&raw);
        return -1;
    }
    if (raw.len == 0u) {
        sb_free(&raw);
        copy_str(why, whylen, "文件是空的");
        return -1;
    }
    if (normalize_text(&raw, why, whylen) != 0) {
        sb_free(&raw);
        return -1;
    }

    p = raw.data;
    while (p != NULL && *p != '\0') {
        char *nl = strchr(p, '\n');
        char *eq;

        if (nl != NULL) {
            *nl = '\0';
        }
        trim(p);
        if (p[0] != '\0' && p[0] != '#' && p[0] != ';' && p[0] != '[') {
            eq = strchr(p, '=');
            if (eq != NULL) {
                char *value;
                *eq = '\0';
                trim(p);
                value = eq + 1;
                trim(value);
                if (strcmp(p, "username") == 0 || strcmp(p, "user") == 0) {
                    copy_str(c->username, sizeof(c->username), value);
                } else if (strcmp(p, "password") == 0 || strcmp(p, "pass") == 0) {
                    copy_str(c->password, sizeof(c->password), value);
                }
            }
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    sb_free(&raw);

    if (c->username[0] == '\0' && c->password[0] == '\0') {
        copy_str(why, whylen, "文件里既没有 username= 也没有 password= 这两行（键名是否写错？）");
        return -1;
    }
    if (c->username[0] == '\0') {
        copy_str(why, whylen, "缺少 username=（学号）一行，或者等号后面是空的");
        return -1;
    }
    if (c->password[0] == '\0') {
        copy_str(why, whylen, "缺少 password=（密码）一行，或者等号后面是空的");
        return -1;
    }
    return 0;
}

/* 当前工作目录（诊断用） */
static void get_cwd(char *out, size_t outlen)
{
    if (outlen == 0u) {
        return;
    }
    out[0] = '\0';
#ifdef _WIN32
    if (_getcwd(out, (int)outlen) == NULL) {
        out[0] = '\0';
    }
#else
    if (getcwd(out, outlen) == NULL) {
        out[0] = '\0';
    }
#endif
}

static int config_save(const char *path, const credentials *c)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    fputs("# 武汉科技大学校园网自动认证工具 —— 个人配置\r\n", f);
    fputs("# 这里保存着你的学号和密码，请不要把这个文件发给别人。\r\n", f);
    fputs("# 需要修改账号密码时，重新运行程序选择菜单 2 即可。\r\n", f);
    fprintf(f, "username=%s\r\n", c->username);
    fprintf(f, "password=%s\r\n", c->password);
    if (fclose(f) != 0) {
        return -1;
    }
#ifndef _WIN32
    (void)chmod(path, (mode_t)0600);          /* 只有自己可读写 */
#endif
    return 0;
}

/* 统一的保存结果提示 */
static void report_save_result(int ok, const char *used, int fell_back, const char *primary)
{
    if (ok == 0) {
        if (fell_back) {
            printf("程序所在目录不可写，配置已改存到：%s\n", used);
            printf("（以后运行会自动到这个位置读取；想换位置可设置环境变量 WUST_CONFIG）\n");
        } else {
            printf("配置已保存到：%s\n", used);
        }
        return;
    }
    printf("保存配置失败：所有候选位置都写不进去。\n");
    printf("  已尝试：%s\n", primary);
    printf("  以及用户配置目录和当前目录。\n");
    printf("建议把程序放到一个可写目录（例如 D:\\wust-auth\\ 或 Linux 的 ~/wust-auth/）后重试，\n");
    printf("或者设置环境变量 WUST_CONFIG 指到一个可写位置。\n");
}

/* ------------------------------------------------------------------ */
/* 交互：录入 / 修改账号密码                                           */
/* ------------------------------------------------------------------ */
static int ask_credentials(credentials *c, int first_time)
{
    char line[512];

    if (first_time) {
        printf("\n未检测到可用的账号配置，请先录入你的学号和密码。\n");
        printf("（账号只保存在本机的 %s 中，程序不会上传到任何第三方）\n", CONFIG_FILE_NAME);
    } else {
        printf("\n—— 修改账号和密码 ——\n");
        printf("当前学号：%s\n", (c->username[0] != '\0') ? c->username : "（未设置）");
        printf("提示：直接回车表示该项保持不变。\n");
    }

    for (;;) {
        printf("%s", first_time ? "请输入学号：" : "请输入新学号：");
        if (read_line(NULL, line, sizeof(line), 0) != 0) {
            return -1;
        }
        trim(line);
        if (line[0] != '\0') {
            copy_str(c->username, sizeof(c->username), line);
        }
        if (c->username[0] != '\0') {
            break;
        }
        printf("学号不能为空，请重新输入。\n");
    }

    for (;;) {
        printf("请输入密码：");
        if (read_line(NULL, line, sizeof(line), 1) != 0) {
            return -1;
        }
        if (line[0] == '\0') {
            if (!first_time && c->password[0] != '\0') {
                printf("（密码保持不变）\n");
                break;
            }
            printf("密码不能为空，请重新输入。\n");
            continue;
        }
        copy_str(c->password, sizeof(c->password), line);
        break;
    }
    return 0;
}

/*
 * 保存配置：先写主路径；写不进去（例如程序放在 C:\ 根目录、Program Files
 * 或只读盘）就退到“当前目录”，再退到用户配置目录（会自动创建）。
 * fell_back 回传是否用了备用位置。
 */
static int save_credentials(const char *primary, const credentials *c,
                            char *used, size_t usedlen, int *fell_back)
{
    char userdir[512];
    char cand[600];

    if (fell_back != NULL) {
        *fell_back = 0;
    }
    if (config_save(primary, c) == 0) {
        copy_str(used, usedlen, primary);
        return 0;
    }

    get_user_config_dir(userdir, sizeof(userdir));
    if (userdir[0] != '\0' && ensure_dir(userdir) == 0
        && join_path(userdir, CONFIG_FILE_NAME, cand, sizeof(cand)) == 0
        && config_save(cand, c) == 0) {
        copy_str(used, usedlen, cand);
        if (fell_back != NULL) {
            *fell_back = 1;
        }
        return 0;
    }

    if (strcmp(primary, CONFIG_FILE_NAME) != 0 && config_save(CONFIG_FILE_NAME, c) == 0) {
        copy_str(used, usedlen, CONFIG_FILE_NAME);
        if (fell_back != NULL) {
            *fell_back = 1;
        }
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* 联网探测 / 认证                                                     */
/* ------------------------------------------------------------------ */
static const char *login_url(void)
{
    const char *env = getenv("WUST_LOGIN_URL");
    return (env != NULL && *env != '\0') ? env : DEFAULT_LOGIN_URL;
}

static const char *probe_url(void)
{
    const char *env = getenv("WUST_PROBE_URL");
    return (env != NULL && *env != '\0') ? env : DEFAULT_PROBE_URL;
}

static void lower_str(char *s)
{
    for (; *s != '\0'; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

/* 形如 1.2.3.4 的 IPv4 字面量 */
static int looks_like_ipv4(const char *s)
{
    int         dots = 0;
    int         digits = 0;
    const char *p;

    for (p = s; *p != '\0'; p++) {
        if (*p == '.') {
            if (digits == 0 || digits > 3) {
                return 0;
            }
            dots++;
            digits = 0;
        } else if (*p >= '0' && *p <= '9') {
            digits++;
        } else {
            return 0;
        }
    }
    return (dots == 3 && digits > 0 && digits <= 3) ? 1 : 0;
}

/* 取域名最后两段（近似“注册域”）：www.bing.com -> bing.com */
static const char *last_two_labels(const char *host)
{
    const char *p = strrchr(host, '.');

    if (p == NULL || p == host) {
        return NULL;
    }
    while (p > host && *(p - 1) != '.') {
        p--;
    }
    return p;
}

/*
 * 两个主机名是否属于同一个站点。
 * 依次判断：完全相同 → 互为子域 → 同一注册域（如 www.bing.com 与 cn.bing.com）。
 * 同一注册域这条是为了容忍真实的站点内跳转（很多站点会把 www 跳到别的国家域名）。
 * 对 IPv4 字面量不做“注册域”比较，避免 1.2.3.4 与 5.6.3.4 被误判成同一站点。
 */
static int same_site(const char *a, const char *b)
{
    char   la[256];
    char   lb[256];
    size_t alen;
    size_t blen;

    copy_str(la, sizeof(la), a);
    copy_str(lb, sizeof(lb), b);
    lower_str(la);
    lower_str(lb);
    if (la[0] == '\0' || lb[0] == '\0') {
        return 0;
    }
    if (strcmp(la, lb) == 0) {
        return 1;
    }
    alen = strlen(la);
    blen = strlen(lb);
    if (blen > alen && lb[blen - alen - 1u] == '.'
        && strcmp(lb + (blen - alen), la) == 0) {
        return 1;
    }
    if (alen > blen && la[alen - blen - 1u] == '.'
        && strcmp(la + (alen - blen), lb) == 0) {
        return 1;
    }
    if (!looks_like_ipv4(la) && !looks_like_ipv4(lb)) {
        const char *ra = last_two_labels(la);
        const char *rb = last_two_labels(lb);
        if (ra != NULL && rb != NULL && strcmp(ra, rb) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 取 URL 里的主机名。0=取到了，1=相对地址（同站点），-1=看不懂 */
static int location_host(const char *location, char *out, size_t outlen)
{
    url_parts u;

    if (outlen > 0u) {
        out[0] = '\0';
    }
    if (location[0] == '\0') {
        return -1;
    }
    if (strncmp(location, "http://", 7u) == 0) {
        if (url_parse(location, &u) != 0) {
            return -1;
        }
        copy_str(out, outlen, u.host);
        return 0;
    }
    if (strncmp(location, "https://", 8u) == 0) {
        const char *p = location + 8;
        const char *slash = strchr(p, '/');
        const char *colon;
        size_t      n = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
        colon = (const char *)memchr(p, ':', n);
        if (colon != NULL) {
            n = (size_t)(colon - p);
        }
        if (n == 0u || n >= outlen) {
            return -1;
        }
        memcpy(out, p, n);
        out[n] = '\0';
        return 0;
    }
    return 1;                                  /* 相对地址：还是同一个站点 */
}

/* hay 的前 limit 个字节里是否出现 needle */
static int contains_within(const char *hay, size_t limit, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;

    if (nl == 0u) {
        return 0;
    }
    for (i = 0u; i < limit; i++) {
        if (hay[i] == '\0') {
            return 0;
        }
        if (hay[i] == needle[0] && strncmp(hay + i, needle, nl) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 是否出现 “url=<我们刚才请求的地址>” 这种回填（门户的典型特征） */
static int echoed_as_param(const char *text, const char *needle)
{
    const char *p = text;

    if (needle == NULL || *needle == '\0') {
        return 0;
    }
    while ((p = strstr(p, "url=")) != NULL) {
        p += 4;
        if (contains_within(p, 160u, needle)) {
            return 1;
        }
    }
    return 0;
}

/*
 * 是不是校园网门户/劫持页面？
 * 命中任意一条就认为“没有真正通向外网”，宁可多认证一次，也不能误判成已在线。
 */
static int response_is_portal(const http_response *r, const char *probe_host)
{
    static const char *const marks[] = {
        "srun_portal", "ac_id=", "wlanuserip", "wlanacname",
        "nas_id=", "user_ip=", "eportal", "login?url=", "/portal/"
    };
    const char *body = (r->body != NULL) ? r->body : "";
    const char *loc  = r->location;
    size_t      i;

    for (i = 0u; i < sizeof(marks) / sizeof(marks[0]); i++) {
        if (strstr(body, marks[i]) != NULL || strstr(loc, marks[i]) != NULL) {
            return 1;
        }
    }
    if (echoed_as_param(body, probe_host) || echoed_as_param(loc, probe_host)) {
        return 1;
    }
    return 0;
}

/*
 * 判断探测响应是否代表“真的能上外网”。判定原则：
 *   - 有确凿证据说明“确实到达了目标站点”才算已联网；
 *   - 拿不准时一律算未联网（多认证一次没有副作用，误判成已在线会让人上不了网）；
 *   - 200 响应里，“命中真实站点特征”优先于各种门户指纹 —— 因为门户页面上
 *     也可能出现 baidu 字样/链接，而真实首页里同样可能出现 ac_id= 这类广告参数。
 *
 * 判定顺序：
 *   1. 3xx：跳到同一个站点（如 http 跳 https）→ 通；跳到别的站点 → 门户劫持；
 *   2. 204：通；
 *   3. 200：先看有没有“真实站点特征”（百度是 <!--STATUS OK--> / 百度一下；
 *      其它地址用 WUST_PROBE_MARKER 指定）；命中 → 通；
 *      没命中再看门户指纹 → 报门户；都不像 → 保守判为未联网。
 */
static int response_means_online(const http_response *r, const char *probe_host,
                                 char *why, size_t whylen)
{
    const char *body = (r->body != NULL) ? r->body : "";
    const char *marker;
    int         is_baidu = (probe_host[0] != '\0' && strstr(probe_host, "baidu") != NULL);

    /* ---- 3xx：只看跳转目标是不是同一个站点 ---- */
    if (r->status >= 300 && r->status < 400) {
        char lochost[256];
        int  rc = location_host(r->location, lochost, sizeof(lochost));
        if (rc < 0) {
            copy_str(why, whylen, "返回了跳转，但跳转地址无法解析");
            return 0;
        }
        if (rc == 1 || same_site(lochost, probe_host)) {
            copy_str(why, whylen, "跳转到同一站点的其它地址（正常）");
            return 1;
        }
        copy_str(why, whylen, "被跳转到了别的站点（疑似校园网门户劫持）");
        return 0;
    }

    if (r->status == 204) {
        copy_str(why, whylen, "HTTP 204，链路正常");
        return 1;
    }
    if (r->status != 200) {
        copy_str(why, whylen, "HTTP 状态码不是 200/204/3xx");
        return 0;
    }

    /* ---- 200：先找“真实站点特征” ---- */
    marker = getenv("WUST_PROBE_MARKER");
    if (marker != NULL && *marker != '\0') {
        if (strstr(body, marker) != NULL) {
            copy_str(why, whylen, "正文命中 WUST_PROBE_MARKER 指定的特征");
            return 1;
        }
    } else if (is_baidu) {
        if (strstr(body, "<!--STATUS OK-->") != NULL) {
            copy_str(why, whylen, "正文确认是百度首页（STATUS OK 特征）");
            return 1;
        }
        if (strstr(body, "百度一下") != NULL && r->body_len >= 4096u) {
            copy_str(why, whylen, "正文确认是百度首页（标题特征）");
            return 1;
        }
    }

    /* ---- 没有真实站点特征，再看看像不像门户 ---- */
    if (response_is_portal(r, probe_host)) {
        copy_str(why, whylen, "响应里带有校园网门户特征（未认证流量被劫持）");
        return 0;
    }
    if ((marker != NULL && *marker != '\0') || is_baidu) {
        copy_str(why, whylen, "HTTP 200，但正文没有真实站点特征（很可能是门户页面）");
        return 0;
    }
    if (probe_host[0] != '\0' && strstr(body, probe_host) != NULL) {
        copy_str(why, whylen, "正文里出现了探测站点域名");
        return 1;
    }
    copy_str(why, whylen, "HTTP 200，但无法确认响应来自真实站点");
    return 0;
}

static int check_online(int verbose, char *errbuf, size_t errlen)
{
    http_response resp;
    const char   *url = probe_url();
    char          probe_host[256];
    char          why[256];
    url_parts     u;
    int           online;

    if (errbuf != NULL && errlen > 0u) {
        errbuf[0] = '\0';
    }
    why[0] = '\0';
    probe_host[0] = '\0';
    if (url_parse(url, &u) == 0) {
        copy_str(probe_host, sizeof(probe_host), u.host);
    }

    if (http_request("GET", url, NULL, NULL, PROBE_TIMEOUT_SEC, &resp) != 0) {
        if (errbuf != NULL && errlen > 0u) {
            copy_str(errbuf, errlen, resp.error);
        }
        http_response_free(&resp);
        return 0;
    }
    online = response_means_online(&resp, probe_host, why, sizeof(why));
    if (verbose) {
        printf("      [探测] %s -> HTTP %d：%s\n", url, resp.status,
               why[0] != '\0' ? why : (online ? "判定已联网" : "判定未联网"));
        printf("      [探测] 结论：%s\n", online ? "已联网" : "未联网");
    }
    http_response_free(&resp);
    return online;
}

typedef struct {
    int  transport_ok;
    int  has_code;
    long code;
    char auth_msg[320];
    char error[512];
    char raw[1024];
} login_result;

static void do_login_request(const credentials *c, login_result *lr)
{
    strbuf        form;
    strbuf        enc;
    http_response resp;
    const char   *url = login_url();

    memset(lr, 0, sizeof(*lr));
    sb_init(&form);
    sb_init(&enc);

    if (url_encode(c->username, &enc) != 0) {
        copy_str(lr->error, sizeof(lr->error), "内存不足");
        goto cleanup;
    }
    if (sb_add(&form, "username=") != 0 || sb_add(&form, enc.data) != 0) {
        copy_str(lr->error, sizeof(lr->error), "内存不足");
        goto cleanup;
    }
    sb_reset(&enc);
    if (url_encode(c->password, &enc) != 0) {
        copy_str(lr->error, sizeof(lr->error), "内存不足");
        goto cleanup;
    }
    if (sb_add(&form, "&password=") != 0 || sb_add(&form, enc.data) != 0
        || sb_add(&form, "&nasId=1") != 0) {
        copy_str(lr->error, sizeof(lr->error), "内存不足");
        goto cleanup;
    }

    if (http_request("POST", url,
                     "application/x-www-form-urlencoded;charset=UTF-8",
                     form.data, LOGIN_TIMEOUT_SEC, &resp) != 0) {
        copy_str(lr->error, sizeof(lr->error), resp.error);
        http_response_free(&resp);
        goto cleanup;
    }

    lr->transport_ok = 1;
    if (resp.body != NULL) {
        size_t n = resp.body_len;
        if (n > sizeof(lr->raw) - 1u) {
            n = sizeof(lr->raw) - 1u;
        }
        memcpy(lr->raw, resp.body, n);
        lr->raw[n] = '\0';
        if (json_get_int(resp.body, "code", &lr->code) == 0) {
            lr->has_code = 1;
        }
        (void)json_get_str(resp.body, "authMsg", lr->auth_msg, sizeof(lr->auth_msg));
    }
    http_response_free(&resp);

cleanup:
    sb_free(&form);
    sb_free(&enc);
}

/* 登录流程：探测 -> 认证 -> 复检。返回 0 表示最终可以上网。
 * retryable 非空时，回传“失败是否值得重试”（连不上服务器才值得，
 * 账号密码错误重试没有意义）。 */
static int run_login_flow(const credentials *c, int verbose, int *retryable)
{
    login_result lr;
    char         err[512];

    if (retryable != NULL) {
        *retryable = 0;
    }

    printf("\n[1/3] 正在检测网络是否已经连通 ...\n");
    if (check_online(verbose, err, sizeof(err))) {
        printf("      当前网络已经连通（已在线），无需重复认证。\n");
        return EXIT_OK;
    }
    if (err[0] != '\0') {
        printf("      联网探测失败：%s\n", err);
    }
    printf("      当前未联网，开始认证。\n");

    printf("\n[2/3] 正在向认证服务器提交登录请求（学号 %s）...\n", c->username);
    do_login_request(c, &lr);

    if (verbose) {
        printf("      —— 服务器原始响应 ——\n%s\n      ————————————————————\n",
               (lr.raw[0] != '\0') ? lr.raw : "(空)");
    }

    if (!lr.transport_ok) {
        printf("      认证请求失败：%s\n", lr.error);
        printf("\n登录失败：连不上认证服务器，请先确认已连上校园网（WiFi 或网线）。\n");
        if (retryable != NULL) {
            *retryable = 1;
        }
        return EXIT_FAIL;
    }

    if (lr.has_code) {
        if (lr.auth_msg[0] != '\0') {
            printf("      服务器返回：code=%ld，authMsg=%s\n", lr.code, lr.auth_msg);
        } else {
            printf("      服务器返回：code=%ld\n", lr.code);
        }
    } else {
        printf("      服务器返回的内容里没有解析到 code 字段。\n");
    }

    if (!(lr.has_code && lr.code == 0)) {
        printf("\n登录失败：%s\n",
               lr.has_code ? "账号或密码不正确（或账号被锁定）"
                           : "服务器返回异常，请稍后重试");
        return EXIT_FAIL;
    }
    printf("      登录成功！\n");

    printf("\n[3/3] 正在复检网络连通性 ...\n");
    msleep(800u);
    if (check_online(verbose, err, sizeof(err))) {
        printf("      网络已连通，可以正常上网。\n");
        return EXIT_OK;
    }
    /*
     * 认证服务器已经明确回了 code=0，认证本身是成功的。
     * 这里的复检失败通常只是“探测地址不可达”（某些校园网限制百度、
     * 或者探测站点自己抽风），所以提示一下即可，不再当作认证失败。
     */
    printf("      认证服务器已确认登录成功（code=0），但探测不到 %s。\n", probe_url());
    printf("      这通常是探测站点本身不可达，网络应该已经可以用了；\n");
    printf("      如果确实还上不了网，可以用 -v 看探测详情，或换个探测地址：\n");
    printf("        set WUST_PROBE_URL=http://www.bing.com\n");
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 帮助 / 菜单                                                         */
/* ------------------------------------------------------------------ */
static void print_help(const char *prog)
{
    printf("用法：%s [选项]\n\n", prog);
    printf("选项：\n");
    printf("  -a, --auto      静默模式：直接读取配置完成“检测并登录”，不显示菜单\n");
    printf("                  （适合配置成开机自启）\n");
    printf("  -q, --quiet     后台静默：在 -a 的基础上隐藏控制台窗口，并把输出写进\n");
    printf("                  日志文件（wust-auth.log），完全不弹窗、不等输入\n");
    printf("  -v, --verbose   打印探测结果与服务器原始响应，便于排查问题\n");
    printf("  -p, --where     只做配置诊断：显示程序实际使用的配置文件路径和读取结果\n");
    printf("  -t, --tries N   最多尝试认证几次（默认 3；-q 时为 12）\n");
    printf("  -d, --delay N   每次重试间隔多少秒（默认 5；-q 时为 10）\n");
    printf("  -h, --help      显示本帮助\n\n");
    printf("参数写成 '-a'、/a、--auto 都能识别（任务计划里常见的手误）。\n\n");
    printf("不带任何选项运行时会显示交互菜单。\n\n");
    printf("配置文件：默认是程序所在目录下的 %s\n", CONFIG_FILE_NAME);
    printf("  也可以用环境变量 WUST_CONFIG 指定其它路径。\n\n");
    printf("退出码：0=在线或认证成功  1=认证失败  2=配置缺失  3=参数错误\n");
}

static void print_banner(void)
{
    printf("==================================================\n");
    printf("   武汉科技大学 校园网自动认证工具  v%s\n", APP_VERSION);
    printf("==================================================\n");
}

static void print_menu(void)
{
    printf("\n---------------- 主菜单 ----------------\n");
    printf("  1. 自动检测网络并登录（普通登录）\n");
    printf("  2. 修改账号和密码\n");
    printf("  3. 退出程序\n");
    printf("----------------------------------------\n");
}

/* ------------------------------------------------------------------ */
/* 静默/后台运行：日志与隐藏控制台                                     */
/* ------------------------------------------------------------------ */
/*
 * 选择日志文件位置：WUST_LOG 环境变量 > 程序目录（可写时）> 用户配置目录
 */
static void resolve_log_path(char *out, size_t outlen)
{
    const char *env = getenv("WUST_LOG");
    char        exedir[512];
    char        userdir[512];
    char        cand[700];

    if (env != NULL && *env != '\0') {
        copy_str(out, outlen, env);
        return;
    }
    get_exe_dir(exedir, sizeof(exedir));
    get_user_config_dir(userdir, sizeof(userdir));

    if (dir_writable(exedir) && join_path(exedir, LOG_FILE_NAME, cand, sizeof(cand)) == 0) {
        copy_str(out, outlen, cand);
        return;
    }
    if (userdir[0] != '\0' && ensure_dir(userdir) == 0
        && join_path(userdir, LOG_FILE_NAME, cand, sizeof(cand)) == 0) {
        copy_str(out, outlen, cand);
        return;
    }
    copy_str(out, outlen, LOG_FILE_NAME);
}

static void log_timestamp(char *out, size_t outlen)
{
    time_t    now = time(NULL);
    struct tm tmv;
    char      buf[64];

    buf[0] = '\0';
#ifdef _WIN32
    {
        struct tm *p = localtime(&now);
        if (p != NULL) {
            tmv = *p;
            if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) == 0u) {
                buf[0] = '\0';
            }
        }
    }
#else
    if (localtime_r(&now, &tmv) != NULL) {
        if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) == 0u) {
            buf[0] = '\0';
        }
    }
#endif
    copy_str(out, outlen, (buf[0] != '\0') ? buf : "未知时间");
}

/*
 * 进入静默模式：把 stdout/stderr 重定向到日志文件（这样所有原有提示都会
 * 落进日志，便于事后排查），然后在 Windows 上把控制台窗口隐藏掉。
 * 注意：日志文件用追加模式，多次开机自启的记录都会保留。
 */
static int enter_quiet_mode(const char *log_path, const char *exe_dir)
{
    (void)freopen(log_path, "a", stdout);
    (void)freopen(log_path, "a", stderr);

    (void)exe_dir;
#ifdef _WIN32
    {
        /*
         * 只隐藏窗口，不调用 FreeConsole()：
         * 实测 FreeConsole 会让随后写到日志文件的内容丢失（日志变成 0 字节），
         * 而隐藏窗口已经足够达到“不弹窗”的效果。
         * 需要绝对零闪窗的场景用 wust-authw.exe（GUI 子系统，本来就没有控制台）。
         */
        HWND console = GetConsoleWindow();
        if (console != NULL) {
            (void)ShowWindow(console, SW_HIDE);
        }
        printf("控制台：%s\n", (console != NULL) ? "有（已隐藏）" : "无（GUI 子系统，从未创建）");
    }
#else
    printf("控制台：无窗口（后台运行）\n");
#endif
    return 0;
}

static void log_run_header(const char *tag, const char *exe_dir, const char *config_path)
{
    char ts[64];
    log_timestamp(ts, sizeof(ts));
    printf("\n===== %s | %s v%s | %s =====\n", ts, APP_NAME, APP_VERSION, tag);
    printf("程序目录：%s\n", exe_dir);
    printf("配置文件：%s\n", config_path);
    (void)fflush(stdout);
}

static void log_run_footer(int code, const char *meaning)
{
    char ts[64];
    log_timestamp(ts, sizeof(ts));
    printf("===== %s | 结束：%s（退出码 %d）=====\n", ts, meaning, code);
    (void)fflush(stdout);
}

/*
 * 参数规范化：任务计划里常被写成 '-a'（单引号）、"‑a"，或者 /a。
 * 单引号在 Windows 命令行里不是引号字符，会原样传给程序，
 * 所以这里主动去掉两端成对的引号，并把 / 前缀当成 -。
 */
static void normalize_arg(const char *in, char *out, size_t outlen)
{
    size_t n = strlen(in);

    while (n >= 2u
           && ((in[0] == '\'' && in[n - 1u] == '\'')
               || (in[0] == '"' && in[n - 1u] == '"'))) {
        in++;
        n -= 2u;
    }
    if (n >= outlen) {
        n = outlen - 1u;
    }
    memcpy(out, in, n);
    out[n] = '\0';
    if (out[0] == '/') {
        out[0] = '-';
    }
}

/* 参数是否等于其中任意一个写法（-a / --auto / -auto） */
static int arg_match(const char *a, const char *s1, const char *s2, const char *s3)
{
    if (s1 != NULL && strcmp(a, s1) == 0) {
        return 1;
    }
    if (s2 != NULL && strcmp(a, s2) == 0) {
        return 1;
    }
    if (s3 != NULL && strcmp(a, s3) == 0) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char        config_path[512];
    char        config_why[256];
    char        exe_dir[512];
    char        log_path[700];
    credentials cred;
    int         auto_mode = 0;
    int         verbose   = 0;
    int         where_mode = 0;
    int         quiet_mode = 0;
    int         force_help = 0;
    int         config_ok = 0;
    int         tries     = 3;
    int         delay_sec = 5;
    int         i;
    int         exit_code = EXIT_OK;

    config_why[0] = '\0';
#ifdef WUST_FORCE_QUIET
    /* 用 -DWUST_FORCE_QUIET 编译的版本（wust-authw.exe）永远后台静默跑 */
    quiet_mode = 1;
    auto_mode  = 1;
#endif

    for (i = 1; i < argc; i++) {
        char        a[128];
        char        name[128];
        const char *inline_val = NULL;

        normalize_arg(argv[i], a, sizeof(a));
        /* 支持 --tries=10 / -t=10 这种“等号带值”的写法 */
        {
            char *eq = strchr(a, '=');
            if (eq != NULL) {
                *eq = '\0';
                inline_val = eq + 1;
            }
        }
        copy_str(name, sizeof(name), a);

        if (arg_match(name, "-a", "--auto", "-auto")) {
            auto_mode = 1;
        } else if (arg_match(name, "-v", "--verbose", "-verbose")) {
            verbose = 1;
        } else if (arg_match(name, "-p", "--where", "-where")) {
            where_mode = 1;
        } else if (arg_match(name, "-q", "--quiet", "-quiet")) {
            quiet_mode = 1;
        } else if (arg_match(name, "-h", "--help", "-help") || arg_match(name, "-?", NULL, NULL)) {
            force_help = 1;
        } else if (arg_match(name, "-t", "--tries", "-tries")
                   || arg_match(name, "-d", "--delay", "-delay")) {
            int         is_tries = (strchr(name, 't') != NULL);
            const char *val = NULL;

            if (inline_val != NULL && *inline_val != '\0') {
                val = inline_val;
            } else if (i + 1 < argc) {
                val = argv[++i];
            }
            if (val != NULL) {
                int n = (int)strtol(val, NULL, 10);
                if (n > 0 && n <= 600) {
                    if (is_tries) {
                        tries = n;
                    } else {
                        delay_sec = n;
                    }
                }
            }
        } else if (a[0] != '\0') {
            fprintf(stderr, "未知参数：%s（可用的：-a -q -v -p -h -t N -d N）\n\n", argv[i]);
            print_help(argv[0]);
            return EXIT_USAGE;
        }
    }

    if (quiet_mode) {
        auto_mode = 1;                         /* 静默模式不显示菜单、不等待输入 */
        get_exe_dir(exe_dir, sizeof(exe_dir));
        resolve_config_path(config_path, sizeof(config_path));
        resolve_log_path(log_path, sizeof(log_path));
        (void)enter_quiet_mode(log_path, exe_dir);
        log_run_header("静默启动（开机自启）", exe_dir, config_path);
        if (tries == 3 && delay_sec == 5) {    /* 静默模式为开机场景放宽重试 */
            tries = 12;
            delay_sec = 10;
        }
    }
    setup_console();                           /* 有控制台时设置 UTF-8，无则忽略 */

    if (force_help) {
        print_help(argv[0]);
        return EXIT_OK;
    }

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "错误：初始化 WinSock 失败。\n");
            return EXIT_FAIL;
        }
    }
#else
#  ifdef SIGPIPE
    (void)signal(SIGPIPE, SIG_IGN);          /* 写已关闭的连接不要杀掉进程 */
#  endif
#endif

    resolve_config_path(config_path, sizeof(config_path));
    memset(&cred, 0, sizeof(cred));
    {
        int have = (config_load(config_path, &cred, config_why, sizeof(config_why)) == 0);
        if (!have && strcmp(config_path, CONFIG_FILE_NAME) != 0) {
            /* 程序目录里读不到，再试试当前目录 */
            credentials tmp;
            char        why2[256];
            memset(&tmp, 0, sizeof(tmp));
            why2[0] = '\0';
            if (config_load(CONFIG_FILE_NAME, &tmp, why2, sizeof(why2)) == 0) {
                cred = tmp;
                copy_str(config_path, sizeof(config_path), CONFIG_FILE_NAME);
                have = 1;
            } else {
                copy_str(config_why, sizeof(config_why), why2);
            }
        }
        if (!have) {
            memset(&cred, 0, sizeof(cred));
        }
        config_ok = have;
    }

    /* ---------------- 配置诊断模式 ---------------- */
    if (where_mode) {
        char cwd[512];
        char exedir[512];
        char userdir[512];

        get_cwd(cwd, sizeof(cwd));
        get_exe_dir(exedir, sizeof(exedir));
        get_user_config_dir(userdir, sizeof(userdir));

        printf("=== 配置诊断 ===\n");
        printf("当前工作目录  ：%s\n", (cwd[0] != '\0') ? cwd : "(未知)");
        printf("程序所在目录  ：%s（可写：%s）\n", exedir, dir_writable(exedir) ? "是" : "否");
        if (userdir[0] != '\0') {
            if (!path_is_dir(userdir)) {
                printf("用户配置目录  ：%s（尚未创建，首次保存时会自动创建）\n", userdir);
            } else {
                printf("用户配置目录  ：%s（可写：%s）\n", userdir,
                       dir_writable(userdir) ? "是" : "否");
            }
        }
        printf("使用的配置文件：%s\n", config_path);
        if (config_ok) {
            printf("读取结果      ：正常，学号 = %s\n", cred.username);
        } else {
            printf("读取结果      ：失败 —— %s\n", config_why);
            if (file_exists(CONFIG_FILE_NAME)
                && strcmp(config_path, CONFIG_FILE_NAME) != 0) {
                printf("注意：当前目录下还有一个 %s，但没有被使用。\n", CONFIG_FILE_NAME);
            }
            if (getenv("WUST_CONFIG") != NULL) {
                printf("环境变量 WUST_CONFIG=%s\n", getenv("WUST_CONFIG"));
            }
            printf("提示：第一次使用请不带参数运行本程序，按提示录入学号和密码。\n");
        }
#ifdef _WIN32
        WSACleanup();
#endif
        return config_ok ? EXIT_OK : EXIT_CONFIG;
    }

    /* ---------------- 静默模式（-a / -q / wust-authw.exe） ---------------- */
    if (auto_mode) {
        int attempt;
        int have = (cred.username[0] != '\0' && cred.password[0] != '\0');

        if (!have) {
            fprintf(stderr, "错误：读不到可用的账号配置。\n");
            fprintf(stderr, "  程序使用的配置文件：%s\n", config_path);
            fprintf(stderr, "  具体原因：%s\n", config_why);
            fprintf(stderr, "该文件需要包含下面两行，且都不能为空：\n");
            fprintf(stderr, "    username=你的学号\n");
            fprintf(stderr, "    password=你的密码\n");
            fprintf(stderr, "请先不带参数运行本程序，按提示录入学号和密码；\n");
            fprintf(stderr, "也可以用 “%s -p” 查看路径诊断。\n", APP_NAME);
            if (quiet_mode) {
                log_run_footer(EXIT_CONFIG, "配置不可用");
            }
#ifdef _WIN32
            WSACleanup();
#endif
            return EXIT_CONFIG;
        }

        for (attempt = 1; attempt <= tries; attempt++) {
            int retryable = 0;
            exit_code = run_login_flow(&cred, verbose, &retryable);
            if (exit_code == EXIT_OK || !retryable || attempt == tries) {
                break;
            }
            printf("\n第 %d 次尝试未成功，%d 秒后重试 ...\n", attempt, delay_sec);
            (void)fflush(stdout);
            msleep((unsigned)delay_sec * 1000u);
        }
        if (quiet_mode) {
            log_run_footer(exit_code, (exit_code == EXIT_OK) ? "已在线或登录成功" : "登录失败");
        }
#ifdef _WIN32
        WSACleanup();
#endif
        return exit_code;
    }

    /* ---------------- 交互模式 ---------------- */
    print_banner();

    if (cred.username[0] == '\0' || cred.password[0] == '\0') {
        char used[512];
        int  fell_back = 0;

        if (config_why[0] != '\0' && strcmp(config_why, CONFIG_WHY_NOFILE) != 0) {
            printf("\n（配置文件 %s 读取失败：%s）\n", config_path, config_why);
        }
        if (ask_credentials(&cred, 1) != 0) {
#ifdef _WIN32
            WSACleanup();
#endif
            return EXIT_CONFIG;
        }
        if (save_credentials(config_path, &cred, used, sizeof(used), &fell_back) == 0) {
            report_save_result(0, used, fell_back, config_path);
        } else {
            report_save_result(-1, used, 0, config_path);
            printf("本次仍然可以继续登录，但下次运行需要重新输入。\n");
        }
    } else {
        printf("已读取配置文件：%s（学号 %s）\n", config_path, cred.username);
    }

    for (;;) {
        char line[64];
        int  choice;

        print_menu();
        printf("请选择 [1-3]：");
        if (read_line(NULL, line, sizeof(line), 0) != 0) {
            printf("\n再见！\n");
            break;
        }
        trim(line);
        if (line[0] == '\0') {
            continue;
        }
        choice = (int)strtol(line, NULL, 10);

        if (choice == 1) {
            exit_code = run_login_flow(&cred, verbose, NULL);
        } else if (choice == 2) {
            credentials updated = cred;
            char        used[512];
            int         fell_back = 0;

            if (ask_credentials(&updated, 0) != 0) {
                printf("\n再见！\n");
                break;
            }
            if (save_credentials(config_path, &updated, used, sizeof(used), &fell_back) == 0) {
                cred = updated;
                if (fell_back) {
                    printf("程序所在目录不可写，配置已改存到：%s\n", used);
                } else {
                    printf("配置已更新并保存到：%s\n", used);
                }
            } else {
                report_save_result(-1, used, 0, config_path);
                printf("本次修改没有生效。\n");
            }
        } else if (choice == 3 || choice == 0) {
            printf("\n再见！\n");
            break;
        } else {
            printf("输入无效，请输入 1、2 或 3。\n");
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return exit_code;
}
