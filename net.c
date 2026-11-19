#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
typedef SOCKET Socket;
typedef int AddrLen;
#else
#define _DEFAULT_SOURCE
#include <sys/socket.h>
#include <sys/random.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <unistd.h>
typedef int Socket;
typedef socklen_t AddrLen;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#include "net.h"
#include "chat.h"
#include <stdlib.h>
#include <string.h>

enum { PORT=42874, BEACON_INTERVAL=500, TIMEOUT=2500, INTERVAL=16, WORDS=16 };
enum { MAGIC, FROM, FROM_HI, TO, TO_HI, SEQ, STAMP, ECHO, MOVE, X, Y, LEFT, RIGHT, SCORE_L, SCORE_R, HOST };
static const uint32_t magic=0x50504f33, beacon_magic=0x50504433;
static const struct in6_addr multicast={{ {0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,1} }};
static Socket discovery[2]={INVALID_SOCKET,INVALID_SOCKET};
static Socket gameplay[2]={INVALID_SOCKET,INVALID_SOCKET};
static struct sockaddr_storage peer;
static AddrLen peer_len;
static int route;
static uint64_t id, remote;
static uint32_t sequence, last_seq, seen, sent, echo, advertised;
static int received;
enum { CHAT_QUEUE=32, CHAT_RETRY=180, CHAT_TIMEOUT=8000 };
static struct ChatPacket chat_queue[CHAT_QUEUE];
static unsigned chat_head, chat_count;
static uint32_t chat_sequence, chat_seen, chat_sent, chat_started;
struct NetState net;

int net_init(void) {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2,2),&data)==0;
#else
    return 1;
#endif
}

static void reset_peer(void) {
    memset(&net,0,sizeof net);
    remote=last_seq=echo=0;
    received=0;
    chat_head=chat_count=chat_seen=0;
}

void net_shutdown(void) {
    for (int index=0; index<2; ++index) {
        if (discovery[index]!=INVALID_SOCKET) closesocket(discovery[index]);
        if (gameplay[index]!=INVALID_SOCKET) closesocket(gameplay[index]);
        discovery[index]=gameplay[index]=INVALID_SOCKET;
    }
    reset_peer();
#ifdef _WIN32
    WSACleanup();
#endif
}

static Socket make_socket(int family,int listener) {
    Socket fd=socket(family,SOCK_DGRAM,IPPROTO_UDP);
    if (fd==INVALID_SOCKET) return fd;
    int yes=1;
    if (listener && setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(const char *)&yes,sizeof yes)) goto fail;
    if (family==AF_INET6 && setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,(const char *)&yes,sizeof yes)) goto fail;
    if (family==AF_INET && !listener && setsockopt(fd,SOL_SOCKET,SO_BROADCAST,(const char *)&yes,sizeof yes)) goto fail;
#ifdef _WIN32
    u_long nonblock=1;
    if (ioctlsocket(fd,FIONBIO,&nonblock)) goto fail;
#else
    if (fcntl(fd,F_SETFL,O_NONBLOCK)<0) goto fail;
#endif
    if (family==AF_INET6) {
        struct sockaddr_in6 address={0};
        address.sin6_family=family;
        address.sin6_port=htons(listener?PORT:0);
        if (bind(fd,(const struct sockaddr *)&address,sizeof address)) goto fail;
    } else {
        struct sockaddr_in address={0};
        address.sin_family=family;
        address.sin_port=htons(listener?PORT:0);
        if (bind(fd,(const struct sockaddr *)&address,sizeof address)) goto fail;
    }
    return fd;
fail:
    closesocket(fd);
    return INVALID_SOCKET;
}

