#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <shellapi.h>
#include <bcrypt.h>
typedef SOCKET Socket;
#else
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <netpacket/packet.h>
#include <net/if.h>
#include <linux/wireless.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
typedef int Socket;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#include "dhcp.h"
#include "link.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IDLE, PROBE, REQUEST, SERVER, CLIENT };
enum { SERVER_PORT=67, CLIENT_PORT=68, LEASE=3600, PROBE_MS=750, RETRY_MS=200 };
static struct {
    unsigned int index;
    unsigned char mac[6];
#ifndef _WIN32
    char name[IF_NAMESIZE];
#endif
} adapter;
static struct {
    int phase, prefix;
    uint32_t xid, address;
    uint64_t next, deadline, leased_until;
    struct Dhcp offer, pending;
    unsigned char lease_mac[6];
} setup;
static Socket client=INVALID_SOCKET, server=INVALID_SOCKET;
static char status[32];
static uint64_t next_status;
#ifdef _WIN32
static LPFN_WSARECVMSG receive_message;
static HANDLE child;
#else
static Socket packet_socket=INVALID_SOCKET;
static pid_t child;
static int child_pipe=-1;
static volatile sig_atomic_t stopping;

static void stop_signal(int signal_number) {
    (void)signal_number;
    stopping=1;
}
#endif

static uint64_t ticks(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec stamp;
    clock_gettime(CLOCK_MONOTONIC,&stamp);
    return (uint64_t)stamp.tv_sec*1000+stamp.tv_nsec/1000000;
#endif
}

static int usable(uint32_t address) {
    return (address>>24)>0 && (address>>24)!=127 && (address>>24)<224 &&
           (address>>16)!=0xa9fe;
}

#ifndef _WIN32
static int wired(const char *name) {
    char path[128];
    snprintf(path,sizeof path,"/sys/class/net/%s/wireless",name);
    if (!access(path,F_OK)) return 0;
    snprintf(path,sizeof path,"/sys/class/net/%s/phy80211",name);
    if (!access(path,F_OK)) return 0;
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if (fd<0) return 0;
    struct iwreq request={0};
    snprintf(request.ifr_name,sizeof request.ifr_name,"%s",name);
    int wireless=ioctl(fd,SIOCGIWNAME,&request)==0;
    close(fd);
    return !wireless;
}
#endif

static int find_adapter(unsigned int wanted,int select,uint32_t *address) {
    int found=0;
    *address=0;
#ifdef _WIN32
    ULONG size=15000, result=ERROR_BUFFER_OVERFLOW;
    IP_ADAPTER_ADDRESSES *list=NULL;
    for (int attempt=0; attempt<3 && result==ERROR_BUFFER_OVERFLOW; ++attempt) {
        free(list);
        list=malloc(size);
        if (!list) return 0;
        result=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_ALL_INTERFACES,NULL,list,&size);
    }
    if (result==NO_ERROR) for (const IP_ADAPTER_ADDRESSES *entry=list; entry; entry=entry->Next) {
        if (!entry->IfIndex || (wanted && wanted!=entry->IfIndex) || entry->IfType!=IF_TYPE_ETHERNET_CSMACD ||
            entry->OperStatus!=IfOperStatusUp || entry->PhysicalAddressLength!=6) continue;
        uint32_t existing=0;
        for (const IP_ADAPTER_UNICAST_ADDRESS *item=entry->FirstUnicastAddress; item; item=item->Next) {
            if (item->Address.lpSockaddr->sa_family!=AF_INET) continue;
            uint32_t value=ntohl(((const struct sockaddr_in *)item->Address.lpSockaddr)->sin_addr.s_addr);
            if (usable(value)) { existing=value; break; }
        }
        if (select && existing) continue;
        if (select) {
            adapter.index=entry->IfIndex;
            memcpy(adapter.mac,entry->PhysicalAddress,sizeof adapter.mac);
        }
        *address=existing;
        found=1;
        break;
    }
    free(list);
