#define _GNU_SOURCE
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include "syscall.h"

char *if_indextoname(unsigned index, char *name)
{
	struct ifreq ifr;
	int fd, r;

	if ((fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0)) < 0) return 0;
	ifr.ifr_ifindex = index;
	r = ioctl(fd, SIOCGIFNAME, &ifr);
	__syscall(SYS_close, fd);
	if (r < 0) {
		if (errno == ENODEV) errno = ENXIO;
		return 0;
	}
	{
		size_t i;
		for (i = 0; i + 1 < IF_NAMESIZE && ifr.ifr_name[i]; i++)
			name[i] = ifr.ifr_name[i];
		name[i] = 0;
	}
	return name;
}
