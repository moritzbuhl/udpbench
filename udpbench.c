/*
 * Copyright (c) 2019 Alexander Bluhm <bluhm@genua.de>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

sig_atomic_t alarm_signaled;

int udp_family;
int udp_socket = -1;
FILE *ssh_stream;
pid_t ssh_pid;

void alarm_handler(int);
void udp_bind(const char *, const char *);
void udp_connect(const char *, const char *);
void udp_getsockname(char **, char **);
void udp_buffersize(int);
void udp_send(const char *, size_t);
void udp_receive(char *, size_t);
void ssh_bind(const char *, const char *, const char *, const char *,
    int, size_t , int);
void ssh_connect(const char *, const char *, const char *, const char *,
    int, size_t , int);
void ssh_pipe(char **);
void ssh_getpeername(char **, char **);
void ssh_wait(void);

static void __dead
usage(void)
{
	fprintf(stderr, "usage: udpperf [-b bufsize] [-l length] [-p port] "
	    "[-s remotessh] [-t timeout] "
	    "send|recv [hostname]\n"
	    "    -b bufsize     set size of send or receive buffer\n"
	    "    -l length      set length of udp payload\n"
	    "    -p port        udp port for bind or connect, default 12345\n"
	    "    -s remotessh	ssh host to start the remote udpperf\n"
	    "    -t timeout     send duration or receive timeout, default 1\n"
	    );
	exit(2);
}

int plen;

enum direction {
    DIR_NONE,
    DIR_SEND,
    DIR_RECV,
} dir;

int
main(int argc, char *argv[])
{
	struct sigaction act;
	const char *errstr;
	char *udp_payload;
	size_t udp_length = 0;
	int ch, buffer_size = 0, timeout = 1;
	const char *progname = argv[0];
	char *hostname = NULL, *service = "12345", *remotessh = NULL;
	char *localaddr, *localport;

	if (pledge("stdio dns inet proc exec", NULL) == -1)
		err(1, "pledge");

	if (setvbuf(stdout, NULL, _IOLBF, 0) != 0)
		err(1, "setvbuf");

	while ((ch = getopt(argc, argv, "b:l:p:s:t:")) != -1) {
		switch (ch) {
		case 'b':
			buffer_size = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "buffer size is %s: %s", errstr,
				    optarg);
			break;
		case 'l':
			udp_length = strtonum(optarg, 0, IP_MAXPACKET, &errstr);
			if (errstr != NULL)
				errx(1, "payload length is %s: %s", errstr,
				    optarg);
			break;
		case 'p':
			service = optarg;
			break;
		case 's':
			remotessh = optarg;
			break;
		case 't':
			timeout = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "timeout is %s: %s", errstr,
				    optarg);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 2)
		usage();
	if (argc < 1)
		errx(1, "no mode and direction");

	if (strcmp(argv[0], "send") == 0)
		dir = DIR_SEND;
	else if (strcmp(argv[0], "recv") == 0)
		dir = DIR_RECV;
	else
		errx(1, "unknown direction: %s", argv[0]);

	if (dir == DIR_SEND && argc < 2)
		errx(1, "no hostname");
	if (argc >= 2)
		hostname = argv[1];
	if (remotessh == NULL) {
		if (pledge("stdio dns inet", NULL) == -1)
			err(1, "pledge");
	}

	memset(&act, 0, sizeof(act));
	act.sa_handler = alarm_handler;
	act.sa_flags = SA_RESETHAND;
	if (sigaction(SIGALRM, &act, NULL) == -1)
		err(1, "sigaction");

	udp_payload = malloc(udp_length);
	if (udp_payload == NULL)
		err(1, "malloc udp payload");
	if (dir == DIR_SEND) {
		arc4random_buf(udp_payload, udp_length);
		if (remotessh != NULL) {
			ssh_bind(remotessh, progname, hostname, service,
			    buffer_size, udp_length, timeout);
			if (pledge("stdio dns inet", NULL) == -1)
				err(1, "pledge");
			ssh_getpeername(&hostname, &service);
		}
		udp_connect(hostname, service);
		udp_getsockname(&localaddr, &localport);
		udp_buffersize(buffer_size);
		if (timeout > 0)
			alarm(timeout);
		udp_send(udp_payload, udp_length);
		if (remotessh != NULL)
			ssh_wait();
	} else {
		udp_bind(hostname, service);
		udp_getsockname(&localaddr, &localport);
		udp_buffersize(buffer_size);
		if (remotessh != NULL) {
			ssh_connect(remotessh, progname, localaddr, localport,
			buffer_size, udp_length, timeout);
			if (pledge("stdio dns inet", NULL) == -1)
				err(1, "pledge");
			ssh_getpeername(NULL, NULL);
		}
		if (timeout > 0)
			alarm(timeout + 3);
		udp_receive(udp_payload, udp_length);
		if (remotessh != NULL)
			ssh_wait();
	}
	free(localaddr);
	free(localport);

	return 0;
}

void
alarm_handler(int sig)
{
	alarm_signaled = 1;
}

void
udp_bind(const char *host, const char *service)
{
	struct addrinfo hints, *res, *res0;
	int error;
	int save_errno;
	const char *cause = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = 17;
	hints.ai_flags = AI_PASSIVE;
	error = getaddrinfo(host, service, &hints, &res0);
	if (error)
		errx(1, "getaddrinfo: %s", gai_strerror(error));
	udp_socket = -1;
	for (res = res0; res; res = res->ai_next) {
		udp_socket = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
		if (udp_socket == -1) {
			cause = "socket";
			continue;
		}

		if (bind(udp_socket, res->ai_addr, res->ai_addrlen) == -1) {
			cause = "bind";
			save_errno = errno;
			close(udp_socket);
			errno = save_errno;
			udp_socket = -1;
			continue;
		}

		break;  /* okay we got one */
	}
	if (udp_socket == -1)
		err(1, "%s", cause);
	udp_family = res->ai_family;
	freeaddrinfo(res0);
}

