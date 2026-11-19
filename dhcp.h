#ifndef PACKET_PONG_DHCP_H
#define PACKET_PONG_DHCP_H

#include <stddef.h>
#include <stdint.h>

enum { DHCP_DISCOVER=1, DHCP_OFFER, DHCP_REQUEST, DHCP_DECLINE, DHCP_ACK, DHCP_NAK };
enum { DHCP_WIRE_SIZE=300, DHCP_SERVER=0x0a4d0001, DHCP_PEER=0x0a4d0002 };

struct Dhcp {
    uint32_t xid, address, requested, server, mask, lease;
    unsigned char mac[6], type;
};

size_t dhcp_encode(unsigned char wire[DHCP_WIRE_SIZE],const struct Dhcp *packet);
int dhcp_decode(struct Dhcp *packet,const unsigned char *wire,size_t size);
uint32_t dhcp_read32(const unsigned char *wire);
void dhcp_write32(unsigned char *wire,uint32_t value);

#endif