int net_open(uint32_t now) {
#ifdef _WIN32
    if (BCryptGenRandom(NULL,(unsigned char *)&id,sizeof id,BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return 0;
#else
    if (getrandom(&id,sizeof id,0)!=(ssize_t)sizeof id) return 0;
#endif
    if (!id) id=1;
    advertised=now-BEACON_INTERVAL;
    for (int index=0; index<2; ++index) {
        int family=index?AF_INET6:AF_INET;
        discovery[index]=make_socket(family,1);
        if (discovery[index]!=INVALID_SOCKET) gameplay[index]=make_socket(family,0);
    }
    return gameplay[0]!=INVALID_SOCKET || gameplay[1]!=INVALID_SOCKET;
}

static void beacon4(const uint32_t wire[3],uint32_t broadcast,unsigned int if_index) {
    if (gameplay[0]==INVALID_SOCKET) return;
    struct sockaddr_in target={0};
    target.sin_family=AF_INET;
    target.sin_port=htons(PORT);
    target.sin_addr.s_addr=broadcast;
#ifdef _WIN32
    DWORD index=htonl(if_index);
    if (setsockopt(gameplay[0],IPPROTO_IP,IP_UNICAST_IF,(const char *)&index,sizeof index)) return;
    sendto(gameplay[0],(const char *)wire,3*sizeof *wire,0,(const struct sockaddr *)&target,sizeof target);
    index=0;
    setsockopt(gameplay[0],IPPROTO_IP,IP_UNICAST_IF,(const char *)&index,sizeof index);
#else
    struct iovec data={(void *)wire,3*sizeof *wire};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(struct in_pktinfo))]; } control={0};
    struct msghdr message={0};
    message.msg_name=&target;
    message.msg_namelen=sizeof target;
    message.msg_iov=&data;
    message.msg_iovlen=1;
    message.msg_control=control.bytes;
    message.msg_controllen=sizeof control.bytes;
    struct cmsghdr *header=CMSG_FIRSTHDR(&message);
    header->cmsg_level=IPPROTO_IP;
    header->cmsg_type=IP_PKTINFO;
    header->cmsg_len=CMSG_LEN(sizeof(struct in_pktinfo));
    struct in_pktinfo info={0};
    info.ipi_ifindex=(int)if_index;
    memcpy(CMSG_DATA(header),&info,sizeof info);
    sendmsg(gameplay[0],&message,0);
#endif
}

static void beacon6(const uint32_t wire[3],unsigned int if_index) {
    if (!if_index || gameplay[1]==INVALID_SOCKET) return;
    struct ipv6_mreq group={0};
    group.ipv6mr_multiaddr=multicast;
    group.ipv6mr_interface=if_index;
    setsockopt(discovery[1],IPPROTO_IPV6,IPV6_JOIN_GROUP,(const char *)&group,sizeof group);
    struct sockaddr_in6 target={0};
    target.sin6_family=AF_INET6;
    target.sin6_port=htons(PORT);
    target.sin6_addr=multicast;
    target.sin6_scope_id=if_index;
    sendto(gameplay[1],(const char *)wire,3*sizeof *wire,0,(const struct sockaddr *)&target,sizeof target);
}

static void advertise(void) {
    uint32_t wire[]={htonl(beacon_magic),htonl((uint32_t)id),htonl((uint32_t)(id>>32))};
#ifdef _WIN32
    ULONG size=15000, result;
    IP_ADAPTER_ADDRESSES *list=NULL;
    for (int attempt=0; attempt<3; ++attempt) {
        free(list);
        list=malloc(size);
        if (!list) return;
        result=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
                                    NULL,list,&size);
        if (result!=ERROR_BUFFER_OVERFLOW) break;
    }
    if (result==NO_ERROR) for (const IP_ADAPTER_ADDRESSES *adapter=list; adapter; adapter=adapter->Next) {
        if (adapter->OperStatus!=IfOperStatusUp) continue;
        int link_local=0;
        for (const IP_ADAPTER_UNICAST_ADDRESS *entry=adapter->FirstUnicastAddress; entry; entry=entry->Next) {
            if (entry->DadState!=IpDadStatePreferred) continue;
            const struct sockaddr *address=entry->Address.lpSockaddr;
            if (address->sa_family==AF_INET6) {
                const struct sockaddr_in6 *ipv6=(const struct sockaddr_in6 *)address;
                if (IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr)) link_local=1;
            } else if (address->sa_family==AF_INET && entry->OnLinkPrefixLength>0 && entry->OnLinkPrefixLength<31) {
                const struct sockaddr_in *ipv4=(const struct sockaddr_in *)address;
                uint32_t host_mask=UINT32_MAX>>entry->OnLinkPrefixLength;
                if (ipv4->sin_addr.s_addr) beacon4(wire,ipv4->sin_addr.s_addr|htonl(host_mask),adapter->IfIndex);
            }
        }
        if (link_local && !(adapter->Flags&IP_ADAPTER_NO_MULTICAST)) beacon6(wire,adapter->Ipv6IfIndex);
    }
    free(list);
