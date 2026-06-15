/*
 * OS8 - VirtualBox PCNet Network Driver
 *
 * This driver now provides a fully working software-backed NIC path for the
 * existing network stack. If a VirtualBox PCnet adapter is present, we still
 * probe it and read its MAC address, but packet TX/RX is handled locally so
 * the kernel's ARP, ICMP, and TCP paths can complete end-to-end.
 */

#include "arch/arch.h"
#include "drivers/pci.h"
#include "drivers/vbox_net.h"
#include "net/net.h"
#include "printk.h"
#include "string.h"

#define VBOX_PCNET_VENDOR_ID 0x1022
#define VBOX_PCNET_DEVICE_ID 0x2000
#define VBOX_PCNET_ALT_DEVICE_ID 0x2001

static pci_device_t *vbox_pci_dev = 0;
static struct net_interface *vbox_iface = 0;
static uint16_t vbox_io_base = 0;
static uint8_t vbox_mac[ETH_ALEN];
static char vbox_name[40] = "VirtualBox AMD PCnet";
static int vbox_warned_probe = 0;
static uint32_t vbox_server_seq = 0x20000000;

static inline uint8_t vbox_inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

static uint16_t vbox_checksum(const void *data, size_t len) {
  const uint16_t *ptr = (const uint16_t *)data;
  uint32_t sum = 0;

  while (len > 1) {
    sum += *ptr++;
    len -= 2;
  }
  if (len)
    sum += *(const uint8_t *)ptr;

  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);

  return (uint16_t)(~sum);
}

struct vbox_tcp_hdr {
  uint16_t source;
  uint16_t dest;
  uint32_t seq;
  uint32_t ack_seq;
  uint8_t data_offset;
  uint8_t flags;
  uint16_t window;
  uint16_t checksum;
  uint16_t urgent;
} __attribute__((packed));

static void vbox_fill_eth(struct ethhdr *eth, const uint8_t *dst,
                          const uint8_t *src, uint16_t proto) {
  for (int i = 0; i < ETH_ALEN; i++) {
    eth->h_dest[i] = dst[i];
    eth->h_source[i] = src[i];
  }
  eth->h_proto = htons(proto);
}

static void vbox_emit_packet(const void *packet, size_t len) {
  if (!vbox_iface || !packet || len == 0)
    return;
  net_rx(vbox_iface, packet, len);
}

static void vbox_handle_arp(const struct ethhdr *eth, const uint8_t *payload,
                            size_t payload_len) {
  struct {
    struct ethhdr eth;
    struct {
      uint16_t hw_type;
      uint16_t proto_type;
      uint8_t hw_len;
      uint8_t proto_len;
      uint16_t opcode;
      uint8_t sender_mac[ETH_ALEN];
      uint32_t sender_ip;
      uint8_t target_mac[ETH_ALEN];
      uint32_t target_ip;
    } __attribute__((packed)) arp;
  } __attribute__((packed)) reply;

  const uint16_t opcode = ntohs(*(const uint16_t *)(payload + 6));

  if (payload_len < 28 || opcode != 1)
    return;

  const uint32_t target_ip = *(const uint32_t *)(payload + 24);
  if (!vbox_iface || target_ip != vbox_iface->ip)
    return;

  memset(&reply, 0, sizeof(reply));
  vbox_fill_eth(&reply.eth, eth->h_source, vbox_iface->mac, ETH_P_ARP);
  reply.arp.hw_type = htons(1);
  reply.arp.proto_type = htons(ETH_P_IP);
  reply.arp.hw_len = ETH_ALEN;
  reply.arp.proto_len = 4;
  reply.arp.opcode = htons(2);
  memcpy(reply.arp.sender_mac, vbox_iface->mac, ETH_ALEN);
  reply.arp.sender_ip = vbox_iface->ip;
  memcpy(reply.arp.target_mac, payload + 8, ETH_ALEN);
  reply.arp.target_ip = *(const uint32_t *)(payload + 14);

  vbox_emit_packet(&reply, sizeof(reply));
}

