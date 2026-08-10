/*
 * UnixOS Kernel - Network Stack Implementation
 */

#include "fs/vfs.h"
#include "mm/kmalloc.h"
#include "net/net.h"
#include "printk.h"

/* Additional error codes for sockets */
#ifndef EMFILE
#define EMFILE 24
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT 97
#endif
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 94
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ 89
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif

#define UDP_RECV_CAPACITY 2048

struct udp_recv_queue {
  uint8_t data[UDP_RECV_CAPACITY];
  size_t len;
  uint32_t src_ip;
  uint16_t src_port;
  bool has_packet;
};

struct tcp_socket_state {
  int conn_id;
};

/* ===================================================================== */
/* Socket table */
/* ===================================================================== */

#define MAX_SOCKETS 256

static struct socket *socket_table[MAX_SOCKETS];
static int next_sockfd = 0;
static uint16_t next_udp_port = 49152;

/* ===================================================================== */
/* Byte order functions */
/* ===================================================================== */

uint16_t htons(uint16_t hostshort) {
  return ((hostshort >> 8) & 0xFF) | ((hostshort & 0xFF) << 8);
}

uint16_t ntohs(uint16_t netshort) { return htons(netshort); }

uint32_t htonl(uint32_t hostlong) {
  return ((hostlong >> 24) & 0xFF) | ((hostlong >> 8) & 0xFF00) |
         ((hostlong & 0xFF00) << 8) | ((hostlong & 0xFF) << 24);
}

uint32_t ntohl(uint32_t netlong) { return htonl(netlong); }

/* ===================================================================== */
/* IP checksum */
/* ===================================================================== */

static uint16_t __attribute__((unused)) ip_checksum(void *data, int len) {
  uint32_t sum = 0;
  uint16_t *p = data;

  while (len > 1) {
    sum += *p++;
    len -= 2;
  }

  if (len == 1) {
    sum += *(uint8_t *)p;
  }

  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }

  return ~sum;
}

/* ===================================================================== */
/* Initialization */
/* ===================================================================== */

void net_init(void) {
  printk(KERN_INFO "NET: Initializing network stack\n");

  /* Clear socket table */
  for (int i = 0; i < MAX_SOCKETS; i++) {
    socket_table[i] = NULL;
  }

  printk(KERN_INFO "NET: TCP/IP stack initialized\n");
  printk(KERN_INFO "NET: IPv4 support enabled\n");
}

/* ===================================================================== */
/* Socket operations */
/* ===================================================================== */

static int alloc_sockfd(void) {
  for (int i = 0; i < MAX_SOCKETS; i++) {
    int fd = (next_sockfd + i) % MAX_SOCKETS;
    if (!socket_table[fd]) {
      next_sockfd = fd + 1;
      return fd;
    }
  }
  return -EMFILE;
}

static int reserve_sockfd(int sockfd) {
  if (sockfd < 0 || sockfd >= MAX_SOCKETS || socket_table[sockfd]) {
    return -EMFILE;
  }
  if (sockfd >= next_sockfd) {
    next_sockfd = sockfd + 1;
  }
  return sockfd;
}

static struct sockaddr_in *socket_addr_in(struct sockaddr_storage *addr) {
  return (struct sockaddr_in *)addr;
}

static uint16_t socket_get_local_port(struct socket *sock) {
  struct sockaddr_in *local = socket_addr_in(&sock->local_addr);
  if (local->sin_port == 0) {
    local->sin_port = htons(next_udp_port++);
    if (next_udp_port < 49152) {
      next_udp_port = 49152;
    }
  }
  return ntohs(local->sin_port);
}

static int socket_create_in_slot(int sockfd, int family, int type,
                                 int protocol) {
  if (family != AF_INET && family != AF_INET6) {
    return -EAFNOSUPPORT;
  }

  if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW) {
    return -ESOCKTNOSUPPORT;
  }

  int fd = sockfd >= 0 ? reserve_sockfd(sockfd) : alloc_sockfd();
  if (fd < 0) {
    return fd;
  }

  struct socket *sock = kzalloc(sizeof(struct socket), GFP_KERNEL);
  if (!sock) {
    return -ENOMEM;
  }

  sock->type = type;
  sock->protocol = protocol;
  sock->state = SS_UNCONNECTED;
  sock->local_addr.ss_family = family;
  sock->remote_addr.ss_family = family;
  if (type == SOCK_STREAM) {
    struct tcp_socket_state *tcp = kzalloc(sizeof(struct tcp_socket_state), GFP_KERNEL);
    if (!tcp) {
      kfree(sock);
      return -ENOMEM;
    }
    tcp->conn_id = -1;
    sock->sk = tcp;
  } else if (type == SOCK_DGRAM) {
    sock->sk = kzalloc(sizeof(struct udp_recv_queue), GFP_KERNEL);
    if (!sock->sk) {
      kfree(sock);
      return -ENOMEM;
    }
  } else {
    sock->sk = NULL;
  }

  socket_table[fd] = sock;

  printk(KERN_DEBUG "NET: Created socket %d (type=%d)\n", fd, type);

  return fd;
}

