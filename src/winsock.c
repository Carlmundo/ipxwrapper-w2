/* ipxwrapper - Winsock functions
 * Copyright (C) 2008 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <windows.h>
#include <winsock2.h>
#include <wsipx.h>
#include <mswsock.h>
#include <nspapi.h>
#include <wsnwlink.h>

#include "ipxwrapper.h"
#include "common.h"
#include "interface.h"
#include "router.h"

typedef struct _PROTOCOL_INFO {
	DWORD dwServiceFlags ;
	INT iAddressFamily ;
	INT iMaxSockAddr ;
	INT iMinSockAddr ;
	INT iSocketType ;
	INT iProtocol ;
	DWORD dwMessageSize ;
	void *lpProtocol ;
} PROTOCOL_INFO;

static size_t strsize(void *str, BOOL unicode) {
	return unicode ? 2 + wcslen(str)*2 : 1 + strlen(str);
}

static int do_EnumProtocols(LPINT protocols, LPVOID buf, LPDWORD bsptr, BOOL unicode) {
	int bufsize = *bsptr, rval, i, want_ipx = 0;
	
	PROTOCOL_INFO *pinfo = buf;
	
	if((rval = unicode ? r_EnumProtocolsW(protocols, buf, bsptr) : r_EnumProtocolsA(protocols, buf, bsptr)) == -1) {
		return -1;
	}
	
	if(!protocols) {
		want_ipx = 1;
	}else{
		for(i = 0; protocols[i]; i++) {
			if(protocols[i] == NSPROTO_IPX) {
				want_ipx = 1;
				break;
			}
		}
	}
	
	if(want_ipx) {
		for(i = 0; i < rval; i++) {
			if(pinfo[i].iProtocol == NSPROTO_IPX) {
				return rval;
			}
		}
		
		*bsptr += sizeof(PROTOCOL_INFO) + (unicode ? 8 : 4);
		
		if(*bsptr > bufsize) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return -1;
		}
		
		/* Make sure there is space between the last PROTOCOL_INFO structure
		 * and the protocol names for the extra structure.
		*/
		
		size_t slen = 0, off = 0;
		
		for(i = 0; i < rval; i++) {
			slen += strsize(pinfo[i].lpProtocol, unicode);
		}
		
		char *name_buf = malloc(slen);
		if(!name_buf) {
			SetLastError(ERROR_OUTOFMEMORY);
			return -1;
		}
		
		for(i = 0; i < rval; i++) {
			slen = strsize(pinfo[i].lpProtocol, unicode);
			memcpy(name_buf + off, pinfo[i].lpProtocol, slen);
			
			off += slen;
		}
		
		char *name_dest = ((char*)buf) + sizeof(PROTOCOL_INFO) * (rval + 1);
		
		memcpy(name_dest, name_buf, off);
		free(name_buf);
		
		if(unicode) {
			wcscpy((wchar_t*)(name_dest + off), L"IPX");
		}else{
			strcpy(name_dest + off, "IPX");
		}
		
		for(i = 0, off = 0; i < rval; i++) {
			pinfo[i].lpProtocol = name_dest + off;
			off += strsize(pinfo[i].lpProtocol, unicode);
		}
		
		int ipx_off = rval++;
		
		pinfo[ipx_off].dwServiceFlags = 5641;
		pinfo[ipx_off].iAddressFamily = AF_IPX;
		pinfo[ipx_off].iMaxSockAddr = 16;
		pinfo[ipx_off].iMinSockAddr = 14;
		pinfo[ipx_off].iSocketType = SOCK_DGRAM;
		pinfo[ipx_off].iProtocol = NSPROTO_IPX;
		pinfo[ipx_off].dwMessageSize = 576;
		pinfo[ipx_off].lpProtocol = name_dest + off;
	}
	
	return rval;
}