#else
    struct ifaddrs *list;
    if (getifaddrs(&list)) return;
    for (const struct ifaddrs *entry=list; entry; entry=entry->ifa_next) {
        if (!entry->ifa_addr || !(entry->ifa_flags&IFF_UP)) continue;
        if (entry->ifa_addr->sa_family==AF_INET6 && (entry->ifa_flags&IFF_MULTICAST)) {
            const struct sockaddr_in6 *ipv6=(const struct sockaddr_in6 *)entry->ifa_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr)) beacon6(wire,if_nametoindex(entry->ifa_name));
        } else if (entry->ifa_addr->sa_family==AF_INET && (entry->ifa_flags&IFF_BROADCAST) && entry->ifa_broadaddr) {
            const struct sockaddr_in *broadcast=(const struct sockaddr_in *)entry->ifa_broadaddr;
            beacon4(wire,broadcast->sin_addr.s_addr,if_nametoindex(entry->ifa_name));
        }
    }
    freeifaddrs(list);
#endif
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

static void select_peer(uint64_t candidate,const struct sockaddr_storage *source,AddrLen length,int index,uint32_t now) {
    remote=candidate;
    peer=*source;
    peer_len=length;
    route=index;
    seen=now;
    sent=now-INTERVAL;
    net.host=id<remote;
}

static void receive_beacons(uint32_t now,int index) {
    for (int count=0; count<64; ++count) {
        struct sockaddr_storage source={0};
        AddrLen length=sizeof source;
        uint32_t wire[4];
        int bytes=recvfrom(discovery[index],(char *)wire,sizeof wire,0,(struct sockaddr *)&source,&length);
        if (bytes<0) break;
        if (remote || bytes!=3*(int)sizeof *wire || ntohl(wire[0])!=beacon_magic) continue;
        uint64_t candidate=((uint64_t)ntohl(wire[2])<<32)|ntohl(wire[1]);
        if (id<candidate) select_peer(candidate,&source,length,index,now);
    }
}

static void send_chat(const struct ChatPacket *packet) {
    unsigned char wire[CHAT_WIRE_MAX];
    size_t size=chat_encode(wire,packet);
    if (size) sendto(gameplay[route],(const char *)wire,(int)size,0,(const struct sockaddr *)&peer,peer_len);
}

int net_chat_send(const char *text,uint32_t now) {
    if (!net.link || chat_count==CHAT_QUEUE || !*text || strlen(text)>CHAT_LIMIT) return 0;
    for (const char *cursor=text; *cursor; ++cursor)
        if (!chat_printable((unsigned char)*cursor)) return 0;
    if (!++chat_sequence) ++chat_sequence;
    struct ChatPacket *packet=&chat_queue[(chat_head+chat_count)%CHAT_QUEUE];
    *packet=(struct ChatPacket){id,remote,chat_sequence,{0}};
    strcpy(packet->text,text);
    if (!chat_count++) { chat_started=now; chat_sent=now-CHAT_RETRY; }
    return 1;
}

static void pop_chat(uint32_t now) {
    chat_head=(chat_head+1)%CHAT_QUEUE;
    --chat_count;
    chat_started=now;
    chat_sent=now-CHAT_RETRY;
}

