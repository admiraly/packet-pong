#ifndef PACKET_PONG_LINK_H
#define PACKET_PONG_LINK_H

int link_helper(const char *parent,const char *adapter);
int link_prepare(void);
int link_needs_permission(void);
void link_start(void);
void link_update(void);
const char *link_status(void);
void link_stop(void);

#endif
