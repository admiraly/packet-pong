#include "chat.h"
#include <assert.h>
#include <string.h>

int main(void) {
    struct ChatPacket sent={UINT64_C(0x0123456789abcdef),UINT64_C(0xfedcba9876543210),0x10203040,{0}};
    struct ChatPacket received;
    unsigned char wire[CHAT_WIRE_MAX+1];
    memset(sent.text,'x',CHAT_LIMIT);
    size_t size=chat_encode(wire,&sent);
    assert(size==CHAT_WIRE_MAX);
    assert(!memcmp(wire,"PPC1\x01\x23\x45\x67\x89\xab\xcd\xef\xfe\xdc\xba\x98\x76\x54\x32\x10\x10\x20\x30\x40\x78",CHAT_HEADER));
    assert(chat_decode(&received,wire,size));
    assert(received.sender==sent.sender && received.target==sent.target && received.sequence==sent.sequence);
    assert(!strcmp(received.text,sent.text));
    assert(!chat_decode(&received,wire,size+1));
    assert(!chat_decode(&received,wire,size-1));
    assert(!chat_decode(&received,wire,CHAT_HEADER-1));
    wire[CHAT_HEADER]=0;
    assert(!chat_decode(&received,wire,size));
    wire[CHAT_HEADER]=0xff;
    assert(!chat_decode(&received,wire,size));
    sent.text[0]='\n';
    assert(!chat_encode(wire,&sent));
    memset(sent.text,'x',sizeof sent.text);
    assert(!chat_encode(wire,&sent));
    sent.text[0]=0;
    size=chat_encode(wire,&sent);
    assert(size==CHAT_HEADER && chat_decode(&received,wire,size) && !received.text[0]);
    wire[0]='X';
    assert(!chat_decode(&received,wire,size));
    sent.sequence=0;
    assert(!chat_encode(wire,&sent));
    return 0;
}