#else
    struct ifaddrs *list;
    if (getifaddrs(&list)) return 0;
    for (const struct ifaddrs *entry=list; entry; entry=entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family!=AF_PACKET ||
            !(entry->ifa_flags&IFF_UP) || !(entry->ifa_flags&IFF_LOWER_UP) ||
            (entry->ifa_flags&IFF_LOOPBACK)) continue;
        const struct sockaddr_ll *hardware=(const struct sockaddr_ll *)entry->ifa_addr;
        if (!hardware->sll_ifindex || (wanted && wanted!=(unsigned int)hardware->sll_ifindex) ||
            hardware->sll_hatype!=1 || hardware->sll_halen!=6 || !wired(entry->ifa_name)) continue;
        uint32_t existing=0;
        for (const struct ifaddrs *item=list; item; item=item->ifa_next) {
            if (!item->ifa_addr || item->ifa_addr->sa_family!=AF_INET || strcmp(item->ifa_name,entry->ifa_name)) continue;
            uint32_t value=ntohl(((const struct sockaddr_in *)item->ifa_addr)->sin_addr.s_addr);
            if (usable(value)) { existing=value; break; }
        }
        if (select && existing) continue;
        if (select) {
            adapter.index=(unsigned int)hardware->sll_ifindex;
            memcpy(adapter.mac,hardware->sll_addr,sizeof adapter.mac);
            snprintf(adapter.name,sizeof adapter.name,"%s",entry->ifa_name);
        }
        *address=existing;
        found=1;
        break;
    }
    freeifaddrs(list);
#endif
    return found;
}

static int change_address(int add,uint32_t address,int prefix) {
#ifdef _WIN32
    MIB_UNICASTIPADDRESS_ROW row;
    InitializeUnicastIpAddressEntry(&row);
    row.InterfaceIndex=adapter.index;
    row.Address.Ipv4.sin_family=AF_INET;
    row.Address.Ipv4.sin_addr.s_addr=htonl(address);
    row.OnLinkPrefixLength=(UINT8)prefix;
    row.PrefixOrigin=IpPrefixOriginManual;
    row.SuffixOrigin=IpSuffixOriginManual;
    return (add?CreateUnicastIpAddressEntry(&row):DeleteUnicastIpAddressEntry(&row))==NO_ERROR;
#else
    char cidr[24], name[IF_NAMESIZE];
    if (!if_indextoname(adapter.index,name)) return 0;
    snprintf(cidr,sizeof cidr,"%u.%u.%u.%u/%d",address>>24,(address>>16)&255,(address>>8)&255,address&255,prefix);
    pid_t process=fork();
    if (process==0) {
        int quiet=open("/dev/null",O_WRONLY);
        if (quiet>=0) dup2(quiet,STDERR_FILENO);
        const char *action=add?"add":"del";
        execl("/usr/sbin/ip","ip","addr",action,cidr,"broadcast","+","dev",name,(char *)NULL);
        execl("/sbin/ip","ip","addr",action,cidr,"broadcast","+","dev",name,(char *)NULL);
        _exit(127);
    }
    if (process<0) return 0;
    int result;
    pid_t waited;
    do { waited=waitpid(process,&result,0); } while (waited<0 && errno==EINTR);
    return waited==process && WIFEXITED(result) && WEXITSTATUS(result)==0;
#endif
}

static Socket open_port(unsigned short port) {
#ifdef _WIN32
    Socket fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
#else
    Socket fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,IPPROTO_UDP);
#endif
    if (fd==INVALID_SOCKET) return fd;
    struct sockaddr_in local={0};
    local.sin_family=AF_INET;
    local.sin_port=htons(port);
#ifdef _WIN32
    int yes=1;
    u_long nonblock=1;
    DWORD bytes;
    GUID guid=WSAID_WSARECVMSG;
    if (setsockopt(fd,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char *)&yes,sizeof yes) ||
        setsockopt(fd,SOL_SOCKET,SO_BROADCAST,(const char *)&yes,sizeof yes) ||
        setsockopt(fd,IPPROTO_IP,IP_PKTINFO,(const char *)&yes,sizeof yes) ||
        ioctlsocket(fd,FIONBIO,&nonblock) ||
        WSAIoctl(fd,SIO_GET_EXTENSION_FUNCTION_POINTER,&guid,sizeof guid,
                 &receive_message,sizeof receive_message,&bytes,NULL,NULL)) goto fail;
#else
    if (setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,adapter.name,strlen(adapter.name)+1)) goto fail;
#endif
    if (!bind(fd,(const struct sockaddr *)&local,sizeof local)) return fd;