void
udp_connect(const char *host, const char *service)
{
	struct addrinfo hints, *res, *res0;
	int error;
	int save_errno;
	const char *cause = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = 17;
	error = getaddrinfo(host, service, &hints, &res0);
	if (error)
		errx(1, "getaddrinfo: %s", gai_strerror(error));
	udp_socket = -1;
	for (res = res0; res; res = res->ai_next) {
		udp_socket = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
		if (udp_socket == -1) {
			cause = "socket";
			continue;
		}

		if (connect(udp_socket, res->ai_addr, res->ai_addrlen) == -1) {
			cause = "connect";
			save_errno = errno;
			close(udp_socket);
			errno = save_errno;
			udp_socket = -1;
			continue;
		}

		break;  /* okay we got one */
	}
	if (udp_socket == -1)
		err(1, "%s", cause);
	udp_family = res->ai_family;
	freeaddrinfo(res0);
}

void
udp_getsockname(char **addr, char **port)
{
	struct sockaddr_storage ss;
	struct sockaddr *sa = (struct sockaddr *)&ss;
	socklen_t len;
	int error;

	len = sizeof(ss);
	if (getsockname(udp_socket, sa, &len) == -1)
		err(1, "getsockname");

	*addr = malloc(NI_MAXHOST);
	if (*addr == NULL)
		err(1, "malloc addr");
	*port = malloc(NI_MAXSERV);
	if (*port == NULL)
		err(1, "malloc port");

	error = getnameinfo(sa, len, *addr, NI_MAXHOST, *port, NI_MAXSERV,
	    NI_NUMERICHOST | NI_NUMERICSERV | NI_DGRAM);
	if (error)
		errx(1, "getnameinfo: %s", gai_strerror(error));

	printf("sockname: %s %s\n", *addr, *port);
}

void
udp_buffersize(int size)
{
	socklen_t len;
	int name;

	/* use default */
	if (size == 0)
		return;

	name = (dir == DIR_SEND) ? SO_SNDBUF : SO_RCVBUF;
	len = sizeof(size);
	if (setsockopt(udp_socket, SOL_SOCKET, name, &size, len) == -1)
		err(1, "setsockopt buffer size %d", size);

}

