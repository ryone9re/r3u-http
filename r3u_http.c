#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <linux/limits.h>
#include <netdb.h>
#include <pwd.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "r3u http"
#define SERVER_VERSION "0.0.1"
#define MAX_REQUEST_BODY_LENGTH 4194304
#define MAX_BACKLOG 5
#define DEFAULT_PORT "80"
#define USAGE "Usage: %s [--port=n] [--chroot --user=u --group=g] <docroot>\n"

static int debug_mode = 0;

static struct option longopts[] = {
    {"debug", no_argument, &debug_mode, 1},
    {"chroot", no_argument, NULL, 'c'},
    {"user", required_argument, NULL, 'u'},
    {"group", required_argument, NULL, 'g'},
    {"port", required_argument, NULL, 'p'},
    {"help", no_argument, NULL, 'h'},
    {0, 0, 0, 0},
};

struct HTTPHeaderField
{
    char *name;
    char *value;
    struct HTTPHeaderField *next;
};

struct HTTPRequest
{
    int protocol_minor_version;
    char *method;
    char *path;
    struct HTTPHeaderField *header;
    char *body;
    long length;
};

struct FileInfo
{
    char *path;
    long size;
    int ok;
};

static void setup_environment(char *root, char *user, char *group);
static void become_daemon();
static int listen_socket(char *port);
static void server_main(int server_fd, char *docroot);
static void service(FILE *in, FILE *out, char *docroot);
static struct HTTPRequest *read_request(FILE *in);
static void read_request_line(struct HTTPRequest *req, FILE *in);
static void uppcase(char *str);
static struct HTTPHeaderField *read_header_field(FILE *in);
static long content_length(struct HTTPRequest *req);
static char *lookup_header_field_value(struct HTTPRequest *req, char *name);
static void respond_to(struct HTTPRequest *req, FILE *out, char *docroot);
static void do_file_response(struct HTTPRequest *req, FILE *out, char *docroot);
static void output_common_header_fields(struct HTTPRequest *req, FILE *out, char *status);
static char *guess_content_type(struct FileInfo *info);
static void method_not_allowed(struct HTTPRequest *req, FILE *out);
static void not_implemented(struct HTTPRequest *req, FILE *out);
static void not_found(struct HTTPRequest *req, FILE *out);
static struct FileInfo *get_fileinfo(char *docroot, char *urlpath);
static char *build_fspath(char *docroot, char *urlpath);
static void free_fileinfo(struct FileInfo *info);
static void free_request(struct HTTPRequest *req);
static void log_exit(char *fmt, ...);
static void *xmalloc(size_t sz);
static void install_signal_handlers(void);
static void trap_signal(int sig, __sighandler_t handler, int flags);
static void signal_exit(int sig);
static void noop_handler(int sig);

int main(int argc, char **argv)
{
    int server_fd;
    int opt;
    int do_chroot = 0;
    char *user = NULL;
    char *group = NULL;
    char *port = DEFAULT_PORT;
    struct stat fi;
    char docroot[PATH_MAX];

    while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1)
    {
        switch (opt)
        {
        case 0:
            break;
        case 'c':
            do_chroot = 1;
            break;
        case 'u':
            user = optarg;
            break;
        case 'g':
            group = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'h':
            fprintf(stdout, USAGE, argv[0]);
            exit(0);
        case '?':
            fprintf(stderr, USAGE, argv[0]);
            exit(1);
        }
    }
    if (optind != argc - 1)
    {
        fprintf(stderr, USAGE, argv[0]);
        exit(1);
    }
    if (isalnum(argv[optind][0]))
    {
        char t[PATH_MAX];

        if (getcwd(t, sizeof(t)) == NULL)
        {
            perror("getcwd(3)");
            exit(1);
        }
        strcpy(docroot, t);
        strcpy(docroot + strlen(docroot), "/");
        strcpy(docroot + strlen(docroot), argv[optind]);
    }
    else
        strcpy(docroot, argv[optind]);
    if (lstat(docroot, &fi) < 0)
        log_exit("%s", strerror(errno));
    if (!S_ISDIR(fi.st_mode))
        log_exit("%s is not a directory", docroot);
    install_signal_handlers();
    if (do_chroot)
    {
        setup_environment(docroot, user, group);
        memset(docroot, '\0', sizeof(docroot));
    }
    if (!debug_mode)
    {
        openlog(SERVER_NAME, LOG_PID | LOG_NDELAY, LOG_DAEMON);
        become_daemon();
    }
    server_fd = listen_socket(port);
    server_main(server_fd, docroot);
    exit(0);
}

