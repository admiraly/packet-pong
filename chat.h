#ifndef PACKET_PONG_CHAT_H
#define PACKET_PONG_CHAT_H

#include <stddef.h>
#include <stdint.h>

enum { CHAT_LIMIT=120, CHAT_HEADER=25, CHAT_WIRE_MAX=CHAT_HEADER+CHAT_LIMIT };

struct ChatPacket {
    uint64_t sender, target;
    uint32_t sequence;
    char text[CHAT_LIMIT+1];
};

int chat_printable(unsigned char ch);
size_t chat_encode(unsigned char wire[CHAT_WIRE_MAX],const struct ChatPacket *packet);
int chat_decode(struct ChatPacket *packet,const unsigned char *wire,size_t size);

#endif