INT APIENTRY EnumProtocolsA(LPINT protocols, LPVOID buf, LPDWORD bsptr) {
	return do_EnumProtocols(protocols, buf, bsptr, FALSE);
}

INT APIENTRY EnumProtocolsW(LPINT protocols, LPVOID buf, LPDWORD bsptr) {
	return do_EnumProtocols(protocols, buf, bsptr, TRUE);
}

INT WINAPI WSHEnumProtocols(LPINT protocols, LPWSTR ign, LPVOID buf, LPDWORD bsptr) {
	return do_EnumProtocols(protocols, buf, bsptr, FALSE);
}

SOCKET WSAAPI socket(int af, int type, int protocol) {
	log_printf("socket(%d, %d, %d)", af, type, protocol);
	
	if(af == AF_IPX) {
		ipx_socket *nsock = malloc(sizeof(ipx_socket));
		if(!nsock) {
			WSASetLastError(ERROR_OUTOFMEMORY);
			return -1;
		}
		
		nsock->fd = r_socket(AF_INET, SOCK_DGRAM, 0);
		if(nsock->fd == -1) {
			log_printf("Creating fake socket failed: %s", w32_error(WSAGetLastError()));
			
			free(nsock);
			return -1;
		}
		
		nsock->flags = IPX_SEND | IPX_RECV;
		nsock->s_ptype = (protocol ? NSPROTO_IPX - protocol : 0);
		
		lock_sockets();
		
		nsock->next = sockets;
		sockets = nsock;
		
		log_printf("IPX socket created (fd = %d)", nsock->fd);
		
		RETURN(nsock->fd);
	}else{
		return r_socket(af, type, protocol);
	}
}

int WSAAPI closesocket(SOCKET fd) {
	int ret = r_closesocket(fd);
	
	ipx_socket *ptr = get_socket(fd);
	ipx_socket *pptr = sockets;
	
	if(!ptr) {
		/* Not an IPX socket */
		return ret;
	}
	
	if(ret == SOCKET_ERROR) {
		log_printf("closesocket(%d) failed: %s", fd, w32_error(WSAGetLastError()));
		RETURN(SOCKET_ERROR);
	}
	
	log_printf("IPX socket closed (fd = %d)", fd);
	
	if(ptr->flags & IPX_BOUND) {
		rclient_unbind(&g_rclient, fd);
	}
	
	if(ptr == sockets) {
		sockets = ptr->next;
		free(ptr);
	}else{
		while(ptr && pptr->next) {
			if(ptr == pptr->next) {
				pptr->next = ptr->next;
				free(ptr);
			}
			
			pptr = pptr->next;
		}
	}
	
	RETURN(0);
}

