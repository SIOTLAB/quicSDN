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
// struct sockaddr_in servaddr, cli;
struct sockaddr_in cli;

// #define PORT 6653

int PORT = 0;
char *addr;

int forward_direction_socket_connect(){

	int sockfd = -1;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(0);
    }

    else
        printf("Socket successfully created..\n");
    bzero(&servaddr, sizeof(servaddr));

    // assign IP, PORT 
    servaddr.sin_family = AF_INET;

	printf("addr is %s\n", addr);
	printf("port is %d\n", PORT);

    servaddr.sin_addr.s_addr = inet_addr(addr);
    servaddr.sin_port = htons(PORT);

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

	if(argc < 5 || argc > 5){
        print_usage(1);
        return 1;
    }

	int quic_port = 0;
	sscanf(argv[4], "%d", &PORT);
	addr = argv[3];	

	const char *key_path = "apache-selfsigned.key";
    const char *cert_file = "apache-selfsigned.crt";

	int q = 0;
	bool qq;

	// printf("want it quiet?\n");
	// scanf("%d", &q);

	// qq = q ? true : false;

	qq = false;
	int sock = forward_direction_socket_connect();
	// int sock = 1;
	if(start_server(sock, argv[1], argv[2], key_path, cert_file, qq) < -1)
		printf("Function failed\n");

	return 1;

}