static void setup_environment(char *root, char *user, char *group)
{
    struct passwd *pw;
    struct group *gr;

    if (!(user && group))
    {
        fprintf(stderr, "use both of --user and --group\n");
        exit(1);
    }
    gr = getgrnam(group);
    if (!gr)
    {
        fprintf(stderr, "no such group: %s\n", group);
        exit(1);
    }
    if (setgid(gr->gr_gid) < 0)
    {
        perror("setgid(2)");
        exit(1);
    }
    if (initgroups(user, gr->gr_gid) < 0)
    {
        perror("initgroups(2)");
        exit(1);
    }
    pw = getpwnam(user);
    if (!pw)
    {
        fprintf(stderr, "no such user: %s\n", user);
        exit(1);
    }
    if (chroot(root) < 0)
    {
        perror("chroot(2)");
        exit(1);
    }
    if (setuid(pw->pw_uid) < 0)
    {
        perror("setuid(2)");
        exit(1);
    }
}

static void become_daemon()
{
    int n;

    if (chdir("/") < 0)
        log_exit("chdir(2) failed: %s", strerror(errno));
    if (freopen("/dev/null", "r", stdin) == NULL)
    {
        perror("freopen(3)");
        exit(1);
    }
    if (freopen("/dev/null", "w", stdout) == NULL)
    {
        perror("freopen(3)");
        exit(1);
    }
    if (freopen("/dev/null", "w", stderr) == NULL)
    {
        perror("freopen(3)");
        exit(1);
    }
    n = fork();
    if (n < 0)
        log_exit("fork(2) failed: %s", strerror(errno));
    if (n != 0)
        _exit(0);
    if (setsid() < 0)
        log_exit("setsid(2) failed: %s", strerror(errno));
}

static int listen_socket(char *port)
{
    struct addrinfo hints, *res, *ai;
    int err;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if ((err = getaddrinfo(NULL, port, &hints, &res)) != 0)
        log_exit("%s", gai_strerror(err));
    for (ai = res; ai; ai = ai->ai_next)
    {
        int sock;

        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
            continue;
        if (bind(sock, ai->ai_addr, ai->ai_addrlen) < 0)
        {
            close(sock);
            continue;
        }
        if (listen(sock, MAX_BACKLOG) < 0)
        {
            close(sock);
            continue;
        }
        freeaddrinfo(res);
        return (sock);
    }
    log_exit("failed to listen socket");
    return (-1);
}

static void server_main(int server_fd, char *docroot)
{
    while (1)
    {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int sock;
        int pid;

        sock = accept(server_fd, (struct sockaddr *)&addr, &addrlen);
        if (sock < 0)
            log_exit("accept(2) failed: %s", strerror(errno));
        pid = fork();
        if (pid < 0)
            exit(3);
        if (pid == 0)
        {
            FILE *inf = fdopen(sock, "r");
            FILE *outf = fdopen(sock, "w");

            service(inf, outf, docroot);
            exit(0);
        }
        close(sock);
    }
}

static void service(FILE *in, FILE *out, char *docroot)
{
    struct HTTPRequest *req;

    req = read_request(in);
    respond_to(req, out, docroot);
    free_request(req);
}

static struct HTTPRequest *read_request(FILE *in)
{
    struct HTTPRequest *req;
    struct HTTPHeaderField *h;

