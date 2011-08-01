#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <assert.h>
#include <string.h>
#include <inttypes.h>
#include <endian.h>
#include <linux/sockios.h>

#define SIOCPEEKOUTQ	0x894F		/* peek output queue */

static const struct timespec ts1ms = { .tv_nsec = 1000000 };
static const char *contaminant = NULL;
static int do_peek_outq;

static void sigaction_handler(int signo, siginfo_t *si, void *uctx)
{
	printf("signal %d si_code=%#x\n", signo, si->si_code);
	if (signo == SIGUSR1)
		contaminant = "deadbeef";
	else
		do_peek_outq = 1;
}

static void peek_outq(int sock, uint64_t cur)
{
	int size, ret;
	char *buf, *p;
	int nr_contaminants = 0;
	uint64_t val;

	assert(!(ioctl(sock, SIOCOUTQ, &size)));
	buf = malloc(size);
	assert(buf);

	memcpy(buf, &size, sizeof(size));
	ret = ioctl(sock, SIOCPEEKOUTQ, buf);
	if (ret < 0) {
		perror("SIOCPEEKOUTQ");
		return;
	}
	assert(ret <= size);

	printf("peek_outq %d bytes: ", ret);
	cur--;

	printf("[%#08llx", (unsigned long long)cur);

	for (p = buf + ret - sizeof(val); p > buf; p -= sizeof(val)) {
		memcpy(&val, p, sizeof(val));
		val = be64toh(val);

		if (val == cur) {
			cur--;
		} else {
			printf(" #%#08llx", (unsigned long long)cur + 1);
			nr_contaminants++;
		}
	}

	printf(" %#08llx]", (unsigned long long)cur + 1);
	printf(" nr_contaminants=%d\n", nr_contaminants);
}

static void *send_thread_fn(void *arg)
{
	int sock = (long)arg;
	uint64_t cur = 0, buf;

	while (1) {
		if (do_peek_outq) {
			peek_outq(sock, cur);
			do_peek_outq = 0;
		}

		if (!contaminant) {
			buf = htobe64(cur++);
		} else {
			printf("inserting contaminant @%#08llx\n",
			       (unsigned long long)cur);
			memcpy(&buf, contaminant, sizeof(buf));
			contaminant = NULL;
		}
		assert(send(sock, &buf, sizeof(buf), 0) == sizeof(buf));
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_sigaction = sigaction_handler,
				.sa_flags = SA_SIGINFO };
	struct sockaddr_in in = { .sin_family = AF_INET,
				  .sin_addr.s_addr = INADDR_ANY };
	unsigned long rx_kbps = 128;
	uint64_t next = 0, timestamp;
	struct timeval tv;
	socklen_t slen;
	pthread_t pth;
	char *p;
	int i, sock;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: net-host [IP:]PORT [RX_KPBS]\n");
		return 1;
	}

	if (argc == 3) {
		rx_kbps = strtoul(argv[2], &p, 0);
		assert(p && *p == '\0');
	}

	if ((p = strchr(argv[1], ':'))) {
		*p++ = '\0';
		assert(inet_aton(argv[1], &in.sin_addr));
	} else {
		p = argv[1];
	}
	in.sin_port = htons(strtoul(p, &p, 0));
	assert(p && *p == '\0');

	assert((sock = socket(AF_INET, SOCK_STREAM, 0)) >= 0);

	if (in.sin_addr.s_addr != INADDR_ANY) {
		assert(!connect(sock, (struct sockaddr *)&in, sizeof(in)));
	} else {
		assert(!bind(sock, (struct sockaddr *)&in, sizeof(in)));
		assert(!listen(sock, 5));
		sock = accept(sock, NULL, NULL);
	}

	slen = sizeof(in);
	assert(!getpeername(sock, (struct sockaddr *)&in, &slen));
	printf("Connected to %s:%u\n",
	       inet_ntoa(in.sin_addr), ntohs(in.sin_port));

	assert(!sigaction(SIGUSR1, &sa, NULL));
	assert(!sigaction(SIGUSR2, &sa, NULL));

	assert(!pthread_create(&pth, NULL, send_thread_fn, (void *)(long)sock));

	assert(!gettimeofday(&tv, NULL));
	timestamp = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;

	while (1) {
		uint64_t now, buf;
		char cbuf[sizeof(buf) + 1] = "";
		int usecs, cnt;

		assert(!gettimeofday(&tv, NULL));
		now = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
		usecs = now - timestamp ?: 1;
		usecs = usecs <= 0 || usecs > 1000000 ? 1000000 : usecs;
		timestamp = now;

		cnt = (usecs * rx_kbps) / 64 / 1000 ?: 1;

		for (i = 0; i < cnt; i++) {
			assert(recv(sock, &buf, sizeof(buf), MSG_WAITALL) ==
			       sizeof(buf));
			if (be64toh(buf) != next) {
				memcpy(cbuf, &buf, sizeof(buf));
				printf("foreign data @%#08llx : \"%s\"\n",
				       (unsigned long long)next, cbuf);
				continue;
			}
			next++;
		}

		nanosleep(&ts1ms, NULL);
	}

	return 0;
}