/*
 * OS8 - VirtualBox Network Driver
 */

#ifndef DRIVERS_VBOX_NET_H
#define DRIVERS_VBOX_NET_H

int vbox_net_init(void);
int vbox_net_is_ready(void);
void vbox_net_poll(void);
const char *vbox_net_get_name(void);

#endif