void
udp_send(const char *payload, size_t udplen)
{
	struct timeval begin, end, duration;
	unsigned long count;
	size_t length;
	double bits;

	if (gettimeofday(&begin, NULL) == -1)
		err(1, "gettimeofday begin");

	count = 0;
	while (!alarm_signaled) {
		if (send(udp_socket, payload, udplen, 0) == -1) {
			if (errno == ENOBUFS)
				continue;
			err(1, "send");
		}
		count++;
	}

	if (gettimeofday(&end, NULL) == -1)
		err(1, "gettimeofday end");

	length = (udp_family == AF_INET) ?
	    sizeof(struct ip) : sizeof(struct ip6_hdr);
	length += sizeof(struct udphdr) + udplen;
	timersub(&end, &begin, &duration);
	bits = (double)count * length;
	bits /= (double)duration.tv_sec + duration.tv_usec / 1000000.;
	printf("send: count %lu, length %zu, duration %lld.%06ld, bit/s %g\n",
	    count, length, duration.tv_sec, duration.tv_usec, bits);
}

void
udp_receive(char *payload, size_t udplen)
{
	struct timeval begin, idle, end, duration, timeo;
	unsigned long count, syscall, bored;
	size_t length;
	ssize_t rcvlen;
	socklen_t len;
	double bits;

	/* wait for the first packet to start timing */
	rcvlen = recv(udp_socket, payload, udplen, 0);
	if (rcvlen == -1)
		err(1, "recv 1");

	if (gettimeofday(&begin, NULL) == -1)
		err(1, "gettimeofday begin");
	timerclear(&idle);

	timeo.tv_sec = 0;
	timeo.tv_usec = 100000;
	len = sizeof(timeo);
	if (setsockopt(udp_socket, SOL_SOCKET, SO_RCVTIMEO, &timeo, len) == -1)
		err(1, "setsockopt recv timeout");

	count = 1;
	syscall = 1;
	bored = 0;
	while (!alarm_signaled) {
		syscall++;
		if (recv(udp_socket, payload, udplen, 0) == -1) {
			if (errno == EWOULDBLOCK) {
				bored++;
				if (bored == 1) {
					if (gettimeofday(&idle, NULL) == -1)
						err(1, "gettimeofday idle");
					/* packet was seen before timeout */
					timersub(&idle, &timeo, &idle);
				}
				continue;
			}
			if (errno == EINTR)
			break;
			err(1, "recv");
		}
		bored = 0;
		count++;
	}

	if (gettimeofday(&end, NULL) == -1)
		err(1, "gettimeofday end");

	length = (udp_family == AF_INET) ?
	    sizeof(struct ip) : sizeof(struct ip6_hdr);
	length += sizeof(struct udphdr) + rcvlen;
	if (timerisset(&idle)) {
		timersub(&idle, &begin, &duration);
		timersub(&end, &idle, &idle);
	} else {
		timersub(&end, &begin, &duration);
	}
	bits = (double)count * length;
	bits /= (double)duration.tv_sec + duration.tv_usec / 1000000.;
	printf("recv: count %lu, length %zu, duration %lld.%06ld, bit/s %g\n",
	    count, length, duration.tv_sec, duration.tv_usec, bits);
	if (idle.tv_sec < 1)
		errx(1, "not enough idle time: %lld.%06ld",
		    idle.tv_sec, idle.tv_usec);
}

