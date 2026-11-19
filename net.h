#ifndef PACKET_PONG_NET_H
#define PACKET_PONG_NET_H

#include <stdint.h>

extern struct NetState {
    int link, host, move, state[6];
    uint32_t rtt;
} net;

int net_init(void);
void net_shutdown(void);
int net_open(uint32_t now);
void net_receive(uint32_t now,void (*chat_message)(const char *,uint32_t));
void net_send(uint32_t now,int move,const int state[6]);
int net_chat_send(const char *text,uint32_t now);

#endif