int WSAAPI bind(SOCKET fd, const struct sockaddr *addr, int addrlen) {
	ipx_socket *ptr = get_socket(fd);
	
	if(ptr) {
		struct sockaddr_ipx ipxaddr;
		char net_s[12], node_s[18];
		
		if(addrlen < sizeof(ipxaddr)) {
			RETURN_WSA(WSAEFAULT, -1);
		}
		
		memcpy(&ipxaddr, addr, sizeof(ipxaddr));
		
		NET_TO_STRING(net_s, ipxaddr.sa_netnum);
		NODE_TO_STRING(node_s, ipxaddr.sa_nodenum);
		
		log_printf("bind(%d, net=%s node=%s socket=%hu)", fd, net_s, node_s, ntohs(ipxaddr.sa_socket));
		
		if(ptr->flags & IPX_BOUND) {
			log_printf("bind failed: socket already bound");
			RETURN_WSA(WSAEINVAL, -1);
		}
		
		if(!rclient_bind(&g_rclient, fd, &ipxaddr, &(ptr->nic_bcast), ptr->flags & IPX_REUSE ? TRUE : FALSE)) {
			RETURN(-1);
		}
		
		NET_TO_STRING(net_s, ipxaddr.sa_netnum);
		NODE_TO_STRING(node_s, ipxaddr.sa_nodenum);
		
		log_printf("bind address: net=%s node=%s socket=%hu", net_s, node_s, ntohs(ipxaddr.sa_socket));
		
		struct sockaddr_in bind_addr;
		bind_addr.sin_family = AF_INET;
		bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind_addr.sin_port = 0;
		
		if(r_bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) == -1) {
			log_printf("Binding local UDP socket failed: %s", w32_error(WSAGetLastError()));
			
			rclient_unbind(&g_rclient, fd);
			RETURN(-1);
		}
		
		int al = sizeof(bind_addr);
		
		if(r_getsockname(fd, (struct sockaddr*)&bind_addr, &al) == -1) {
			log_printf("getsockname failed: %s", w32_error(WSAGetLastError()));
			
			rclient_unbind(&g_rclient, fd);
			RETURN(-1);
		}
		
		memcpy(&(ptr->addr), &ipxaddr, sizeof(ipxaddr));
		ptr->flags |= IPX_BOUND;
		
		if(ptr->flags & IPX_RECV) {
			rclient_set_port(&g_rclient, fd, bind_addr.sin_port);
		}
		
		rclient_set_filter(&g_rclient, fd, ptr->flags & IPX_FILTER ? ptr->f_ptype : -1);
		
		RETURN(0);
	}else{
		return r_bind(fd, addr, addrlen);
	}
}

int WSAAPI getsockname(SOCKET fd, struct sockaddr *addr, int *addrlen) {
	ipx_socket *ptr = get_socket(fd);
	
	if(ptr) {
		if(ptr->flags & IPX_BOUND) {
			if(*addrlen < sizeof(struct sockaddr_ipx)) {
				*addrlen = sizeof(struct sockaddr_ipx);
				RETURN_WSA(WSAEFAULT, -1);
			}
			
			memcpy(addr, &(ptr->addr), sizeof(ptr->addr));
			*addrlen = sizeof(struct sockaddr_ipx);
			
			RETURN(0);
		}else{
			RETURN_WSA(WSAEINVAL, -1);
		}
	}else{
		return r_getsockname(fd, addr, addrlen);
	}
}

#define RECVBUF_SIZE (sizeof(struct rpacket_header) + MAX_PKT_SIZE)

/* Recieve a packet from an IPX socket
 * addr must be NULL or a region of memory big enough for a sockaddr_ipx
 *
 * The mutex should be locked before calling and will be released before returning
 * The size of the packet will be returned on success, even if it was truncated
*/
static int recv_packet(ipx_socket *sockptr, char *buf, int bufsize, int flags, struct sockaddr_ipx *addr) {
	SOCKET fd = sockptr->fd;
	int is_bound = sockptr->flags & IPX_BOUND;
	
	unlock_sockets();
	
	if(!is_bound) {
		WSASetLastError(WSAEINVAL);
		return -1;
	}
	
	char *recvbuf = malloc(RECVBUF_SIZE);
	if(!recvbuf) {
		WSASetLastError(ERROR_OUTOFMEMORY);
		return -1;
	}
	
	struct rpacket_header *rp_header = (struct rpacket_header*)recvbuf;
	struct ipx_packet *packet = (struct ipx_packet*)(recvbuf + sizeof(*rp_header));
	
	int rval = r_recv(fd, recvbuf, RECVBUF_SIZE, flags);
	if(rval == -1) {
		free(recvbuf);
		return -1;
	}
	
	if(rval < sizeof(*rp_header) + sizeof(ipx_packet) - 1 || rval != sizeof(*rp_header) + packet->size + sizeof(ipx_packet) - 1) {
		log_printf("Invalid packet received on loopback port!");
		
		free(recvbuf);
		WSASetLastError(WSAEWOULDBLOCK);
		return -1;
	}
	
	add_host(packet->src_net, packet->src_node, rp_header->src_ipaddr);
	
	if(addr) {
		addr->sa_family = AF_IPX;
		memcpy(addr->sa_netnum, packet->src_net, 4);
		memcpy(addr->sa_nodenum, packet->src_node, 6);
		addr->sa_socket = packet->src_socket;
	}
	
	memcpy(buf, packet->data, packet->size <= bufsize ? packet->size : bufsize);
	rval = packet->size;
	free(recvbuf);
	
	return rval;
}