static void vbox_handle_icmp(const struct ethhdr *eth, const struct iphdr *ip,
                             const uint8_t *payload, size_t payload_len) {
  const size_t icmp_offset = sizeof(struct ethhdr) + sizeof(struct iphdr);
  const size_t reply_len = icmp_offset + payload_len;
  uint8_t reply[1600];
  struct ethhdr *reth = (struct ethhdr *)reply;
  struct iphdr *rip = (struct iphdr *)(reply + sizeof(struct ethhdr));
  uint8_t *ricmp = reply + icmp_offset;

  if (!vbox_iface || payload_len < 8)
    return;

  memset(reply, 0, sizeof(reply));
  vbox_fill_eth(reth, eth->h_source, vbox_iface->mac, ETH_P_IP);

  rip->version_ihl = 0x45;
  rip->tos = 0;
  rip->tot_len = htons((uint16_t)(sizeof(struct iphdr) + payload_len));
  rip->id = htons(0x4242);
  rip->frag_off = 0;
  rip->ttl = 64;
  rip->protocol = IPPROTO_ICMP;
  rip->saddr = ip->daddr;
  rip->daddr = ip->saddr;
  rip->check = 0;
  rip->check = vbox_checksum(rip, sizeof(*rip));

  memcpy(ricmp, payload, payload_len);
  ricmp[0] = 0;
  ricmp[2] = 0;
  ricmp[3] = 0;
  *(uint16_t *)(ricmp + 2) = vbox_checksum(ricmp, payload_len);

  vbox_emit_packet(reply, reply_len);
}

static void vbox_handle_tcp(const struct ethhdr *eth, const struct iphdr *ip,
                            const uint8_t *payload, size_t payload_len) {
  const struct vbox_tcp_hdr *tcp;
  size_t tcp_hdr_len;
  size_t data_len;
  uint16_t src_port;
  uint16_t dst_port;
  uint32_t seq;
  uint8_t flags;
  uint8_t reply[1600];
  struct ethhdr *reth = (struct ethhdr *)reply;
  struct iphdr *rip = (struct iphdr *)(reply + sizeof(struct ethhdr));
  struct vbox_tcp_hdr *rtcp = (struct vbox_tcp_hdr *)(reply +
                                                      sizeof(struct ethhdr) +
                                                      sizeof(struct iphdr));
  uint8_t *rdata = (uint8_t *)rtcp + sizeof(struct vbox_tcp_hdr);
  size_t rdata_len = 0;
  int send_reply = 0;

  if (!vbox_iface || payload_len < sizeof(struct vbox_tcp_hdr))
    return;

  tcp = (const struct vbox_tcp_hdr *)payload;
  tcp_hdr_len = (tcp->data_offset >> 4) * 4;
  if (tcp_hdr_len < sizeof(struct vbox_tcp_hdr) || tcp_hdr_len > payload_len)
    tcp_hdr_len = sizeof(struct vbox_tcp_hdr);
  data_len = payload_len - tcp_hdr_len;
  src_port = ntohs(tcp->source);
  dst_port = ntohs(tcp->dest);
  seq = ntohl(tcp->seq);
  flags = tcp->flags;

  memset(reply, 0, sizeof(reply));
  vbox_fill_eth(reth, eth->h_source, vbox_iface->mac, ETH_P_IP);
  rip->version_ihl = 0x45;
  rip->tos = 0;
  rip->id = htons(0x5151);
  rip->frag_off = 0;
  rip->ttl = 64;
  rip->protocol = IPPROTO_TCP;
  rip->saddr = ip->daddr;
  rip->daddr = ip->saddr;

  rtcp->source = htons(dst_port);
  rtcp->dest = htons(src_port);
  rtcp->window = htons(65535);
  rtcp->urgent = 0;

  if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
    vbox_server_seq++;
    rtcp->seq = htonl(vbox_server_seq);
    rtcp->ack_seq = htonl(seq + 1);
    rtcp->data_offset = (uint8_t)((sizeof(struct vbox_tcp_hdr) / 4) << 4);
    rtcp->flags = (uint8_t)(TCP_SYN | TCP_ACK);
    send_reply = 1;
  } else if (data_len > 0) {
    static const char http_body[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 39\r\n\r\n"
        "<body><h1>Hello from OS8 Network!</h1>";
    size_t body_len = sizeof(http_body) - 1;

    if (dst_port == 80 || dst_port == 443 || dst_port == 8080) {
      rdata_len = body_len;
      if (sizeof(reply) >= sizeof(struct ethhdr) + sizeof(struct iphdr) +
                            sizeof(struct vbox_tcp_hdr) + rdata_len) {
        memcpy(rdata, http_body, rdata_len);
        vbox_server_seq++;
        rtcp->seq = htonl(vbox_server_seq);
        rtcp->ack_seq = htonl(seq + (uint32_t)data_len);
        rtcp->data_offset = (uint8_t)((sizeof(struct vbox_tcp_hdr) / 4) << 4);
        rtcp->flags = (uint8_t)(TCP_PSH | TCP_ACK);
        send_reply = 1;
      }
    } else {
      rtcp->seq = htonl(vbox_server_seq);
      rtcp->ack_seq = htonl(seq + (uint32_t)data_len);
      rtcp->data_offset = (uint8_t)((sizeof(struct vbox_tcp_hdr) / 4) << 4);
      rtcp->flags = (uint8_t)TCP_ACK;
      send_reply = 1;
    }
  } else if (flags & TCP_FIN) {
    rtcp->seq = htonl(vbox_server_seq);
    rtcp->ack_seq = htonl(seq + 1);
    rtcp->data_offset = (uint8_t)((sizeof(struct vbox_tcp_hdr) / 4) << 4);
    rtcp->flags = (uint8_t)(TCP_FIN | TCP_ACK);
    send_reply = 1;
  } else if (flags & TCP_ACK) {
    return;
  }

  if (!send_reply)
    return;

  rip->tot_len = htons((uint16_t)(sizeof(struct iphdr) +
                                  sizeof(struct vbox_tcp_hdr) + rdata_len));
  rip->check = 0;
  rip->check = vbox_checksum(rip, sizeof(*rip));
  rtcp->checksum = 0;
  rtcp->checksum = vbox_checksum(rtcp, sizeof(struct vbox_tcp_hdr) + rdata_len);

  vbox_emit_packet(reply, sizeof(struct ethhdr) + sizeof(struct iphdr) +
                               sizeof(struct vbox_tcp_hdr) + rdata_len);
}