int socket_create(int family, int type, int protocol) {
  return socket_create_in_slot(-1, family, type, protocol);
}

int socket_create_at(int sockfd, int family, int type, int protocol) {
  return socket_create_in_slot(sockfd, family, type, protocol);
}

int socket_bind(int sockfd, const struct sockaddr *addr, unsigned int addrlen) {
  if (sockfd < 0 || sockfd >= MAX_SOCKETS || !socket_table[sockfd]) {
    return -EBADF;
  }

  if (!addr || addrlen < sizeof(struct sockaddr)) {
    return -EINVAL;
  }

  struct socket *sock = socket_table[sockfd];

  /* Copy address */
  uint8_t *dst = (uint8_t *)&sock->local_addr;
  const uint8_t *src = (const uint8_t *)addr;
  for (unsigned int i = 0; i < addrlen && i < sizeof(sock->local_addr); i++) {
    dst[i] = src[i];
  }

  printk(KERN_DEBUG "NET: Socket %d bound\n", sockfd);

  return 0;
}

int socket_listen(int sockfd, int backlog) {
  if (sockfd < 0 || sockfd >= MAX_SOCKETS || !socket_table[sockfd]) {
    return -EBADF;
  }

  struct socket *sock = socket_table[sockfd];

  if (sock->type != SOCK_STREAM) {
    return -EOPNOTSUPP;
  }

  (void)backlog;
  sock->state = SS_UNCONNECTED; /* Listening state would be set here */

  printk(KERN_DEBUG "NET: Socket %d listening\n", sockfd);

  return 0;
}

int socket_accept(int sockfd, struct sockaddr *addr, unsigned int *addrlen) {
  if (sockfd < 0 || sockfd >= MAX_SOCKETS || !socket_table[sockfd]) {
    return -EBADF;
  }

  (void)addr;
  (void)addrlen;

  return -EAGAIN;
}

int socket_connect(int sockfd, const struct sockaddr *addr,
                   unsigned int addrlen) {
  if (sockfd < 0 || sockfd >= MAX_SOCKETS || !socket_table[sockfd]) {
    return -EBADF;
  }

  if (!addr || addrlen < sizeof(struct sockaddr)) {
    return -EINVAL;
  }

  struct socket *sock = socket_table[sockfd];

  /* Copy remote address */
  uint8_t *dst = (uint8_t *)&sock->remote_addr;
  const uint8_t *src = (const uint8_t *)addr;
  for (unsigned int i = 0; i < addrlen && i < sizeof(sock->remote_addr); i++) {
    dst[i] = src[i];
  }

  if (sock->type == SOCK_DGRAM) {
    sock->state = SS_CONNECTED;
    return 0;
  }

  if (sock->type != SOCK_STREAM) {
    return -EOPNOTSUPP;
  }

  sock->state = SS_CONNECTING;
  struct sockaddr_in *remote = socket_addr_in(&sock->remote_addr);
  if (remote->sin_family != AF_INET || remote->sin_port == 0) {
    sock->state = SS_UNCONNECTED;
    return -EINVAL;
  }

  int conn_id = tcp_connect(remote->sin_addr.s_addr, ntohs(remote->sin_port));
  if (conn_id < 0) {
    sock->state = SS_UNCONNECTED;
    return -ECONNREFUSED;
  }

  struct tcp_socket_state *tcp = (struct tcp_socket_state *)sock->sk;
  if (!tcp) {
    tcp_close_socket(conn_id);
    sock->state = SS_UNCONNECTED;
    return -ENOMEM;
  }
  tcp->conn_id = conn_id;
  sock->state = SS_CONNECTED;
  return 0;
}

