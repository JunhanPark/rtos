#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>
#include <net/ether.h>
#undef IP_TTL
#include <net/ip.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <net/checksum.h>
#include <net/udp.h>
#include <net/tftp.h>
#include <util/list.h>
#include <util/ring.h>
#include <util/event.h>
#undef BYTE_ORDER
#include <lwip/tcp.h>
#include <control/rpc.h>
#include "malloc.h"
#include "mp.h"
#include "cpu.h"
#include "shell.h"
#include "vm.h"
#include "stdio.h"

#include "manager.h"

#define DEFAULT_MANAGER_IP	0xc0a864fe	// 192.168.100.254
#define DEFAULT_MANAGER_GW	0xc0a864c8	// 192.168.100.200
#define DEFAULT_MANAGER_NETMASK	0xffffff00	// 255.255.255.0
#define DEFAULT_MANAGER_PORT	111

NI*	manager_ni;
static struct netif* manager_netif;
static struct tcp_pcb* manager_server;
static uint16_t manager_port;

// LwIP TCP callbacks
typedef struct {
	struct tcp_pcb*	pcb;
	List*		pbufs;
	int		poll_count;
} RPCData;

static List* clients;	/* pcb */
static List* actives;	/* rpc */

static err_t manager_poll(void* arg, struct tcp_pcb* pcb);

static void rpc_free(RPC* rpc) {
	RPCData* data = (RPCData*)rpc->data;
	
	ListIterator iter;
	list_iterator_init(&iter, data->pbufs);
	while(list_iterator_has_next(&iter)) {
		struct pbuf* pbuf = list_iterator_next(&iter);
		list_iterator_remove(&iter);
		pbuf_free(pbuf);
	}
	list_destroy(data->pbufs);
	list_remove_data(actives, rpc);
	free(rpc);
	
	list_remove_data(clients, data->pcb);
}

static void manager_close(struct tcp_pcb* pcb, RPC* rpc, bool is_force) {
	printf("Close connection: %p\n", pcb);
	tcp_arg(pcb, NULL);
	tcp_sent(pcb, NULL);
	tcp_recv(pcb, NULL);
	tcp_err(pcb, NULL);
	tcp_poll(pcb, NULL, 0);
	
	if(rpc) {
		rpc_free(rpc);
	} else {
		list_remove_data(clients, pcb);
	}
	
	if(is_force) {
		tcp_abort(pcb);
	} else if(tcp_close(pcb) != ERR_OK) {
		printf("Cannot close pcb: %p %p\n", pcb, rpc);
		//tcp_poll(pcb, manager_poll, 2);
	}
}

static err_t manager_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
	RPC* rpc = arg;
	
	if(p == NULL) {	// Remote host closed the connection
		manager_close(pcb, rpc, false);
	} else if(err != ERR_OK) {
		pbuf_free(p);
	} else {
		RPCData* data = (RPCData*)rpc->data;
		list_add(data->pbufs, p);

		rpc_loop(rpc);
		tcp_recved(pcb, p->len);
		if(rpc_is_active(rpc)) {
			if(list_index_of(actives, rpc, NULL) < 0) {
				list_add(actives, rpc);
			}
		}
	}
	
	return ERR_OK;
}

static void manager_err(void* arg, err_t err) {
	RPC* rpc = arg;
	if(rpc != NULL) {
		RPCData* data = (RPCData*)rpc->data;
		printf("Error closed: %d %p\n", err, data->pcb);
		rpc_free(rpc);
	}
}

static err_t manager_poll(void* arg, struct tcp_pcb* pcb) {
	RPC* rpc = arg;
	
	if(rpc == NULL) {
		manager_close(pcb, NULL, true);
	} else {
		RPCData* data = (RPCData*)rpc->data;
		data->poll_count++;
		
		if(rpc->ver == 0 && data->poll_count++ >= 4) {	// 2 seconds
			printf("Close connection: not receiving hello in 2 secs.\n");
			manager_close(pcb, rpc, false);
		}
	}
	
	return ERR_OK;
}

// Handlers
static void vm_create_handler(RPC* rpc, VMSpec* vm, void* context, void(*callback)(RPC* rpc, uint32_t id)) {
	uint32_t id = vm_create(vm);
	callback(rpc, id);
}

static void vm_get_handler(RPC* rpc, uint32_t vmid, void* context, void(*callback)(RPC* rpc, VMSpec* vm)) {
	// TODO: Implement it
	callback(rpc, NULL);
}