int WSAAPI recvfrom(SOCKET fd, char *buf, int len, int flags, struct sockaddr *addr, int *addrlen) {
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr) {
		if(addr && addrlen && *addrlen < sizeof(struct sockaddr_ipx)) {
			unlock_sockets();
			
			WSASetLastError(WSAEFAULT);
			return -1;
		}
		
		int rval = recv_packet(sockptr, buf, len, flags, (struct sockaddr_ipx*)addr);
		
		/* The value pointed to by addrlen is only set if the recv call was
		 * successful, may not be correct.
		*/
		if(rval >= 0 && addr && addrlen) {
			*addrlen = sizeof(struct sockaddr_ipx);
		}
		
		if(rval > len) {
			WSASetLastError(WSAEMSGSIZE);
			return -1;
		}
		
		return rval;
	}else{
		return r_recvfrom(fd, buf, len, flags, addr, addrlen);
	}
}

int WSAAPI recv(SOCKET fd, char *buf, int len, int flags) {
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr) {
		int rval = recv_packet(sockptr, buf, len, flags, NULL);
		
		if(rval > len) {
			WSASetLastError(WSAEMSGSIZE);
			return -1;
		}
		
		return rval;
	}else{
		return r_recv(fd, buf, len, flags);
	}
}

int PASCAL WSARecvEx(SOCKET fd, char *buf, int len, int *flags) {
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr) {
		int rval = recv_packet(sockptr, buf, len, 0, NULL);
		
		if(rval > len) {
			*flags = MSG_PARTIAL;
			
			/* Wording of MSDN is unclear on what should be returned when
			 * an incomplete message is read, I think it should return the
			 * amount of data copied to the buffer.
			*/
			rval = len;
		}else if(rval != -1) {
			*flags = 0;
		}
		
		return rval;
	}else{
		return r_WSARecvEx(fd, buf, len, flags);
	}
}

#define CHECK_OPTLEN(size) \
	if(*optlen < size) {\
		*optlen = size;\
		RETURN_WSA(WSAEFAULT, -1);\
	}\
	*optlen = size;