fail:
    closesocket(fd);
    return INVALID_SOCKET;
}

#ifndef _WIN32
static void write16(unsigned char *wire,unsigned int value) {
    wire[0]=(unsigned char)(value>>8); wire[1]=(unsigned char)value;
}

static unsigned int read16(const unsigned char *wire) {
    return (unsigned int)wire[0]<<8 | wire[1];
}

static unsigned int checksum(const unsigned char *wire,size_t size,unsigned int sum) {
    for (; size>=2; size-=2,wire+=2) sum+=read16(wire);
    if (size) sum+=(unsigned int)wire[0]<<8;
    while (sum>>16) sum=(sum&65535)+(sum>>16);
    return (~sum)&65535;
}
#endif

static int transmit(const struct Dhcp *message,int is_server) {
#ifdef _WIN32
    unsigned char wire[DHCP_WIRE_SIZE];
    WSABUF data={(ULONG)dhcp_encode(wire,message),(char *)wire};
    struct sockaddr_in target={0};
    target.sin_family=AF_INET;
    target.sin_port=htons(is_server?CLIENT_PORT:SERVER_PORT);
    target.sin_addr.s_addr=INADDR_BROADCAST;
    union { WSACMSGHDR align; char bytes[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))]; } control={0};
    WSAMSG outgoing={0};
    outgoing.name=(struct sockaddr *)&target;
    outgoing.namelen=sizeof target;
    outgoing.lpBuffers=&data;
    outgoing.dwBufferCount=1;
    outgoing.Control.buf=control.bytes;
    outgoing.Control.len=sizeof control.bytes;
    WSACMSGHDR *header=WSA_CMSG_FIRSTHDR(&outgoing);
    header->cmsg_level=IPPROTO_IP;
    header->cmsg_type=IP_PKTINFO;
    header->cmsg_len=WSA_CMSG_LEN(sizeof(IN_PKTINFO));
    IN_PKTINFO info={0};
    info.ipi_ifindex=adapter.index;
    if (is_server) info.ipi_addr.s_addr=htonl(DHCP_SERVER);
    memcpy(WSA_CMSG_DATA(header),&info,sizeof info);
    DWORD sent;
    return !WSASendMsg(is_server?server:client,&outgoing,0,&sent,NULL,NULL) && sent==data.len;
#else
    unsigned char wire[28+DHCP_WIRE_SIZE]={0x45};
    size_t size=28+dhcp_encode(wire+28,message);
    write16(wire+2,(unsigned int)size);
    wire[8]=64; wire[9]=IPPROTO_UDP;
    dhcp_write32(wire+12,is_server?DHCP_SERVER:0);
    dhcp_write32(wire+16,UINT32_MAX);
    write16(wire+10,checksum(wire,20,0));
    write16(wire+20,is_server?SERVER_PORT:CLIENT_PORT);
    write16(wire+22,is_server?CLIENT_PORT:SERVER_PORT);
    write16(wire+24,(unsigned int)size-20);
    struct sockaddr_ll target={0};
    target.sll_family=AF_PACKET;
    target.sll_protocol=htons(ETH_P_IP);
    target.sll_ifindex=(int)adapter.index;
    target.sll_halen=6;
    memset(target.sll_addr,255,6);
    return sendto(packet_socket,wire,size,0,(const struct sockaddr *)&target,sizeof target)==(ssize_t)size;
#endif
}

static int receive(struct Dhcp *packet,int is_server) {
    unsigned char wire[1500];
#ifdef _WIN32
    struct sockaddr_in source;
    union { WSACMSGHDR align; char bytes[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))]; } control;
    WSABUF data={sizeof wire,(char *)wire};
    WSAMSG message={0};
    message.name=(struct sockaddr *)&source;
    message.namelen=sizeof source;
    message.lpBuffers=&data;
    message.dwBufferCount=1;
    message.Control.buf=control.bytes;
    message.Control.len=sizeof control.bytes;
    DWORD size;
    if (receive_message(is_server?server:client,&message,&size,NULL,NULL)) return -1;
    if (message.dwFlags&(MSG_TRUNC|MSG_CTRUNC) || ntohs(source.sin_port)!=(is_server?CLIENT_PORT:SERVER_PORT)) return 0;
    for (WSACMSGHDR *header=WSA_CMSG_FIRSTHDR(&message); header; header=WSA_CMSG_NXTHDR(&message,header)) {
        if (header->cmsg_level!=IPPROTO_IP || header->cmsg_type!=IP_PKTINFO ||
            header->cmsg_len<WSA_CMSG_LEN(sizeof(IN_PKTINFO))) continue;
        IN_PKTINFO info;
        memcpy(&info,WSA_CMSG_DATA(header),sizeof info);
        if (info.ipi_ifindex==adapter.index) return dhcp_decode(packet,wire,size);
    }
    return 0;