static void receive_chat(const struct ChatPacket *packet,uint32_t now,
                         void (*chat_message)(const char *,uint32_t)) {
    if (!packet->text[0]) {
        if (chat_count && packet->sequence==chat_queue[chat_head].sequence) pop_chat(now);
        return;
    }
    if (!chat_seen || (int32_t)(packet->sequence-chat_seen)>0) {
        chat_seen=packet->sequence;
        if (chat_message) chat_message(packet->text,now);
    }
    struct ChatPacket ack={id,remote,packet->sequence,{0}};
    send_chat(&ack);
}

void net_receive(uint32_t now,void (*chat_message)(const char *,uint32_t)) {
    if (remote && now-seen>=TIMEOUT) {
        reset_peer();
        advertised=now-BEACON_INTERVAL;
    }
    if (!remote && now-advertised>=BEACON_INTERVAL) {
        advertised=now;
        advertise();
    }
    for (int index=0; index<2; ++index) {
        if (gameplay[index]==INVALID_SOCKET) continue;
        for (int count=0; count<64; ++count) {
            struct sockaddr_storage source={0};
            AddrLen length=sizeof source;
            unsigned char datagram[CHAT_WIRE_MAX+1];
            int bytes=recvfrom(gameplay[index],(char *)datagram,sizeof datagram,0,(struct sockaddr *)&source,&length);
            if (bytes<0) break;
            if (bytes>=4 && !memcmp(datagram,"PPC1",4)) {
                struct ChatPacket packet;
                if (net.link && index==route && same_peer(&source) &&
                    chat_decode(&packet,datagram,(size_t)bytes) && packet.sender==remote && packet.target==id)
                    receive_chat(&packet,now,chat_message);
                continue;
            }
            uint32_t wire[WORDS];
            if (bytes!=WORDS*(int)sizeof *wire) continue;
            memcpy(wire,datagram,sizeof wire);
            for (int word=0; word<WORDS; ++word) wire[word]=ntohl(wire[word]);
            uint64_t sender=((uint64_t)wire[FROM_HI]<<32)|wire[FROM];
            uint64_t target=((uint64_t)wire[TO_HI]<<32)|wire[TO];
            if (wire[MAGIC]!=magic || !sender || sender==id || target!=id ||
                wire[HOST]!=(uint32_t)(sender<id) || wire[MOVE]>2) continue;
            if (remote) {
                if (sender!=remote || !same_peer(&source) || (received && (int32_t)(wire[SEQ]-last_seq)<=0)) continue;
            } else if (sender>id) continue;
            if (sender<id && ((wire[X]>48000 && wire[X]<UINT32_MAX-499) || wire[Y]>27000 ||
                wire[LEFT]>270 || wire[RIGHT]>270 || wire[SCORE_L]>99 || wire[SCORE_R]>99)) continue;
            if (!remote) select_peer(sender,&source,length,index,now);
            received=net.link=1;
            last_seq=wire[SEQ]; seen=now; echo=wire[STAMP];
            if (wire[ECHO] && now-wire[ECHO]<TIMEOUT) net.rtt=now-wire[ECHO];
            net.move=(int)wire[MOVE]-1;
            if (!net.host) for (int field=0; field<6; ++field) net.state[field]=(int32_t)wire[X+field];
        }
        if (discovery[index]!=INVALID_SOCKET) receive_beacons(now,index);
    }
}

void net_send(uint32_t now,int move,const int state[6]) {
    if (chat_count && now-chat_started>=CHAT_TIMEOUT) pop_chat(now);
    if (chat_count && now-chat_sent>=CHAT_RETRY) {
        chat_sent=now;
        send_chat(&chat_queue[chat_head]);
    }
    if (!remote || now-sent<INTERVAL) return;
    sent=now;
    uint32_t wire[WORDS]={magic,(uint32_t)id,(uint32_t)(id>>32),(uint32_t)remote,(uint32_t)(remote>>32),
                          ++sequence,now,echo,(uint32_t)(move+1)};
    wire[HOST]=net.host;
    if (net.host) for (int field=0; field<6; ++field) wire[X+field]=(uint32_t)state[field];
    for (int word=0; word<WORDS; ++word) wire[word]=htonl(wire[word]);
    sendto(gameplay[route],(const char *)wire,sizeof wire,0,(const struct sockaddr *)&peer,peer_len);
}
