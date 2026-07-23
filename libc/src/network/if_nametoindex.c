#define _GNU_SOURCE
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <string.h>
#include "syscall.h"

unsigned if_nametoindex(const char *name)
{
	struct ifreq ifr;
	int fd, r;

	if ((fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0)) < 0) return 0;
	memset(&ifr, 0, sizeof ifr);
	for (size_t i = 0; i + 1 < sizeof ifr.ifr_name && name[i]; i++)
		ifr.ifr_name[i] = name[i];
	r = ioctl(fd, SIOCGIFINDEX, &ifr);
	__syscall(SYS_close, fd);
	return r < 0 ? 0 : ifr.ifr_ifindex;
}
