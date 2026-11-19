#ifndef PACKET_PONG_NET_H
#define PACKET_PONG_NET_H

#include <stdint.h>

extern struct NetState {
    int link, move, state[6];
    uint32_t rtt;
} net;

int net_init(void);
void net_shutdown(void);
int net_open(const char *ip,uint64_t seed,uint32_t now);
void net_close(void);
void net_receive(uint32_t now);
void net_send(uint32_t now,int move,const int state[6]);

#endif