int WSAAPI getsockopt(SOCKET fd, int level, int optname, char FAR *optval, int FAR *optlen) {
	int* intval = (int*)optval;
	BOOL *bval = (BOOL*)optval;
	
	ipx_socket *ptr = get_socket(fd);
	
	if(ptr) {
		if(level == NSPROTO_IPX) {
			if(optname == IPX_PTYPE) {
				CHECK_OPTLEN(sizeof(int));
				*intval = ptr->s_ptype;
				
				RETURN(0);
			}
			
			if(optname == IPX_FILTERPTYPE) {
				CHECK_OPTLEN(sizeof(int));
				*intval = ptr->f_ptype;
				
				RETURN(0);
			}
			
			if(optname == IPX_MAXSIZE) {
				CHECK_OPTLEN(sizeof(int));
				*intval = MAX_DATA_SIZE;
				
				RETURN(0);
			}
			
			if(optname == IPX_ADDRESS) {
				CHECK_OPTLEN(sizeof(IPX_ADDRESS_DATA));
				
				IPX_ADDRESS_DATA *ipxdata = (IPX_ADDRESS_DATA*)optval;
				
				struct ipx_interface *nic = get_interfaces(ipxdata->adapternum);
				
				if(!nic) {
					WSASetLastError(ERROR_NO_DATA);
					return -1;
				}
				
				memcpy(ipxdata->netnum, nic->ipx_net, 4);
				memcpy(ipxdata->nodenum, nic->ipx_node, 6);
				
				/* TODO: LAN/WAN detection, link speed detection */
				ipxdata->wan = FALSE;
				ipxdata->status = FALSE;
				ipxdata->maxpkt = MAX_DATA_SIZE;
				ipxdata->linkspeed = 100000; /* 10MBps */
				
				free_interfaces(nic);
				
				RETURN(0);
			}
			
			/* NOTE: IPX_MAX_ADAPTER_NUM implies it may be the maximum index
			 * for referencing an IPX interface. This behaviour makes no sense
			 * and a code example in MSDN implies it should be the number of
			 * IPX interfaces, this code follows the latter.
			*/
			if(optname == IPX_MAX_ADAPTER_NUM) {
				CHECK_OPTLEN(sizeof(int));
				
				*intval = 0;
				
				struct ipx_interface *ifaces = get_interfaces(-1), *nic;
				
				for(nic = ifaces; nic;) {
					(*intval)++;
					nic = nic->next;
				}
				
				free_interfaces(ifaces);
				
				RETURN(0);
			}
			
			RETURN_WSA(WSAENOPROTOOPT, -1);
		}
		
		if(level == SOL_SOCKET) {
			if(optname == SO_BROADCAST) {
				CHECK_OPTLEN(sizeof(BOOL));
				
				*bval = ptr->flags & IPX_BROADCAST ? TRUE : FALSE;
				RETURN(0);
			}
			
			if(optname == SO_REUSEADDR) {
				CHECK_OPTLEN(sizeof(BOOL));
				
				*bval = ptr->flags & IPX_REUSE ? TRUE : FALSE;
				RETURN(0);
			}
		}
		
		unlock_sockets();
	}
	
	return r_getsockopt(fd, level, optname, optval, optlen);
}

int WSAAPI setsockopt(SOCKET fd, int level, int optname, const char FAR *optval, int optlen) {
	int *intval = (int*)optval;
	BOOL *bval = (BOOL*)optval;
	
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr) {
		if(level == NSPROTO_IPX) {
			if(optname == IPX_PTYPE) {
				sockptr->s_ptype = *intval;
				RETURN(0);
			}
			
			if(optname == IPX_FILTERPTYPE) {
				if(sockptr->flags & IPX_BOUND && !rclient_set_filter(&g_rclient, fd, *intval)) {
					RETURN(-1);
				}
				
				sockptr->f_ptype = *intval;
				sockptr->flags |= IPX_FILTER;
				
				RETURN(0);
			}
			
			if(optname == IPX_STOPFILTERPTYPE) {
				if(sockptr->flags & IPX_BOUND && !rclient_set_filter(&g_rclient, fd, -1)) {
					RETURN(-1);
				}
				
				sockptr->flags &= ~IPX_FILTER;
				
				RETURN(0);
			}
			
			RETURN_WSA(WSAENOPROTOOPT, -1);
		}
		
		if(level == SOL_SOCKET) {
			if(optname == SO_BROADCAST) {
				if(*bval) {
					sockptr->flags |= IPX_BROADCAST;
				}else{
					sockptr->flags &= ~IPX_BROADCAST;
				}
				
				RETURN(0);
			}else if(optname == SO_REUSEADDR) {
				if(sockptr->flags & IPX_BOUND && !rclient_set_reuse(&g_rclient, fd, *bval)) {
					RETURN(-1);
				}
				
				if(*bval) {
					sockptr->flags |= IPX_REUSE;
				}else{
					sockptr->flags &= ~IPX_REUSE;
				}
				
				RETURN(0);
			}
		}
		
		unlock_sockets();
	}
	
	return r_setsockopt(fd, level, optname, optval, optlen);
}