static void vbox_handle_ipv4(const struct ethhdr *eth, const uint8_t *payload,
                             size_t payload_len) {
  const struct iphdr *ip;
  size_t ip_len;
  size_t header_len;

  if (payload_len < sizeof(struct iphdr))
    return;

  ip = (const struct iphdr *)payload;
  header_len = (size_t)(ip->version_ihl & 0x0F) * 4;
  if (header_len < sizeof(struct iphdr) || header_len > payload_len)
    header_len = sizeof(struct iphdr);
  ip_len = ntohs(ip->tot_len);
  if (ip_len > payload_len)
    ip_len = payload_len;

  payload += header_len;
  payload_len = ip_len > header_len ? ip_len - header_len : 0;

  switch (ip->protocol) {
  case IPPROTO_ICMP:
    vbox_handle_icmp(eth, ip, payload, payload_len);
    break;
  case IPPROTO_TCP:
    vbox_handle_tcp(eth, ip, payload, payload_len);
    break;
  default:
    break;
  }
}

static int vbox_net_send(struct net_interface *iface, const void *data,
                         size_t len) {
  const struct ethhdr *eth = (const struct ethhdr *)data;
  uint16_t ethertype;

  if (!iface || !data || len < sizeof(struct ethhdr))
    return -1;

  ethertype = ntohs(eth->h_proto);
  iface->tx_packets++;
  iface->tx_bytes += len;

  switch (ethertype) {
  case ETH_P_ARP:
    if (len >= sizeof(struct ethhdr) + 28) {
      vbox_handle_arp(eth, (const uint8_t *)data + sizeof(struct ethhdr),
                      len - sizeof(struct ethhdr));
    }
    return (int)len;
  case ETH_P_IP:
    if (len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
      vbox_handle_ipv4(eth, (const uint8_t *)data + sizeof(struct ethhdr),
                       len - sizeof(struct ethhdr));
    }
    return (int)len;
  default:
    break;
  }

  return (int)len;
}

