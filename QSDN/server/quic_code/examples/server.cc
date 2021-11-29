#include <cstdlib>
#include <cassert>
#include <iostream>
#include <algorithm>
#include <memory>
#include <fstream>

#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <openssl/bio.h>
#include <openssl/err.h>

#include "network.h"
#include "debug.h"
#include "util.h"
#include "crypto.h"
#include "shared.h"
#include "http.h"
#include "keylog.h"

#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include "app_server.h"

// #using namespace ngtcp2;

#define MAX 80 
#define SA struct sockaddr

int sockfd, connfd;
struct sockaddr_in cli;
int OVSDB_PORT  = 6640;
int OFL_PORT = 6653;
char *addr;

int ofl_forward_direction_socket_connect(){

    int sockfd = -1;
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(0);
    }

    else
        printf("Socket successfully created..\n");
    bzero(&ofl_servaddr, sizeof(ofl_servaddr));

    // assign IP, PORT 
    ofl_servaddr.sin_family = AF_INET;

    printf("addr is %s\n", addr);
    printf("port is %d\n", OFL_PORT);

    ofl_servaddr.sin_addr.s_addr = inet_addr(addr);
    ofl_servaddr.sin_port = htons(OFL_PORT);
    return sockfd;
}

int ovsdb_forward_direction_socket_connect(){

	int sockfd = -1;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(0);
    }

    else
        printf("Socket successfully created..\n");
    bzero(&ovsdb_servaddr, sizeof(ovsdb_servaddr));

    // assign IP, PORT 
    ovsdb_servaddr.sin_family = AF_INET;
    ovsdb_servaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    ovsdb_servaddr.sin_port = htons(OVSDB_PORT);

    // connect the client socket to server socket 
    if (connect(sockfd, (SA*)&ovsdb_servaddr, sizeof(ovsdb_servaddr)) != 0) {
        printf("connection with the server failed...\n");
        return -1;
    }

    else
        printf("connected to the server..\n");

    return sockfd;
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

	// Change CLI
	if(argc < 5 || argc > 5){
		print_usage(1);
        return 1;
    }

    int quic_port = 0;
    addr = argv[3];
	int mode = 0;

	const char *key_path = "apache-selfsigned.key";
    const char *cert_file = "apache-selfsigned.crt";

    bool qq = true;

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

	int ovsdb_sock = -1;
	int ofl_sock = -1;

	if(mode == 1){
		ofl_sock = ofl_forward_direction_socket_connect();
		if(ofl_sock == -1)
		{
			printf("Socket creation failed in forward direction in ofl\n");
			return 1;
		}
	}
	else if(mode == 2){
		ovsdb_sock = ovsdb_forward_direction_socket_connect();
		if(ovsdb_sock == -1){
			printf("Socket creation failed in forward direction in ovsdb\n");
			return 1;
		}
	}
	else if(mode == 3){
		ofl_sock = ofl_forward_direction_socket_connect();
        if(ofl_sock == -1)
        {
            printf("Socket creation failed in forward direction in ofl\n");
            return 1;
        }
		ovsdb_sock = ovsdb_forward_direction_socket_connect();
        if(ovsdb_sock == -1){
            printf("Socket creation failed in forward direction in ovsdb\n");
            return 1;
        }
	}

    if(start_server(ofl_sock, ovsdb_sock, argv[1], argv[2], key_path, cert_file, qq, mode) < -1)
        printf("Function failed\n");

    return 1;
}

