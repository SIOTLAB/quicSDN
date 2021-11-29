#include <cstdlib>
#include <cassert>
#include <cerrno>
#include <iostream>
#include <algorithm>
#include <memory>
#include <fstream>

#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/mman.h>

#include <openssl/bio.h>
#include <openssl/err.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include "app_client.h"
#include "network.h"
#include "debug.h"
#include "util.h"
#include "crypto.h"
#include "shared.h"
#include "keylog.h"

#include <resolv.h>
#include "openssl/ssl.h"
#include "openssl/err.h"
#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>

#include <thread> 

#define MAX 80 
#define OFL_PORT 6653
#define OVSDB_PORT 6640
#define SA struct sockaddr 
#define FAIL    -1

int sock_ofl = -1;
int sock_ovsdb = -1;

char *addr_udp = NULL;
char *port_udp = NULL;


using namespace ngtcp2;

std::deque<uint8_t *> forward_deque;

int connfd_sock, len_sock;
struct sockaddr_in servaddr_sock, cli_sock;

struct sockaddr_in ovsdb_servaddr_sock;

#define check(expr) if (!(expr)) { perror(#expr); kill(0, SIGTERM); }

void make_non_blocking(int fd){
	int current_options = 0;

	current_options = fcntl(fd, F_GETFL); //get fd flags
	if(current_options == -1){
		fprintf(stderr, "Error getting options for fd: %d\n", fd);
	}
	current_options |= O_NONBLOCK; //add nonblocking
	if(fcntl(fd, F_SETFL, current_options) == -1){
		fprintf(stderr, "Error setting options for fd: %d\n", fd);
	}
}

int open_udp_local_socket()
{
	int sockfd;
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        return 0;
    }
    else
        printf("Socket successfully created..\n");
    
	int enable = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    {	
		printf("setsockopt(SO_REUSEADDR) failed");
		return 0;
	}

	bzero(&servaddr_sock, sizeof(servaddr_sock));

    // assign IP, PORT 
    servaddr_sock.sin_family = AF_INET;
	// servaddr_sock.sin_addr.s_addr = inet_addr("127.0.0.1");
    servaddr_sock.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr_sock.sin_port = htons(OFL_PORT);

    // Binding newly created socket to given IP and verification 
    if ((bind(sockfd, (SA*)&servaddr_sock, sizeof(servaddr_sock))) != 0) {
        printf("socket bind failed...\n");
        return 0;
    }
    else
        printf("Socket successfully binded..\n");

	
	char buf[4096] = {'\0'};
	socklen_t len = sizeof(struct sockaddr_in);
	while(recvfrom(sockfd, buf, 4096, 0,
			     (struct sockaddr *)&openflow_cliaddr, &len) < 0)

	printf("OpenFlow OVS connected\n");
	printf("OpenFlow OVS IP address is %s\n", inet_ntoa(openflow_cliaddr.sin_addr));
    printf("Picked up OVS is %d\n", openflow_cliaddr.sin_port);

    return sockfd;
}

