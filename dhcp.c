#include "dhcp.h"
#include <string.h>

enum { OPTIONS=240 };

uint32_t dhcp_read32(const unsigned char *wire) {
    return (uint32_t)wire[0]<<24 | (uint32_t)wire[1]<<16 | (uint32_t)wire[2]<<8 | wire[3];
}

void dhcp_write32(unsigned char *wire,uint32_t value) {
    for (int index=0; index<4; ++index) wire[index]=(unsigned char)(value>>(24-index*8));
}

static unsigned char *option(unsigned char *wire,unsigned char code,uint32_t value) {
    if (!value) return wire;
    *wire++=code;
    *wire++=4;
    dhcp_write32(wire,value);
    return wire+4;
}

static int reply(unsigned char type) {
    return type==DHCP_OFFER || type==DHCP_ACK || type==DHCP_NAK;
}

size_t dhcp_encode(unsigned char wire[DHCP_WIRE_SIZE],const struct Dhcp *packet) {
    memset(wire,0,DHCP_WIRE_SIZE);
    wire[0]=reply(packet->type)?2:1;
    wire[1]=1; wire[2]=6; wire[10]=0x80;
    dhcp_write32(wire+4,packet->xid);
    dhcp_write32(wire+16,packet->address);
    memcpy(wire+28,packet->mac,sizeof packet->mac);
    dhcp_write32(wire+236,0x63825363);
    unsigned char *next=wire+OPTIONS;
    *next++=53; *next++=1; *next++=packet->type;
    next=option(next,50,packet->requested);
    next=option(next,54,packet->server);
    next=option(next,1,packet->mask);
    next=option(next,51,packet->lease);
    if (!reply(packet->type)) { *next++=55; *next++=1; *next++=1; }
    *next=255;
    return DHCP_WIRE_SIZE;
}

int dhcp_decode(struct Dhcp *packet,const unsigned char *wire,size_t size) {
    if (size<=OPTIONS || wire[1]!=1 || wire[2]!=6 ||
        (wire[0]==1 && (wire[3] || dhcp_read32(wire+24))) || dhcp_read32(wire+236)!=0x63825363) return 0;
    *packet=(struct Dhcp){0};
    packet->xid=dhcp_read32(wire+4);
    packet->address=dhcp_read32(wire+16);
    if (wire[0]==1 && !packet->address) packet->address=dhcp_read32(wire+12);
    memcpy(packet->mac,wire+28,sizeof packet->mac);
    for (size_t offset=OPTIONS; offset<size;) {
        unsigned char code=wire[offset++];
        if (!code) continue;
        if (code==255) return packet->type>=DHCP_DISCOVER && packet->type<=DHCP_NAK &&
                              wire[0]==(reply(packet->type)?2:1);
        if (offset==size) return 0;
        size_t length=wire[offset++];
        if (length>size-offset) return 0;
        if (code==53 && length==1) packet->type=wire[offset];
        if (length==4) {
            uint32_t value=dhcp_read32(wire+offset);
            switch (code) {
                case 1: packet->mask=value; break;
                case 50: packet->requested=value; break;
                case 51: packet->lease=value; break;
                case 54: packet->server=value; break;
            }
        }
        offset+=length;
    }
    return 0;
}