#else
    struct sockaddr_ll source;
    socklen_t length=sizeof source;
    ssize_t size=recvfrom(packet_socket,wire,sizeof wire,0,(struct sockaddr *)&source,&length);
    if (size<0) return -1;
    if (size<28 || source.sll_pkttype==PACKET_OUTGOING || wire[0]>>4!=4 || wire[9]!=IPPROTO_UDP ||
        (read16(wire+6)&0x3fff)) return 0;
    unsigned int header=(wire[0]&15)*4, total=read16(wire+2);
    if (header<20 || total<header+8 || total>(size_t)size || checksum(wire,header,0)) return 0;
    const unsigned char *udp=wire+header;
    unsigned int udp_size=read16(udp+4);
    int from_client=read16(udp)==CLIENT_PORT && read16(udp+2)==SERVER_PORT;
    int from_server=read16(udp)==SERVER_PORT && read16(udp+2)==CLIENT_PORT;
    if (udp_size<8 || udp_size!=total-header || (is_server?!from_client:(!from_client && !from_server))) return 0;
    if (read16(udp+6) && checksum(udp,udp_size,read16(wire+12)+read16(wire+14)+
        read16(wire+16)+read16(wire+18)+IPPROTO_UDP+udp_size)) return 0;
    return dhcp_decode(packet,udp+8,udp_size-8) &&
           (from_client==(packet->type==DHCP_DISCOVER || packet->type==DHCP_REQUEST || packet->type==DHCP_DECLINE));
#endif
}

static void close_ports(void) {
    if (client!=INVALID_SOCKET) closesocket(client);
    if (server!=INVALID_SOCKET) closesocket(server);
    client=server=INVALID_SOCKET;
#ifndef _WIN32
    if (packet_socket!=INVALID_SOCKET) close(packet_socket);
    packet_socket=INVALID_SOCKET;
#endif
}

static int assign(uint32_t address,int prefix) {
    uint32_t existing;
    if (!find_adapter(adapter.index,0,&existing) || existing || !change_address(1,address,prefix)) return 0;
    setup.address=address;
    setup.prefix=prefix;
    return 1;
}

static void local_stop(void) {
    close_ports();
    if (setup.address) change_address(0,setup.address,setup.prefix);
    setup.address=0;
    setup.phase=IDLE;
}

static int prefix_length(uint32_t mask) {
    int prefix=0;
    while (mask&0x80000000) { ++prefix; mask<<=1; }
    return mask?0:prefix;
}

static void answer(const struct Dhcp *request,uint64_t now) {
    if (!memcmp(request->mac,adapter.mac,6) ||
        (now<setup.leased_until && memcmp(request->mac,setup.lease_mac,6))) return;
    struct Dhcp reply=*request;
    reply.address=DHCP_PEER;
    reply.requested=0;
    reply.server=DHCP_SERVER;
    reply.mask=0xffffff00;
    reply.lease=LEASE;
    if (request->type==DHCP_DISCOVER) reply.type=DHCP_OFFER;
    else if (request->type==DHCP_REQUEST && (!request->server || request->server==DHCP_SERVER) &&
             (request->requested?request->requested:request->address)==DHCP_PEER) {
        reply.type=DHCP_ACK;
        memcpy(setup.lease_mac,request->mac,6);
        setup.leased_until=now+(uint64_t)LEASE*1000;
    } else return;
    transmit(&reply,1);
}