void
ssh_bind(const char *remotessh, const char *progname,
    const char *hostname, const char *service,
    int buffer_size, size_t udp_length, int timeout)
{
	char *argv[14];

	argv[0] = "ssh";
	argv[1] = (char *)remotessh;
	argv[2] = (char *)progname;
	argv[3] = "-b";
	if (asprintf(&argv[4], "%d", buffer_size) == -1)
		err(1, "asprintf buffer size");
	argv[5] = "-l";
	if (asprintf(&argv[6], "%zu", udp_length) == -1)
		err(1, "asprintf udp length");
	argv[7] = "-p";
	argv[8] = (char *)service;
	argv[9] = "-t";
	if (asprintf(&argv[10], "%d", timeout + 1) == -1)
		err(1, "asprintf timeout");
	argv[11] = "recv";
	argv[12] = (char *)hostname;
	argv[13] = NULL;

	ssh_pipe(argv);

	free(argv[4]);
	free(argv[6]);
	free(argv[10]);
}

void
ssh_connect(const char *remotessh, const char *progname,
    const char *hostname, const char *service,
    int buffer_size, size_t udp_length, int timeout)
{
	char *argv[14];

	argv[0] = "ssh";
	argv[1] = (char *)remotessh;
	argv[2] = (char *)progname;
	argv[3] = "-b";
	if (asprintf(&argv[4], "%d", buffer_size) == -1)
		err(1, "asprintf buffer size");
	argv[5] = "-l";
	if (asprintf(&argv[6], "%zu", udp_length) == -1)
		err(1, "asprintf udp length");
	argv[7] = "-p";
	argv[8] = (char *)service;
	argv[9] = "-t";
	if (asprintf(&argv[10], "%d", timeout) == -1)
		err(1, "asprintf timeout");
	argv[11] = "send";
	argv[12] = (char *)hostname;
	argv[13] = NULL;

	ssh_pipe(argv);

	free(argv[4]);
	free(argv[6]);
	free(argv[10]);
}

void
ssh_pipe(char *argv[])
{
	int fp[2];

	if (pipe(fp) == -1)
		err(1, "pipe");
	ssh_pid = fork();
	if (ssh_pid == -1)
		err(1, "fork");
	if (ssh_pid == 0) {
		if (close(fp[0]) == -1)
			err(255, "ssh close read pipe");
		if (dup2(fp[1], 1) == -1)
			err(255, "dup2 pipe");
		if (close(fp[1]) == -1)
			err(255, "ssh close write pipe");
		execvp("ssh", argv);
		err(255, "ssh exec");
	}
	if (close(fp[1]) == -1)
		err(1, "close write pipe");

	ssh_stream = fdopen(fp[0], "r");
	if (ssh_stream == NULL)
		err(1, "fdopen");
}

void
ssh_getpeername(char **addr, char **port)
{
	char *line, *str, **wp, *words[4];
	size_t len;

	line = fgetln(ssh_stream, &len);
	if (line == NULL)
		err(1, "fgetln sockname");
	line = strndup(line, len);
	if (line == NULL)
		err(1, "strndup sockname");
	if (len > 0 && line[len-1] == '\n')
		line[len-1] = '\0';

	str = line;
	for (wp = &words[0]; wp < &words[4]; wp++)
		*wp = strsep(&str, " ");
	if (words[0] == NULL || strcmp("sockname:", words[0]) != 0)
		errx(1, "ssh no sockname: %s", line);
	if (words[1] == NULL)
		errx(1, "ssh no addr");
	if (addr != NULL) {
		*addr = strdup(words[1]);
		if (*addr == NULL)
			err(1, "strdup addr");
	}
	if (words[2] == NULL)
		errx(1, "ssh no port");
	if (port != NULL) {
		*port = strdup(words[2]);
		if (*port == NULL)
			err(1, "strdup port");
	}
	if (words[3] != NULL)
		errx(1, "ssh bad sockname: %s", words[3]);

	printf("peername: %s %s\n", words[1], words[2]);
	free(line);
}

void
ssh_wait(void)
{
	char *line;
	size_t len;
	int status;

	line = fgetln(ssh_stream, &len);
	if (line == NULL)
		err(1, "fgetln status");
	line = strndup(line, len);
	if (line == NULL)
		err(1, "strndup status");
	printf("%s", line);
	free(line);

	if (waitpid(ssh_pid, &status, 0) == -1)
		err(1, "waitpid");
	if (status != 0)
		errx(1, "ssh failed: %d", status);
}