static void vm_set_handler(RPC* rpc, VMSpec* vm, void* context, void(*callback)(RPC* rpc, bool result)) {
	// TODO: Implement it
	callback(rpc, false);
}

static void vm_delete_handler(RPC* rpc, uint32_t vmid, void* context, void(*callback)(RPC* rpc, bool result)) {
	bool result = vm_delete(vmid);
	callback(rpc, result);
}

static void vm_list_handler(RPC* rpc, int size, void* context, void(*callback)(RPC* rpc, uint32_t* ids, int size)) {
	uint32_t ids[16];
	size = vm_list(ids, size <= 16 ? size : 16);
	callback(rpc, ids, size);
}

static void status_get_handler(RPC* rpc, uint32_t vmid, void* context, void(*callback)(RPC* rpc, VMStatus status)) {
	VMStatus status = vm_status_get(vmid);
	callback(rpc, status);
}

static void status_set_handler(RPC* rpc, uint32_t vmid, VMStatus status, void* context, void(*callback)(RPC* rpc, bool result)) {
	typedef struct {
		RPC* rpc;
		struct tcp_pcb*	pcb;
		void(*callback)(RPC* rpc, bool result);
	} Data;
	
	void status_setted(bool result, void* context) {
		Data* data = context;
		
		if(list_index_of(clients, data->pcb, NULL) >= 0) {
			data->callback(data->rpc, result);
		}
		free(data);
	}
	
	Data* data = malloc(sizeof(Data));
	data->rpc = rpc;
	data->pcb = context;
	data->callback = callback;
	
	vm_status_set(vmid, status, status_setted, data);
}

static void storage_download_handler(RPC* rpc, uint32_t vmid, uint32_t offset, int32_t size, void* context, void(*callback)(RPC* rpc, void* buf, int32_t size)) {
	if(size < 0) {
		callback(rpc, NULL, size);
	} else {
		void* buf;
		size = vm_storage_read(vmid, &buf, offset, size);
		callback(rpc, buf, size);
	}
}

static void storage_upload_handler(RPC* rpc, uint32_t vmid, uint32_t offset, void* buf, int32_t size, void* context, void(*callback)(RPC* rpc, int32_t size)) {
	if(size < 0) {
		callback(rpc, size);
	} else {
		if(offset == 0) {
			ssize_t len;
			if((len = vm_storage_clear(vmid)) < 0) {
				callback(rpc, len);
				return;
			}
		}
		
		if(size < 0) {
			printf(". Aborted: %d\n", size);
			callback(rpc, size);
		} else {
			size = vm_storage_write(vmid, buf, offset, size);
			callback(rpc, size);
			 
			if(size > 0)
				printf(".");
			else if(size == 0)
				printf(". Done\n");
			else if(size < 0)
				printf(". Error: %d\n", size);
		}
	}
}

static void stdio_handler(RPC* rpc, uint32_t id, uint8_t thread_id, int fd, char* str, uint16_t size, void* context, void(*callback)(RPC* rpc, uint16_t size)) {
	ssize_t len = vm_stdio(id, thread_id, fd, str, size);
	callback(rpc, len >= 0 ? len : 0);
}

// PCB utility
static int pcb_read(RPC* rpc, void* buf, int size) {
	RPCData* data = (RPCData*)rpc->data;
	
	int idx = 0;
	ListIterator iter;
	list_iterator_init(&iter, data->pbufs);
	while(list_iterator_has_next(&iter)) {
		struct pbuf* pbuf = list_iterator_next(&iter);
		if(idx + pbuf->len <= size) {
			memcpy(buf + idx, pbuf->payload, pbuf->len);
			idx += pbuf->len;
			size -= pbuf->len;
			pbuf_free(pbuf);
			list_iterator_remove(&iter);
		} else {
			break;
		}
	}

	return idx;
}

