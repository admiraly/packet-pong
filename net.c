#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Socket;
typedef int AddrLen;
#else
#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
typedef int Socket;
typedef socklen_t AddrLen;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#include "net.h"
#include <string.h>

enum { TIMEOUT=2500, INTERVAL=16, WORDS=14 };
enum { MAGIC, FROM, TO, SEQ, STAMP, ECHO, MOVE, X, Y, LEFT, RIGHT, SCORE_L, SCORE_R, HOST };
static const uint32_t magic=0x50504f32;
static const char port[]="42874";
static Socket sockets[2]={INVALID_SOCKET,INVALID_SOCKET};
static struct sockaddr_storage peer;
static AddrLen peer_len;
static int host, route;
static uint32_t id, remote, sequence, last_seq, seen, sent, echo;
struct NetState net;

int net_init(void) {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2,2),&data)==0;
#else
    return 1;
#endif
}

void net_shutdown(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

void net_close(void) {
    for (int index=0; index<2; ++index) {
        if (sockets[index]!=INVALID_SOCKET) closesocket(sockets[index]);
        sockets[index]=INVALID_SOCKET;
    }
    memset(&net,0,sizeof net);
    remote=sequence=last_seq=echo=0;
}

static Socket make_socket(const struct addrinfo *info,int listening) {
    Socket fd=socket(info->ai_family,SOCK_DGRAM,IPPROTO_UDP);
    if (fd==INVALID_SOCKET) return fd;
    int yes=1;
    if (info->ai_family==AF_INET6 && setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,(const char *)&yes,sizeof yes)) goto fail;
#ifdef _WIN32
    u_long nonblock=1;
    if (ioctlsocket(fd,FIONBIO,&nonblock)) goto fail;
#else
    if (fcntl(fd,F_SETFL,O_NONBLOCK)<0) goto fail;
#endif
    if (listening && bind(fd,info->ai_addr,(AddrLen)info->ai_addrlen)) goto fail;
    return fd;
fail:
    closesocket(fd);
    return INVALID_SOCKET;
}

int net_open(const char *ip,uint64_t seed,uint32_t now) {
    net_close();
    struct addrinfo hints={0}, *list;
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_DGRAM;
    hints.ai_protocol=IPPROTO_UDP;
    hints.ai_flags=ip?AI_NUMERICHOST:AI_PASSIVE;
    if (getaddrinfo(ip,port,&hints,&list)) return 0;
    host=!ip;
    id=(uint32_t)(seed^(seed>>32))|1;
    sent=now-INTERVAL;
    for (const struct addrinfo *info=list; info; info=info->ai_next) {
        int index=info->ai_family==AF_INET6;
        if ((info->ai_family!=AF_INET && !index) || sockets[index]!=INVALID_SOCKET) continue;
        if (ip && index) {
            const struct sockaddr_in6 *address=(const struct sockaddr_in6 *)info->ai_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr) && !address->sin6_scope_id) continue;
        }
        sockets[index]=make_socket(info,host);
        if (ip && sockets[index]!=INVALID_SOCKET) {
            peer_len=(AddrLen)info->ai_addrlen;
            memcpy(&peer,info->ai_addr,peer_len);
            route=index;
            break;
        }
    }
    freeaddrinfo(list);
    return sockets[0]!=INVALID_SOCKET || sockets[1]!=INVALID_SOCKET;
}

static int same_peer(const struct sockaddr_storage *source) {
    if (source->ss_family!=peer.ss_family) return 0;
    if (source->ss_family==AF_INET) {
        const struct sockaddr_in *addr=(const struct sockaddr_in *)source;
        const struct sockaddr_in *other=(const struct sockaddr_in *)&peer;
        return addr->sin_port==other->sin_port && addr->sin_addr.s_addr==other->sin_addr.s_addr;
    }
    const struct sockaddr_in6 *addr=(const struct sockaddr_in6 *)source;
    const struct sockaddr_in6 *other=(const struct sockaddr_in6 *)&peer;
    return addr->sin6_port==other->sin6_port && addr->sin6_scope_id==other->sin6_scope_id &&
           !memcmp(&addr->sin6_addr,&other->sin6_addr,sizeof addr->sin6_addr);
}

void net_receive(uint32_t now) {
    if (remote && now-seen>=TIMEOUT) {
        remote=last_seq=echo=0;
        memset(&net,0,sizeof net);
    }
    for (int index=0; index<2; ++index) {
        if (sockets[index]==INVALID_SOCKET) continue;
        for (int count=0; count<64; ++count) {
            struct sockaddr_storage source={0};
            AddrLen length=sizeof source;
            uint32_t wire[WORDS+1];
            int bytes=recvfrom(sockets[index],(char *)wire,sizeof wire,0,(struct sockaddr *)&source,&length);
            if (bytes<0) break;
            if (bytes!=WORDS*(int)sizeof *wire) continue;
            for (int word=0; word<WORDS; ++word) wire[word]=ntohl(wire[word]);
            if (wire[MAGIC]!=magic || !wire[FROM] || wire[HOST]!=(uint32_t)!host || wire[MOVE]>2 ||
                (wire[TO]!=id && (!host || wire[TO]))) continue;
            if ((!host || remote) && !same_peer(&source)) continue;
            if (remote && (wire[FROM]!=remote || (int32_t)(wire[SEQ]-last_seq)<=0)) continue;
            if (!host && ((wire[X]>48000 && wire[X]<UINT32_MAX-499) || wire[Y]>27000 ||
                wire[LEFT]>270 || wire[RIGHT]>270 || wire[SCORE_L]>99 || wire[SCORE_R]>99)) continue;
            if (host && !remote) { peer=source; peer_len=length; route=index; }
            remote=wire[FROM]; last_seq=wire[SEQ]; seen=now; echo=wire[STAMP];
            if (wire[TO]==id) net.link=1;
            if (wire[ECHO] && now-wire[ECHO]<TIMEOUT) net.rtt=now-wire[ECHO];
            net.move=(int)wire[MOVE]-1;
            if (!host) for (int field=0; field<6; ++field) net.state[field]=(int32_t)wire[X+field];
        }
    }
}

void net_send(uint32_t now,int move,const int state[6]) {
    if ((host && !remote) || now-sent<INTERVAL) return;
    sent=now;
    uint32_t wire[WORDS]={magic,id,remote,++sequence,now,echo,(uint32_t)(move+1)};
    wire[HOST]=host;
    if (host) for (int field=0; field<6; ++field) wire[X+field]=(uint32_t)state[field];
    for (int word=0; word<WORDS; ++word) wire[word]=htonl(wire[word]);
    sendto(sockets[route],(const char *)wire,sizeof wire,0,(const struct sockaddr *)&peer,peer_len);
}
