// license:BSD-3-Clause
// Nintendo Switch build: libnx's network stack has no IPv6 types. asio (MAME's HTTP server, unused on the
// console) needs them to compile; they are declared here and never used at run time.
#ifndef TCVR_SWITCH_NETINET_IN_H
#define TCVR_SWITCH_NETINET_IN_H
#include_next <netinet/in.h>
#include <stdint.h>
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif
#ifndef s6_addr
struct in6_addr { uint8_t s6_addr[16]; };
#define TCVR_SWITCH_IN6 1
#endif
#ifndef TCVR_SWITCH_SOCKADDR6
#define TCVR_SWITCH_SOCKADDR6 1
struct sockaddr_in6 { sa_family_t sin6_family; in_port_t sin6_port; uint32_t sin6_flowinfo; struct in6_addr sin6_addr; uint32_t sin6_scope_id; };
struct ipv6_mreq { struct in6_addr ipv6mr_multiaddr; unsigned int ipv6mr_interface; };
#endif
#ifndef IP_ADD_MEMBERSHIP
struct ip_mreq { struct in_addr imr_multiaddr; struct in_addr imr_interface; };
#endif
#endif