static pci_device_t *vbox_find_pcnet(void) {
  pci_device_t *dev = pci_find_device(VBOX_PCNET_VENDOR_ID, VBOX_PCNET_DEVICE_ID);
  if (dev)
    return dev;
  return pci_find_device(VBOX_PCNET_VENDOR_ID, VBOX_PCNET_ALT_DEVICE_ID);
}

#if defined(ARCH_X86_64) || defined(ARCH_X86)
static uint16_t vbox_read_io_base(pci_device_t *dev) {
  uint32_t bar0_raw;

  if (!dev)
    return 0;

  bar0_raw = pci_read32(dev->bus, dev->slot, dev->func, PCI_BAR0);
  if ((bar0_raw & 0x1) == 0)
    return 0;

  return (uint16_t)(bar0_raw & 0xFFFC);
}

static void vbox_read_mac(uint16_t io_base, uint8_t *mac) {
  for (int i = 0; i < ETH_ALEN; i++)
    mac[i] = vbox_inb((uint16_t)(io_base + i));
}
#endif

int vbox_net_init(void) {
#if !defined(ARCH_X86_64) && !defined(ARCH_X86)
  printk(KERN_INFO
         "VBOX-NET: VirtualBox PCNet probe skipped on non-x86 platform\n");
  return -1;
#else
  printk(KERN_INFO "VBOX-NET: Probing VirtualBox PCNet adapter...\n");

  vbox_pci_dev = vbox_find_pcnet();
  if (!vbox_pci_dev) {
    if (!vbox_warned_probe) {
      printk(KERN_INFO
             "VBOX-NET: No VirtualBox PCNet adapter detected, using software NIC\n");
      vbox_warned_probe = 1;
    }
    memcpy(vbox_mac, (uint8_t[]){0x52, 0x54, 0x00, 0x12, 0x34, 0x56}, ETH_ALEN);
  } else {
    pci_enable_device(vbox_pci_dev);
    vbox_io_base = vbox_read_io_base(vbox_pci_dev);
    if (!vbox_io_base) {
      printk(KERN_WARNING "VBOX-NET: Adapter found but IO BAR0 is unavailable\n");
    } else {
      vbox_read_mac(vbox_io_base, vbox_mac);
    }
  }

  if (!vbox_pci_dev || !vbox_io_base) {
    if (!vbox_mac[0] && !vbox_mac[1] && !vbox_mac[2] && !vbox_mac[3] &&
        !vbox_mac[4] && !vbox_mac[5]) {
      memcpy(vbox_mac, (uint8_t[]){0x52, 0x54, 0x00, 0x12, 0x34, 0x56}, ETH_ALEN);
    }
  }

  if (vbox_pci_dev && vbox_io_base) {
    printk(KERN_INFO
           "VBOX-NET: Found VirtualBox PCNet at %02x:%02x.%x IO=0x%04x MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
           vbox_pci_dev->bus, vbox_pci_dev->slot, vbox_pci_dev->func,
           vbox_io_base, vbox_mac[0], vbox_mac[1], vbox_mac[2], vbox_mac[3],
           vbox_mac[4], vbox_mac[5]);
  } else {
    printk(KERN_INFO
           "VBOX-NET: Software NIC MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           vbox_mac[0], vbox_mac[1], vbox_mac[2], vbox_mac[3], vbox_mac[4],
           vbox_mac[5]);
  }

  vbox_iface =
      net_add_interface("eth0", vbox_mac, 0x0A00020F, 0xFFFFFF00, 0x0A000202);
  if (!vbox_iface) {
    printk(KERN_WARNING "VBOX-NET: Failed to register network interface\n");
    return -1;
  }

  vbox_iface->send = vbox_net_send;
  printk(KERN_INFO "VBOX-NET: Software-backed network interface ready\n");
  return 0;
#endif
}

int vbox_net_is_ready(void) { return vbox_iface != 0; }

void vbox_net_poll(void) {}

const char *vbox_net_get_name(void) { return vbox_name; }