static int pcb_write(RPC* rpc, void* buf, int size) {
	RPCData* data = (RPCData*)rpc->data;
	
	int len = tcp_sndbuf(data->pcb);
	len = len > size ? size : len;

	if(tcp_write(data->pcb, buf, len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
		errno = -1;
		return -1;
	}

	if(tcp_output(data->pcb) != ERR_OK) {
		errno = -2;
		return -1;
	}

	return len;
}

static void pcb_close(RPC* rpc) {
	RPCData* data = (RPCData*)rpc->data;
	manager_close(data->pcb, rpc, false);
}

static err_t manager_accept(void* arg, struct tcp_pcb* pcb, err_t err) {
	struct tcp_pcb_listen* server = arg;
	tcp_accepted(server);
	printf("Accepted: %p\n", pcb);
	
	RPC* rpc = malloc(sizeof(RPC) + sizeof(RPCData));
	bzero(rpc, sizeof(RPC) + sizeof(RPCData));
	rpc->read = pcb_read;
	rpc->write = pcb_write;
	rpc->close = pcb_close;
	
	rpc_vm_create_handler(rpc, vm_create_handler, NULL);
	rpc_vm_get_handler(rpc, vm_get_handler, NULL);
	rpc_vm_set_handler(rpc, vm_set_handler, NULL);
	rpc_vm_delete_handler(rpc, vm_delete_handler, NULL);
	rpc_vm_list_handler(rpc, vm_list_handler, NULL);
	rpc_status_get_handler(rpc, status_get_handler, NULL);
	rpc_status_set_handler(rpc, status_set_handler, pcb);
	rpc_storage_download_handler(rpc, storage_download_handler, NULL);
	rpc_storage_upload_handler(rpc, storage_upload_handler, NULL);
	rpc_stdio_handler(rpc, stdio_handler, NULL);
	
	RPCData* data = (RPCData*)rpc->data;
	data->pcb = pcb;
	data->pbufs = list_create(NULL);
	
	tcp_arg(pcb, rpc);
	tcp_recv(pcb, manager_recv);
	tcp_err(pcb, manager_err);
	tcp_poll(pcb, manager_poll, 2);
	
	list_add(clients, pcb);
	
	return ERR_OK;
}

// RPC Manager
static Packet* manage(Packet* packet) {
	if(shell_process(packet))
		return NULL;
//	else if(rpc_process(packet))
//		return NULL;
//	else if(tftp_process(packet))
//		return NULL;
	
	return packet;
}

static void stdio_callback(uint32_t vmid, int thread_id, int fd, char* buffer, volatile size_t* head, volatile size_t* tail, size_t size) {
	ListIterator iter;
	list_iterator_init(&iter, clients);
	while(list_iterator_has_next(&iter)) {
		struct tcp_pcb* pcb = list_iterator_next(&iter);
		RPC* rpc = pcb->callback_arg;
		
		if(*head <= *tail) {
			size_t len0 = *tail - *head;
			
			// TODO: check missed data (via callback);
			rpc_stdio(rpc, vmid, thread_id, fd, buffer + *head, len0, NULL, NULL);
		} else {
			size_t len1 = size - *head;
			size_t len2 = *tail;
			
			// TODO: check missed data (via callback);
			rpc_stdio(rpc, vmid, thread_id, fd, buffer + *head, len1, NULL, NULL);
			rpc_stdio(rpc, vmid, thread_id, fd, buffer, len2, NULL, NULL);
		}
		
		rpc_loop(rpc);
	}
	
	if(*head <= *tail) {
		size_t len0 = *tail - *head;
		
		*head += len0;
	} else {
		size_t len1 = size - *head;
		size_t len2 = *tail;
		
		*head = (*head + len1 + len2) % size;
	}
}

static bool manager_server_open() {
	uint32_t _ip = (uint32_t)(uint64_t)ni_config_get(manager_ni->ni, "ip");
	struct ip_addr ip;
	IP4_ADDR(&ip, (_ip >> 24) & 0xff, (_ip >> 16) & 0xff, (_ip >> 8) & 0xff, (_ip >> 0) & 0xff);
	
	manager_server = tcp_new();
	
	err_t err = tcp_bind(manager_server, &ip, manager_port);
	if(err != ERR_OK) {
		printf("ERROR: Manager cannot bind TCP session: %d\n", err);

		return false;
	}
	
	manager_server = tcp_listen(manager_server);
	tcp_arg(manager_server, manager_server);
	
	printf("Manager started: %d.%d.%d.%d:%d\n", (_ip >> 24) & 0xff, (_ip >> 16) & 0xff, 
		(_ip >> 8) & 0xff, (_ip >> 0) & 0xff, manager_port);
	
	tcp_accept(manager_server, manager_accept);
	
	if(clients == NULL)
		clients = list_create(NULL);

	if(actives == NULL)
		actives = list_create(NULL);

	return true;
}

static bool manager_server_close() {
	ListIterator iter;
	list_iterator_init(&iter, clients);

	while(list_iterator_has_next(&iter)) {
		struct tcp_pcb* pcb = list_iterator_next(&iter);
		RPC* rpc = pcb->callback_arg;
		manager_close(pcb, rpc, true);
	}

	if(tcp_close(manager_server) != ERR_OK) {
		printf("Cannot close manager server: %p", manager_server);

		return false;
	}
	manager_server = NULL;

	return true;
}

void manager_init() {
	uint64_t attrs[] = { 
		NI_MAC, ni_mac, // Physical MAC
		NI_POOL_SIZE, 0x400000,
		NI_INPUT_BANDWIDTH, 1000000000L,
		NI_OUTPUT_BANDWIDTH, 1000000000L,
		NI_INPUT_BUFFER_SIZE, 1024,
		NI_OUTPUT_BUFFER_SIZE, 1024,
		NI_INPUT_ACCEPT_ALL, 1,
		NI_OUTPUT_ACCEPT_ALL, 1,
		NI_NONE
	};
	
	manager_ni = ni_create(attrs);
	ni_config_put(manager_ni->ni, "ip", (void*)(uint64_t)DEFAULT_MANAGER_IP);
	ni_config_put(manager_ni->ni, "gateway", (void*)(uint64_t)DEFAULT_MANAGER_GW);
	ni_config_put(manager_ni->ni, "netmask", (void*)(uint64_t)DEFAULT_MANAGER_NETMASK);
	ni_config_put(manager_ni->ni, "default", (void*)(uint64_t)true);
	manager_port = DEFAULT_MANAGER_PORT;
	
	manager_netif = ni_init(manager_ni->ni, manage, NULL);
	
	manager_server_open();
	
	bool manager_loop(void* context) {
		ni_poll();
		
		if(!list_is_empty(actives)) {
			ListIterator iter;
			list_iterator_init(&iter, actives);
			while(list_iterator_has_next(&iter)) {
				RPC* rpc = list_iterator_next(&iter);
				if(rpc_is_active(rpc)) {
					rpc_loop(rpc);
				} else {
					list_iterator_remove(&iter);
				}
			}
		}
		
		return true;
	}
	
	bool manager_timer(void* context) {
		ni_timer();
		
		return true;
	}
	
	event_idle_add(manager_loop, NULL);
	event_timer_add(manager_timer, NULL, 100000, 100000);
	
	vm_stdio_handler(stdio_callback);
}

uint32_t manager_get_ip() {
	return (uint32_t)(uint64_t)ni_config_get(manager_ni->ni, "ip");
}

void manager_set_ip(uint32_t ip) {
	ni_config_put(manager_ni->ni, "ip", (void*)(uint64_t)ip);

	struct ip_addr ip2;
	IP4_ADDR(&ip2, (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, (ip >> 0) & 0xff);
	netif_set_ipaddr(manager_netif, &ip2);

	manager_server_close();
	manager_server_open();
}

uint16_t manager_get_port() {
	return manager_port;
}

void manager_set_port(uint16_t port) {
	manager_port = port;

	manager_server_close();
	manager_server_open();
}

uint32_t manager_get_gateway() {
	return (uint32_t)(uint64_t)ni_config_get(manager_ni->ni, "gateway");
}

void manager_set_gateway(uint32_t gw) {
	ni_config_put(manager_ni->ni, "gateway", (void*)(uint64_t)gw);

	struct ip_addr gw2;
	IP4_ADDR(&gw2, (gw >> 24) & 0xff, (gw >> 16) & 0xff, (gw >> 8) & 0xff, (gw >> 0) & 0xff);
	netif_set_gw(manager_netif, &gw2);
}

uint32_t manager_get_netmask() {
	return (uint32_t)(uint64_t)ni_config_get(manager_ni->ni, "netmask");
}

void manager_set_netmask(uint32_t nm) {
	ni_config_put(manager_ni->ni, "netmask", (void*)(uint64_t)nm);

	struct ip_addr nm2;
	IP4_ADDR(&nm2, (nm >> 24) & 0xff, (nm >> 16) & 0xff, (nm >> 8) & 0xff, (nm >> 0) & 0xff);
	netif_set_netmask(manager_netif, &nm2);
}
