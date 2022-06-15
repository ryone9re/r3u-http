#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

static int open_connection(char *host, char *service);

int main(int argc, char **argv)
{
    int sock;
    FILE *fp;
    char buf[BUFSIZ];
    char *b;

    sock = open_connection((argc > 1 ? argv[1] : "localhost"), "daytime");
    fp = fdopen(sock, "r");
    if (!fp)
    {
        perror("fdopen(3)");
        exit(1);
    }
    b = fgets(buf, sizeof(buf), fp);
    fclose(fp);
    fputs(b, stdout);
    exit(0);
}

static int open_connection(char *host, char *service)
{
    int sock;
    struct addrinfo hints, *res, *ai;
    int err;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((err = getaddrinfo(host, service, &hints, &res)) != 0)
    {
        fprintf(stderr, "getaddrinfo(3): %s\n", gai_strerror(err));
        exit(1);
    }
    for (ai = res; ai; ai = ai->ai_next)
    {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
            continue;
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) < 0)
        {
            close(sock);
            continue;
        }
        freeaddrinfo(ai);
        return (sock);
    }
    fprintf(stderr, "socker(2)/connect(2) failed\n");
    freeaddrinfo(ai);
    exit(1);
}