    req = (struct HTTPRequest *)xmalloc(sizeof(struct HTTPRequest));
    read_request_line(req, in);
    req->header = NULL;
    while ((h = read_header_field(in)))
    {
        h->next = req->header;
        req->header = h;
    }
    req->length = content_length(req);
    if (req->length != 0)
    {
        if (req->length > MAX_REQUEST_BODY_LENGTH)
            log_exit("request body too long");
        req->body = xmalloc(req->length);
        if (fread(req->body, req->length, sizeof(char), in) < 1)
            log_exit("failed to read request body");
    }
    else
        req->body = NULL;
    return (req);
}

static void read_request_line(struct HTTPRequest *req, FILE *in)
{
    char buf[BUFSIZ];
    char *p;
    char *path;

    if (!fgets(buf, sizeof(buf), in))
        log_exit("no request line");
    p = strchr(buf, ' ');
    if (!p)
        log_exit("parse error on request line (1): %s", buf);
    *p++ = '\0';
    req->method = (char *)xmalloc(p - buf);
    strcpy(req->method, buf);
    uppcase(req->method);
    path = p;
    p = strchr(path, ' ');
    if (!p)
        log_exit("parse error on request line (2): %s", buf);
    *p++ = '\0';
    req->path = (char *)xmalloc(p - path);
    strcpy(req->path, path);
    if (strncasecmp(p, "HTTP/1.", strlen("HTTP/1.")) != 0)
        log_exit("parse error on request line (3): %s", buf);
    p += strlen("HTTP/1.");
    req->protocol_minor_version = atoi(p);
}

static void uppcase(char *str)
{
    for (size_t i = 0; i < strlen(str); i++)
    {
        if (islower(str[i]))
            str[i] += 'A' - 'a';
    }
}

static struct HTTPHeaderField *read_header_field(FILE *in)
{
    struct HTTPHeaderField *h;
    char buf[BUFSIZ];
    char *p;

    if (!fgets(buf, sizeof(buf), in))
        log_exit("failed to read request header field: %s", strerror(errno));
    if (strcmp(buf, "\n") == 0 || strcmp(buf, "\r\n") == 0)
        return (NULL);

    p = strchr(buf, ':');
    if (!p)
        log_exit("parse error on request header field: %s", buf);
    *p++ = '\0';
    h = (struct HTTPHeaderField *)xmalloc(sizeof(struct HTTPHeaderField));
    h->name = (char *)xmalloc(p - buf);
    strcpy(h->name, buf);
    p += strspn(p, " \t");
    h->value = (char *)xmalloc(strlen(p) + 1);
    strcpy(h->value, p);
    return (h);
}

static long content_length(struct HTTPRequest *req)
{
    char *val;
    long len;

    val = lookup_header_field_value(req, "Content-Length");
    if (!val)
        return (0);
    len = atol(val);
    if (len < 0)
        log_exit("negative Content-Length value");
    return (len);
}

static char *lookup_header_field_value(struct HTTPRequest *req, char *name)
{
    struct HTTPHeaderField *h;

    h = req->header;
    while (h)
    {
        if (strcasecmp(h->name, name) == 0)
            return (h->value);
        h = h->next;
    }
    return (NULL);
}

static void respond_to(struct HTTPRequest *req, FILE *out, char *docroot)
{
    if (strcmp(req->method, "GET") == 0)
        do_file_response(req, out, docroot);
    else if (strcmp(req->method, "HEAD") == 0)
        do_file_response(req, out, docroot);
    else if (strcmp(req->method, "POST") == 0)
        method_not_allowed(req, out);
    else
        not_implemented(req, out);
}