int open_ovsdb_socket()
{
	int sockfd;
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		printf("socket creation failed...\n");
		return 0;
	}
	else
		printf("Socket successfully created..\n");
  
	int enable = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
	{
		printf("setsockopt(SO_REUSEADDR) failed");	
		return 0;
	}

	int flags = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags))) 
	{ 
		printf("ERROR: setsocketopt(), SO_KEEPALIVE"); 
		return 0; 
	}

	int maxpkt = 10;
	if(setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &maxpkt, sizeof(int)) == -1){
		printf("Error: setsockopt failed\n");
		return 0;
	}

	bzero(&ovsdb_servaddr_sock, sizeof(ovsdb_servaddr_sock));

    // assign IP, PORT 
    ovsdb_servaddr_sock.sin_family = AF_INET;
	ovsdb_servaddr_sock.sin_addr.s_addr = inet_addr("127.0.0.1");
    ovsdb_servaddr_sock.sin_port = htons(OVSDB_PORT);

    // Binding newly created socket to given IP and verification 
    if ((bind(sockfd, (SA*)&ovsdb_servaddr_sock, sizeof(ovsdb_servaddr_sock))) != 0) {
        printf("socket bind failed...\n");
        return 0;
    }
    else
        printf("Socket successfully binded..\n");

    // Now server is ready to listen and verification 
    if ((listen(sockfd, 5)) != 0) {
        printf("Listen failed...\n");
        return 0;
    }
    else
        printf("Server listening..\n");

    len_sock = sizeof(ovsdb_cliaddr);
    // Accept the data packet from client and verification 
    connfd_sock = accept(sockfd, (SA*)&ovsdb_cliaddr, (socklen_t *)&len_sock);
    if (connfd_sock < 0) {
        printf("server acccept failed...\n");
        return 0;
    }
    else
        printf("server acccept the client...\n");

	make_non_blocking(connfd_sock);
	printf("Returning after accepting the socket on port %d\n", ovsdb_cliaddr.sin_port);
   
   

    return connfd_sock;
}


void print_usage(bool is_server) {

	printf("==================Usage===================\n");
	if(is_server)
		printf("./server <quic_addr> <quic_port> <tcp_addr> <tcp_port>\n\n\n");
	else
		printf("./client <quic_addr> <quic_port> <tcp_addr> <tcp_port>\n\n\n");

	printf("<quic_addr>: If quic server then addr on which server needs to be run otherwise\n");
	printf("addr which client trying to connect\n");
	printf("===================================\n");
	printf("<quic_port>: If quic server then port on which server needs to run\n");
	printf("Otherwise port which client is trying to reach\n");
	printf("===================================\n");
	printf("<tcp_addr>: give 127.0.0.1 \n");
	printf("<tcp_port>: Giveive the port on which controller is running\n");
}

int main(int argc, char *argv[]) {

	int mode = 0;
	int q;
	bool qq;

	if(argc < 3){
		print_usage(0);
		return 1;
	}		

    addr_udp = argv[1];
	port_udp = argv[2];

//	printf("Do you want quiet mode? \n");
//	scanf("%d", &q);
//	qq = q ? true : false;
	qq = true;

	while(1){
		printf("Select your choice\n");
		printf("\t 1. Run OpenFlow Mode \n");
		printf("\t 2. Run OVSDB Mode \n");
		printf("\t 3. Run both OpenFlow and OVSDB mode together\n");
		printf("\t 4. Explanation about above three mode\n");
		scanf("%d", &mode);
		if(mode == 4){
			printf("\t OpenFlow mode only runs OpenFlow protocol over QUIC\n");
			printf("\t OVSDB mode only runs OVSDB protocol over QUIC\n");
			printf("\t OVSDB and OpenFlow mode runs both OpenFlow and OVSDB protocols multiplexed over QUIC\n");
		}
		else {
			if(mode == 1 || mode == 2 || mode == 3)
				break;
			else
				printf("\t Please enter a valid choice\n");
		}
	}

	if(mode == 1){
		sock_ofl =open_udp_local_socket();
		if(sock_ofl == 0) {
			printf("udp local socket didn't opened\n");
			return 1;
		}
	}

	else if(mode == 2) {
		sock_ovsdb = open_ovsdb_socket();
		if(sock_ovsdb == 0) {
        	printf("udp local socket didn't opened\n");
			return 1;
		}
	}
	else if(mode == 3){
		sock_ofl =open_udp_local_socket();
		if(sock_ofl == 0) {
			printf("udp local socket didn't opened\n");
			return 1;
		}
		sock_ovsdb = open_ovsdb_socket();
			if(sock_ovsdb == 0) {
			printf("udp local socket didn't opened\n");
			return 1;
		}
	}

	if(start_client(sock_ofl, sock_ovsdb, addr_udp, port_udp, mode) != 0){
		return 1;
	}

	if(sock_ofl != -1)
		close(sock_ofl);
	if(sock_ovsdb != -1)
		close(sock_ovsdb);
	
	return 1;
}
