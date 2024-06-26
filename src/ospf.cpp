#include <net/if.h>
#include <netinet/ip.h>
#include <bits/stdc++.h>
#include <sys/socket.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <stdio.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h> 
#include <arpa/inet.h>

#include "ospf.h"
#include "config.h"
#include "interface.h"

extern GlobalConfig myconfigs;

struct protoent *proto_ospf;

int ospf_init() {
	proto_ospf = getprotobyname("ospf");
	if (proto_ospf == NULL) {
		perror("getprotobyname() failed");
		return -1;
	}
	else {
		printf("proto_ospf->p_proto: %d\n", proto_ospf->p_proto);
	}
	return 0;
}

/**
 * @brief 计算ospf数据包的校验和
 * 详见 [RFC2328 D.4](https://www.rfc-editor.org/rfc/rfc2328#page-231)
 * 
 * @param data 数据包的指针
 * @param len 数据包的长度
 * @return uint16_t 校验和
*/
uint16_t ospf_checksum(const void* data, size_t len) {
	uint32_t sum = 0;
	/* 计算十六位二进制和 */
    const uint16_t* ptr = static_cast<const uint16_t*>(data);
	for (size_t i = 0; i < len / 2; ++i) {
        sum += *ptr;
		++ptr;
    }
	/* 如果长度是奇数，最后一个字节单独处理 */
    if (len & 1) {
        sum += static_cast<const uint8_t*>(data)[len - 1];
    }
	/* 处理进位 */
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

/**
 * @brief 发送 ospf 数据包
 * 
 * @param dst_ip 		目的 ip
 * @param ospf_type 	ospf 类型
 * @param ospf_data		ospf 数据
 * @param ospf_data_len ospf 数据长度
 */
void send_ospf_packet(uint32_t dst_ip,
	const uint8_t ospf_type, const char* ospf_data, const size_t ospf_data_len) {

	int socket_fd;
    if ((socket_fd = socket(AF_INET, SOCK_RAW, proto_ospf->p_proto)) < 0) {
        perror("SendPacket: socket_fd init");
    }

	/* 将 socket 绑定到特定到网卡，所有的包都将通过该网卡进行发送 */
	/* 在 linux 中，所有网络设备都使用 ifreq 来标识 */
	/* 在此处只需要使用 ifreq 的 ifr_name */
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, myconfigs.nic_name);
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
        perror("SendPacket: setsockopt");
    }

	/* 1480 = 1500(MTU) - 20(IP_HEADER) */
	char* ospf_packet = (char*) malloc(1480);
	/* 设置 ospf header */
	size_t ospf_len = ospf_data_len + sizeof(OSPFHeader);
	OSPFHeader* ospf_header 	= (OSPFHeader*)ospf_packet;
    ospf_header->version		= 2;
    ospf_header->type 			= ospf_type;
    ospf_header->packet_length 	= htons(ospf_len);
    ospf_header->router_id 		= htonl(myconfigs.router_id);
    ospf_header->area_id   		= htonl(0 /* TODO */);
    ospf_header->checksum 		= 0;
    ospf_header->autype 		= 0;
    ospf_header->authentication[0] = 0;
    ospf_header->authentication[1] = 0;

	/* 复制 ospf 数据部分 */
	memcpy(ospf_packet + sizeof(OSPFHeader), ospf_data, ospf_data_len);

	/* 计算校验和 */
	ospf_header->checksum 		= ospf_checksum(ospf_packet, ospf_len);

	/* 设置目的地址，使用 sockaddr_in */
	/* sendto 中需要将 sockaddr_in 强制类型转换为 sockaddr */
    struct sockaddr_in dst_sockaddr;
    memset(&dst_sockaddr, 0, sizeof(dst_sockaddr));
    dst_sockaddr.sin_family = AF_INET;
    dst_sockaddr.sin_addr.s_addr = htonl(dst_ip);

	/* 发送 ospf 包 */
	if (sendto(socket_fd, ospf_packet, ospf_len, 0, (struct sockaddr*)&dst_sockaddr, sizeof(dst_sockaddr)) < 0) {
		perror("SendPacket: sendto");
	}
	else {
		printf("SendPacket: type %d send success, len %ld\n", ospf_type, ospf_data_len);
	}
	free(ospf_packet);
}

void* send_ospf_hello_package_thread(void* inf) {
	Interface *interface = (Interface*)inf;

	int socket_fd;
    if ((socket_fd = socket(AF_INET, SOCK_RAW, proto_ospf->p_proto)) < 0) {
        perror("SendPacket: socket_fd init");
    }

	/* 将 socket 绑定到特定到网卡，所有的包都将通过该网卡进行发送 */
	/* 在 linux 中，所有网络设备都使用 ifreq 来标识 */
	/* 在此处只需要使用 ifreq 的 ifr_name */
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, myconfigs.nic_name);
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
        perror("SendPacket: setsockopt");
    }

	/* 将目的地址设为 AllSPFRouters 224.0.0.5 */
    struct sockaddr_in dst_sockaddr;
    memset(&dst_sockaddr, 0, sizeof(dst_sockaddr));
    dst_sockaddr.sin_family = AF_INET;
    dst_sockaddr.sin_addr.s_addr = inet_addr("224.0.0.5");

	/* 1480 = 1500(MTU) - 20(IP_HEADER) */
	char* ospf_packet = (char*) malloc(1480);
	while (true) {
		size_t ospf_len = sizeof(OSPFHeader) + sizeof(OSPFHello) + 4 * interface->neighbors.size();
		
		/* 头部 */
		
		OSPFHeader* ospf_header 	= (OSPFHeader*)ospf_packet;
		ospf_header->version		= 2;
		ospf_header->type 			= 1;
		ospf_header->packet_length 	= htons(ospf_len);
		ospf_header->router_id 		= htonl(myconfigs.router_id);
		ospf_header->area_id   		= htonl(myconfigs.area->area_id);
		ospf_header->checksum 		= 0;
		ospf_header->autype 		= 0;
		ospf_header->authentication[0] = 0;
		ospf_header->authentication[1] = 0;

		/* 载荷 */

		OSPFHello* ospf_hello = (OSPFHello*)(ospf_packet + sizeof(OSPFHeader)); 
        ospf_hello->network_mask				= htonl(interface->network_mask);	// 对应接口的 network_mask
        ospf_hello->hello_interval				= htons(myconfigs.hello_interval);
        ospf_hello->options						= 0x02;
        ospf_hello->rtr_pri						= 1;
        ospf_hello->router_dead_interval		= htonl(myconfigs.dead_interval);
        ospf_hello->designated_router			= htonl(interface->dr);
        ospf_hello->backup_designated_router	= htonl(interface->bdr);

		uint32_t* ospf_hello_neighbor = (uint32_t*)(ospf_packet + sizeof(OSPFHeader) + sizeof(OSPFHello));
        for (auto& nbr: interface->neighbors) {
            *ospf_hello_neighbor++ = htonl(nbr->id);
        }

		/* 计算校验和 */
		ospf_header->checksum 		= ospf_checksum(ospf_packet, ospf_len);

		/* Send Packet */
		if (sendto(socket_fd, ospf_packet, ospf_len, 0, (struct sockaddr*)&dst_sockaddr, sizeof(dst_sockaddr)) < 0) {
			perror("[Thread]\tSendHelloPacket: sendto");
		}
		else {
			printf("[Thread]\tSendHelloPacket: send success\n");
		}
        sleep(myconfigs.hello_interval);
	}
}