static void do_file_response(struct HTTPRequest *req, FILE *out, char *docroot)
{
    struct FileInfo *info;

    info = get_fileinfo(docroot, req->path);
    if (!info->ok)
    {
        free_fileinfo(info);
        not_found(req, out);
        return;
    }
    output_common_header_fields(req, out, "200 OK");
    fprintf(out, "Content-Length: %ld\r\n", info->size);
    fprintf(out, "Content-Type: %s\r\n", guess_content_type(info));
    fprintf(out, "\r\n");
    if (strcmp(req->method, "HEAD") != 0)
    {
        int fd;
        char buf[BUFSIZ];
        ssize_t n;

        fd = open(info->path, O_RDONLY);
        if (fd < 0)
            log_exit("failed to open %s: %s", info->path, strerror(errno));
        while (1)
        {
            n = read(fd, buf, sizeof(buf));
            if (n < 0)
                log_exit("failed to read %s: %s", info->path, strerror(errno));
            if (n == 0)
                break;
            if (fwrite(buf, sizeof(char), n, out) < (size_t)n)
                log_exit("failed to write to socket: %s", strerror(errno));
        }
        close(fd);
    }
    fflush(out);
    free_fileinfo(info);
}

static void output_common_header_fields(struct HTTPRequest *req, FILE *out, char *status)
{
    time_t t;
    struct tm *tm;
    char buf[BUFSIZ];

    t = time(NULL);
    tm = gmtime(&t);
    if (!tm)
        log_exit("gmtime() failed: %s", strerror(errno));
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", tm);
    fprintf(out, "HTTP/1.%d %s\r\n", req->protocol_minor_version, status);
    fprintf(out, "Date: %s\r\n", buf);
    fprintf(out, "Server: %s/%s\r\n", SERVER_NAME, SERVER_VERSION);
    fprintf(out, "Connection: close\r\n");
}

static char *guess_content_type(struct FileInfo *info)
{
    (void)info;
    return ("text/html");
}

static void method_not_allowed(struct HTTPRequest *req, FILE *out)
{
    output_common_header_fields(req, out, "405 Method Not Allowed");
}

static void not_implemented(struct HTTPRequest *req, FILE *out)
{
    output_common_header_fields(req, out, "501 Not Implemented");
}

static void not_found(struct HTTPRequest *req, FILE *out)
{
    output_common_header_fields(req, out, "404 Not Found");
}

static struct FileInfo *get_fileinfo(char *docroot, char *urlpath)
{
    struct FileInfo *info;
    struct stat st;

    info = (struct FileInfo *)xmalloc(sizeof(struct FileInfo));
    info->path = build_fspath(docroot, urlpath);
    info->ok = 0;
    if (lstat(info->path, &st) < 0)
        return (info);
    if (!S_ISREG(st.st_mode))
        return (info);
    info->ok = 1;
    info->size = st.st_size;
    return (info);
}

static char *build_fspath(char *docroot, char *urlpath)
{
    char *path;

    path = (char *)xmalloc(sizeof(char) * (strlen(docroot) + 1 + strlen(urlpath) + 1));
    sprintf(path, "%s/%s", docroot, urlpath);
    return (path);
}

static void free_fileinfo(struct FileInfo *info)
{
    free(info->path);
    free(info);
}

static void free_request(struct HTTPRequest *req)
{
    struct HTTPHeaderField *h, *head;

    head = req->header;
    while (head)
    {
        h = head;
        head = head->next;
        free(h->name);
        free(h->value);
        free(h);
    }
    free(req->method);
    free(req->path);
    free(req->body);
    free(req);
}

static void log_exit(char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (debug_mode)
    {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    else
        vsyslog(LOG_ERR, fmt, ap);
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t sz)
{
    void *p;

    p = (void *)malloc(sz);
    if (!p)
        log_exit("failed to allocate memory");
    return (p);
}

static void install_signal_handlers(void)
{
    trap_signal(SIGPIPE, signal_exit, SA_RESTART);
    trap_signal(SIGCHLD, noop_handler, SA_RESTART | SA_NOCLDWAIT);
}

static void trap_signal(int sig, __sighandler_t handler, int flags)
{
    struct sigaction act;

    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = flags;
    if (sigaction(sig, &act, NULL) < 0)
        log_exit("sigaction() failed: %s", strerror(errno));
}

static void signal_exit(int sig)
{
    log_exit("exit by signal %d", sig);
}

static void noop_handler(int sig)
{
    (void)sig;
}