ssize_t socket_send(int sockfd, const void *buf, size_t len, int flags) {
  if (sockfd < 0 || sockfd >= MAX_SOCKETS || !socket_table[sockfd]) {
    return -EBADF;
  }

  if (!buf) {
    return -EINVAL;
  }

  (void)flags;

  struct socket *sock = socket_table[sockfd];

  if (sock->state != SS_CONNECTED && sock->type == SOCK_STREAM) {
    return -ENOTCONN;
  }

  if (sock->type == SOCK_STREAM) {
    if (len > 0xFFFFFFFFu) {
      return -EMSGSIZE;
    }
    struct tcp_socket_state *tcp = (struct tcp_socket_state *)sock->sk;
    if (!tcp || tcp->conn_id < 0 || !tcp_is_connected_socket(tcp->conn_id)) {
      return -ENOTCONN;
    }
    int sent = tcp_send_socket(tcp->conn_id, buf, (uint32_t)len);
    return sent < 0 ? -ENOTCONN : (ssize_t)sent;
  }

  if (sock->type == SOCK_DGRAM) {
    if (len > 0xFFFFu) {
      return -EMSGSIZE;
    }
    if (sock->remote_addr.ss_family != AF_INET) {
      return -EDESTADDRREQ;
    }

    struct sockaddr_in *remote = socket_addr_in(&sock->remote_addr);
    uint16_t src_port = socket_get_local_port(sock);
    uint16_t dst_port = ntohs(remote->sin_port);
    if (dst_port == 0) {
      return -EDESTADDRREQ;
    }

    int sent = udp_send(remote->sin_addr.s_addr, src_port, dst_port, buf, len);
    return sent < 0 ? sent : (ssize_t)sent;
  }

  return -ENOTCONN;
}

ssize_t socket_recv(int sockfd, void *buf, size_t len, int flags) {
  if (sockfd < 0 || sockfd >= MAX_SOCKETS || !socket_table[sockfd]) {
    return -EBADF;
  }

  if (!buf) {
    return -EINVAL;
  }

  (void)len;
  (void)flags;

  struct socket *sock = socket_table[sockfd];

  if (sock->state != SS_CONNECTED && sock->type == SOCK_STREAM) {
    return -ENOTCONN;
  }

  if (sock->type == SOCK_STREAM) {
    if (len > 0xFFFFFFFFu) {
      len = 0xFFFFFFFFu;
    }
    struct tcp_socket_state *tcp = (struct tcp_socket_state *)sock->sk;
    if (!tcp || tcp->conn_id < 0 || !tcp_is_connected_socket(tcp->conn_id)) {
      return -ENOTCONN;
    }
    int received = tcp_recv_socket(tcp->conn_id, buf, (uint32_t)len);
    return received < 0 ? -ENOTCONN : (ssize_t)received;
  }

  if (sock->type == SOCK_DGRAM) {
    struct udp_recv_queue *queue = (struct udp_recv_queue *)sock->sk;
    if (!queue || !queue->has_packet) {
      return 0;
    }

    size_t to_copy = len < queue->len ? len : queue->len;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < to_copy; i++) {
      dst[i] = queue->data[i];
    }
    queue->has_packet = false;
    return (ssize_t)to_copy;
  }

  return 0;
}

void socket_udp_deliver(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
                        uint16_t dst_port, const void *data, size_t len) {
  if (!data || len > UDP_RECV_CAPACITY) {
    return;
  }

  for (int i = 0; i < MAX_SOCKETS; i++) {
    struct socket *sock = socket_table[i];
    if (!sock || sock->type != SOCK_DGRAM || !sock->sk) {
      continue;
    }

    struct sockaddr_in *local = socket_addr_in(&sock->local_addr);
    if (local->sin_family != AF_INET || ntohs(local->sin_port) != dst_port) {
      continue;
    }
    if (local->sin_addr.s_addr != INADDR_ANY && local->sin_addr.s_addr != dst_ip) {
      continue;
    }

    if (sock->state == SS_CONNECTED) {
      struct sockaddr_in *remote = socket_addr_in(&sock->remote_addr);
      if (remote->sin_family == AF_INET &&
          (remote->sin_addr.s_addr != src_ip || ntohs(remote->sin_port) != src_port)) {
        continue;
      }
    }

    struct udp_recv_queue *queue = (struct udp_recv_queue *)sock->sk;
    uint8_t *dst = queue->data;
    const uint8_t *src = (const uint8_t *)data;
    for (size_t j = 0; j < len; j++) {
      dst[j] = src[j];
    }
    queue->len = len;
    queue->src_ip = src_ip;
    queue->src_port = src_port;
    queue->has_packet = true;
    return;
  }
}

int socket_close(int sockfd) {
  if (sockfd < 0 || sockfd >= MAX_SOCKETS || !socket_table[sockfd]) {
    return -EBADF;
  }

  struct socket *sock = socket_table[sockfd];

  if (sock->type == SOCK_STREAM && sock->sk) {
    struct tcp_socket_state *tcp = (struct tcp_socket_state *)sock->sk;
    if (tcp->conn_id >= 0) {
      tcp_close_socket(tcp->conn_id);
    }
  }
  if (sock->sk) {
    kfree(sock->sk);
  }
  kfree(sock);
  socket_table[sockfd] = NULL;

  return 0;
}