int WSAAPI sendto(SOCKET fd, const char *buf, int len, int flags, const struct sockaddr *addr, int addrlen) {
	struct sockaddr_ipx *ipxaddr = (struct sockaddr_ipx*)addr;
	
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr) {
		if(!addr || addrlen < sizeof(struct sockaddr_ipx)) {
			RETURN_WSA(WSAEDESTADDRREQ, -1);
		}
		
		if(!(sockptr->flags & IPX_SEND)) {
			RETURN_WSA(WSAESHUTDOWN, -1);
		}
		
		if(!(sockptr->flags & IPX_BOUND)) {
			log_printf("sendto() on unbound socket, attempting implicit bind");
			
			struct sockaddr_ipx bind_addr;
			
			bind_addr.sa_family = AF_IPX;
			memcpy(bind_addr.sa_netnum, ipxaddr->sa_netnum, 4);
			memset(bind_addr.sa_nodenum, 0, 6);
			bind_addr.sa_socket = 0;
			
			if(bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) == -1) {
				RETURN(-1);
			}
		}
		
		if(len > MAX_DATA_SIZE) {
			RETURN_WSA(WSAEMSGSIZE, -1);
		}
		
		int psize = sizeof(ipx_packet)+len-1;
		
		ipx_packet *packet = malloc(psize);
		if(!packet) {
			RETURN_WSA(ERROR_OUTOFMEMORY, -1);
		}
		
		packet->ptype = sockptr->s_ptype;
		
		memcpy(packet->dest_net, ipxaddr->sa_netnum, 4);
		memcpy(packet->dest_node, ipxaddr->sa_nodenum, 6);
		packet->dest_socket = ipxaddr->sa_socket;
		
		unsigned char z6[] = {0,0,0,0,0,0};
		
		if(memcmp(packet->dest_net, z6, 4) == 0) {
			memcpy(packet->dest_net, sockptr->addr.sa_netnum, 4);
		}
		
		memcpy(packet->src_net, sockptr->addr.sa_netnum, 4);
		memcpy(packet->src_node, sockptr->addr.sa_nodenum, 6);
		packet->src_socket = sockptr->addr.sa_socket;
		
		packet->size = htons(len);
		memcpy(packet->data, buf, len);
		
		ipx_host *host = find_host(packet->dest_net, packet->dest_node);
		
		struct sockaddr_in saddr;
		saddr.sin_family = AF_INET;
		saddr.sin_port = htons(global_conf.udp_port);
		saddr.sin_addr.s_addr = (host ? host->ipaddr : (global_conf.bcast_all ? INADDR_BROADCAST : sockptr->nic_bcast));
		
		int sval = r_sendto(send_fd, (char*)packet, psize, 0, (struct sockaddr*)&saddr, sizeof(saddr));
		if(sval == -1) {
			len = -1;
		}
		
		free(packet);
		RETURN(len);
	}else{
		return r_sendto(fd, buf, len, flags, addr, addrlen);
	}
}

int PASCAL shutdown(SOCKET fd, int cmd) {
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr) {
		if(cmd == SD_RECEIVE || cmd == SD_BOTH) {
			if(sockptr->flags & IPX_BOUND && !rclient_set_port(&g_rclient, fd, 0)) {
				RETURN(-1);
			}
			
			sockptr->flags &= ~IPX_RECV;
		}
		
		if(cmd == SD_SEND || cmd == SD_BOTH) {
			sockptr->flags &= ~IPX_SEND;
		}
		
		RETURN(0);
	}else{
		return r_shutdown(fd, cmd);
	}
}

