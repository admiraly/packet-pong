#include "dhcp.h"
#include <assert.h>
#include <string.h>

int main(void) {
    unsigned char wire[DHCP_WIRE_SIZE];
    for (int type=DHCP_DISCOVER; type<=DHCP_NAK; ++type) {
        struct Dhcp outgoing={.xid=0x12345678,.mac={2,3,4,5,6,7},.type=type};
        if (type==DHCP_REQUEST) {
            outgoing.requested=DHCP_PEER;
            outgoing.server=DHCP_SERVER;
        }
        if (type==DHCP_OFFER || type==DHCP_ACK) {
            outgoing.address=DHCP_PEER;
            outgoing.server=DHCP_SERVER;
            outgoing.mask=0xffffff00;
            outgoing.lease=3600;
        }
        size_t size=dhcp_encode(wire,&outgoing);
        assert(size==DHCP_WIRE_SIZE && wire[4]==0x12 && wire[7]==0x78);
        assert(wire[10]==128 && wire[236]==99 && wire[239]==99);
        struct Dhcp incoming;
        assert(dhcp_decode(&incoming,wire,size));
        assert(incoming.type==type && incoming.xid==outgoing.xid);
        assert(!memcmp(incoming.mac,outgoing.mac,sizeof incoming.mac));
        assert(incoming.address==outgoing.address && incoming.requested==outgoing.requested);
        assert(incoming.server==outgoing.server && incoming.mask==outgoing.mask && incoming.lease==outgoing.lease);
        assert(!dhcp_decode(&incoming,wire,242));
        wire[0]=3-wire[0];
        assert(!dhcp_decode(&incoming,wire,size));
    }
    const unsigned char options[]={0,53,1,DHCP_ACK,199,2,1,2,54,4,10,77,0,1,0,255};
    memcpy(wire+240,options,sizeof options);
    wire[0]=2;
    struct Dhcp incoming;
    assert(dhcp_decode(&incoming,wire,240+sizeof options));
    assert(incoming.server==DHCP_SERVER);
    dhcp_write32(wire+24,0x0a4d0003);
    wire[3]=1;
    assert(dhcp_decode(&incoming,wire,240+sizeof options));
    assert(!dhcp_decode(&incoming,wire,240+sizeof options-1));
    wire[245]=255;
    assert(!dhcp_decode(&incoming,wire,sizeof wire));
    wire[236]=0;
    assert(!dhcp_decode(&incoming,wire,sizeof wire));
    return 0;
}