static void local_start(void) {
    uint32_t randomness[2];
#ifdef _WIN32
    if (BCryptGenRandom(NULL,(unsigned char *)randomness,sizeof randomness,BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return;
#else
    if (getrandom(randomness,sizeof randomness,0)!=(ssize_t)sizeof randomness) return;
    packet_socket=socket(AF_PACKET,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,htons(ETH_P_IP));
    if (packet_socket==INVALID_SOCKET) return;
    struct sockaddr_ll local={0};
    local.sll_family=AF_PACKET;
    local.sll_protocol=htons(ETH_P_IP);
    local.sll_ifindex=(int)adapter.index;
    if (bind(packet_socket,(const struct sockaddr *)&local,sizeof local)) { close_ports(); return; }
#endif
    client=open_port(CLIENT_PORT);
    if (client==INVALID_SOCKET) { close_ports(); return; }
#ifdef _WIN32
    server=open_port(SERVER_PORT);
#endif
    setup.phase=PROBE;
    setup.xid=randomness[0]?randomness[0]:1;
    setup.deadline=ticks()+PROBE_MS+500+randomness[1]%1501;
}

static void local_update(uint64_t now) {
    if (!setup.phase) return;
    if (setup.phase==CLIENT) {
        if (now>=setup.deadline) local_stop();
        return;
    }
#ifdef _WIN32
    if (setup.phase==PROBE && server!=INVALID_SOCKET) for (int count=0; count<16; ++count) {
        struct Dhcp packet;
        int result=receive(&packet,1);
        if (result<0) break;
        if (result && packet.type==DHCP_DISCOVER && memcmp(packet.mac,adapter.mac,6)) setup.pending=packet;
    }
#endif
    for (int count=0; count<64; ++count) {
        struct Dhcp packet;
        int result=receive(&packet,setup.phase==SERVER);
        if (result<0) break;
        if (!result) continue;
        if (setup.phase==SERVER) { answer(&packet,now); continue; }
        if (packet.type==DHCP_DISCOVER && memcmp(packet.mac,adapter.mac,6)) setup.pending=packet;
        if (packet.xid!=setup.xid || memcmp(packet.mac,adapter.mac,6)) continue;
        if (setup.phase==PROBE && packet.type==DHCP_OFFER && usable(packet.address) && usable(packet.server)) {
            setup.offer=packet;
            setup.phase=REQUEST;
            setup.deadline=now+2500;
            setup.next=0;
        } else if (setup.phase==REQUEST && packet.server==setup.offer.server) {
            if (packet.type==DHCP_NAK) { local_stop(); return; }
            if (packet.type==DHCP_ACK && packet.address==setup.offer.address) {
                int prefix=prefix_length(packet.mask?packet.mask:setup.offer.mask);
                uint32_t lease=packet.lease?packet.lease:setup.offer.lease;
                if (!prefix || !lease || !assign(packet.address,prefix)) { local_stop(); return; }
                setup.phase=CLIENT;
                setup.deadline=now+(uint64_t)lease*1000;
                close_ports();
                return;
            }
        }
    }
    if (setup.phase==SERVER) return;
    if (now>=setup.deadline) {
        if (setup.phase!=PROBE || !setup.next) { local_stop(); return; }
        if (server==INVALID_SOCKET) server=open_port(SERVER_PORT);
        if (server==INVALID_SOCKET || !assign(DHCP_SERVER,24)) { local_stop(); return; }
        setup.phase=SERVER;
        closesocket(client);
        client=INVALID_SOCKET;
        if (setup.pending.type) answer(&setup.pending,now);
        return;
    }
    if (now>=setup.next) {
        struct Dhcp request={0};
        request.xid=setup.xid;
        memcpy(request.mac,adapter.mac,6);
        request.type=DHCP_DISCOVER;
        if (setup.phase==REQUEST) {
            request.type=DHCP_REQUEST;
            request.requested=setup.offer.address;
            request.server=setup.offer.server;
        }
        if (!transmit(&request,0)) { local_stop(); return; }
        setup.next=now+RETRY_MS;
    }
}

static int elevated(void) {
#ifdef _WIN32
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;
    PSID admins;
    BOOL member=FALSE;
    if (AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,
                                 0,0,0,0,0,0,&admins)) {
        CheckTokenMembership(NULL,admins,&member);
        FreeSid(admins);
    }
    return member;
#else
    return geteuid()==0;
#endif
}

int link_prepare(void) {
    uint32_t existing;
    return find_adapter(0,1,&existing);
}

int link_needs_permission(void) {
    return !elevated();
}

void link_start(void) {
    if (!adapter.index) return;
    if (elevated()) { local_start(); return; }
#ifdef _WIN32
    wchar_t executable[32768], args[80];
    DWORD length=GetModuleFileNameW(NULL,executable,sizeof executable/sizeof *executable);
    if (!length || length>=sizeof executable/sizeof *executable) return;
    swprintf(args,sizeof args/sizeof *args,L"--link-helper %lu %u",(unsigned long)GetCurrentProcessId(),adapter.index);
    SHELLEXECUTEINFOW launch={0};
    launch.cbSize=sizeof launch;
    launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
    launch.lpVerb=L"runas";
    launch.lpFile=executable;
    launch.lpParameters=args;
    launch.nShow=SW_HIDE;
    if (ShellExecuteExW(&launch)) child=launch.hProcess;
#else
    char executable[4096], index[16];
    ssize_t length=readlink("/proc/self/exe",executable,sizeof executable-1);
    if (length<0 || length==(ssize_t)sizeof executable-1) return;
    executable[length]='\0';
    snprintf(index,sizeof index,"%u",adapter.index);
    int ends[2];
    if (pipe2(ends,O_CLOEXEC)) return;
    child=fork();
    if (child==0) {
        close(ends[1]);
        if (dup2(ends[0],STDIN_FILENO)<0 || fcntl(STDIN_FILENO,F_SETFD,0)<0) _exit(127);
        if (ends[0]!=STDIN_FILENO) close(ends[0]);
        int quiet=open("/dev/null",O_WRONLY);
        if (quiet>=0) dup2(quiet,STDERR_FILENO);
        execl("/usr/bin/pkexec","pkexec","--disable-internal-agent",executable,"--link-helper","0",index,(char *)NULL);
        _exit(127);
    }
    close(ends[0]);
    if (child>0) child_pipe=ends[1];
    else close(ends[1]);
#endif
}

void link_update(void) {
    uint64_t now=ticks();
    local_update(now);
    if (now<next_status) return;
    next_status=now+250;
    uint32_t address=setup.address;
#ifdef _WIN32
    if (child && WaitForSingleObject(child,0)!=WAIT_TIMEOUT) { CloseHandle(child); child=NULL; }
    if (child) find_adapter(adapter.index,0,&address);
#else
    if (child>0 && waitpid(child,NULL,WNOHANG)==child) {
        child=0;
        close(child_pipe);
        child_pipe=-1;
    }
    if (child>0) find_adapter(adapter.index,0,&address);
#endif
    if (address) snprintf(status,sizeof status,"LINK: %u.%u.%u.%u",address>>24,(address>>16)&255,(address>>8)&255,address&255);
    else status[0]='\0';
}

const char *link_status(void) {
    return status;
}

void link_stop(void) {
    local_stop();
#ifdef _WIN32
    if (child) CloseHandle(child);
#else
    if (child_pipe>=0) close(child_pipe);
    if (child>0) waitpid(child,NULL,WNOHANG);
#endif
}

static unsigned int parse_number(const char *text) {
    char *end;
    unsigned long value=strtoul(text,&end,10);
    return *text && !*end && value<=UINT32_MAX?(unsigned int)value:0;
}

int link_helper(const char *parent,const char *interface_index) {
    uint32_t existing;
    unsigned int index=parse_number(interface_index);
    if (!index || !elevated() || !find_adapter(index,1,&existing)) return 1;
#ifdef _WIN32
    unsigned int pid=parse_number(parent);
    if (!pid) return 1;
    HANDLE owner=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if (!owner) return 1;
    local_start();
    while (setup.phase && WaitForSingleObject(owner,20)==WAIT_TIMEOUT) local_update(ticks());
    CloseHandle(owner);
#else
    (void)parent;
    if (fcntl(STDIN_FILENO,F_GETFD)<0) return 1;
    struct sigaction action={0};
    action.sa_handler=stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,NULL);
    sigaction(SIGINT,&action,NULL);
    struct pollfd owner={STDIN_FILENO,POLLIN,0};
    local_start();
    while (!stopping && setup.phase && poll(&owner,1,20)==0) local_update(ticks());
#endif
    local_stop();
    return 0;
}