int PASCAL ioctlsocket(SOCKET fd, long cmd, u_long *argp) {
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr && cmd == FIONREAD) {
		ipx_packet packet;
		fd_set fdset;
		struct timeval tv = {0,0};
		
		FD_ZERO(&fdset);
		FD_SET(sockptr->fd, &fdset);
		
		int r = select(1, &fdset, NULL, NULL, &tv);
		if(r == -1) {
			RETURN(-1);
		}else if(r == 0) {
			*(unsigned long*)argp = 0;
			RETURN(0);
		}
		
		r = r_recv(sockptr->fd, (char*)&packet, sizeof(packet), MSG_PEEK);
		if(r == -1 && WSAGetLastError() != WSAEMSGSIZE) {
			RETURN(-1);
		}
		
		*(unsigned long*)argp = packet.size;
		RETURN(0);
	}
	
	if(sockptr) {
		unlock_sockets();
	}
	
	return r_ioctlsocket(fd, cmd, argp);
}

int PASCAL connect(SOCKET fd, const struct sockaddr *addr, int addrlen) {
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr) {
		if(addrlen < sizeof(struct sockaddr_ipx)) {
			RETURN_WSA(WSAEFAULT, -1);
		}
		
		struct sockaddr_ipx *ipxaddr = (struct sockaddr_ipx*)addr;
		
		const unsigned char z6[] = {0,0,0,0,0,0};
		
		if(ipxaddr->sa_family == AF_UNSPEC || (ipxaddr->sa_family == AF_IPX && memcmp(ipxaddr->sa_nodenum, z6, 6) == 0)) {
			if(!(sockptr->flags & IPX_CONNECTED)) {
				RETURN(0);
			}
			
			struct sockaddr_ipx dc_addr;
			dc_addr.sa_family = AF_UNSPEC;
			
			if(!rclient_set_remote(&g_rclient, fd, &dc_addr)) {
				RETURN(-1);
			}
			
			sockptr->flags &= ~IPX_CONNECTED;
			
			RETURN(0);
		}
		
		if(ipxaddr->sa_family != AF_IPX) {
			RETURN_WSA(WSAEAFNOSUPPORT, -1);
		}
		
		if(!(sockptr->flags & IPX_BOUND)) {
			log_printf("connect() on unbound socket, attempting implicit bind");
			
			struct sockaddr_ipx bind_addr;
			
			bind_addr.sa_family = AF_IPX;
			memcpy(bind_addr.sa_netnum, ipxaddr->sa_netnum, 4);
			memset(bind_addr.sa_nodenum, 0, 6);
			bind_addr.sa_socket = 0;
			
			if(bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) == -1) {
				RETURN(-1);
			}
		}
		
		if(!rclient_set_remote(&g_rclient, fd, ipxaddr)) {
			RETURN(-1);
		}
		
		memcpy(&(sockptr->remote_addr), addr, sizeof(*ipxaddr));
		sockptr->flags |= IPX_CONNECTED;
		
		RETURN(0);
	}else{
		return r_connect(fd, addr, addrlen);
	}
}

int PASCAL send(SOCKET fd, const char *buf, int len, int flags) {
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr) {
		if(!(sockptr->flags & IPX_CONNECTED)) {
			RETURN_WSA(WSAENOTCONN, -1);
		}
		
		int ret = sendto(fd, buf, len, 0, (struct sockaddr*)&(sockptr->remote_addr), sizeof(struct sockaddr_ipx));
		RETURN(ret);
	}else{
		return r_send(fd, buf, len, flags);
	}
}

int PASCAL getpeername(SOCKET fd, struct sockaddr *addr, int *addrlen) {
	ipx_socket *sockptr = get_socket(fd);
	
	if(sockptr) {
		if(!(sockptr->flags & IPX_CONNECTED)) {
			RETURN_WSA(WSAENOTCONN, -1);
		}
		
		if(*addrlen < sizeof(struct sockaddr_ipx)) {
			RETURN_WSA(WSAEFAULT, -1);
		}
		
		memcpy(addr, &(sockptr->remote_addr), sizeof(struct sockaddr_ipx));
		*addrlen = sizeof(struct sockaddr_ipx);
		
		RETURN(0);
	}else{
		return r_getpeername(fd, addr, addrlen);
	}
}
