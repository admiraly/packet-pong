#include "chat.h"
#include <string.h>

static void put(unsigned char *wire,size_t size,uint64_t value) {
    while (size) { wire[--size]=(unsigned char)value; value>>=8; }
}

static uint64_t get(const unsigned char *wire,size_t size) {
    uint64_t value=0;
    while (size--) value=(value<<8)|*wire++;
    return value;
}

int chat_printable(unsigned char ch) { return ch>=32 && ch<=126; }

size_t chat_encode(unsigned char wire[CHAT_WIRE_MAX],const struct ChatPacket *packet) {
    if (!packet->sender || !packet->target || !packet->sequence) return 0;
    size_t length=0;
    while (length<=CHAT_LIMIT && packet->text[length]) {
        if (!chat_printable((unsigned char)packet->text[length])) return 0;
        ++length;
    }
    if (length>CHAT_LIMIT) return 0;
    memcpy(wire,"PPC1",4);
    put(wire+4,8,packet->sender);
    put(wire+12,8,packet->target);
    put(wire+20,4,packet->sequence);
    wire[24]=(unsigned char)length;
    memcpy(wire+CHAT_HEADER,packet->text,length);
    return CHAT_HEADER+length;
}

int chat_decode(struct ChatPacket *packet,const unsigned char *wire,size_t size) {
    if (size<CHAT_HEADER || size>CHAT_WIRE_MAX || memcmp(wire,"PPC1",4) ||
        wire[24]!=size-CHAT_HEADER) return 0;
    struct ChatPacket decoded={get(wire+4,8),get(wire+12,8),(uint32_t)get(wire+20,4),{0}};
    if (!decoded.sender || !decoded.target || !decoded.sequence) return 0;
    for (size_t index=CHAT_HEADER; index<size; ++index) {
        if (!chat_printable(wire[index])) return 0;
        decoded.text[index-CHAT_HEADER]=(char)wire[index];
    }
    *packet=decoded;
    return 1;
}
