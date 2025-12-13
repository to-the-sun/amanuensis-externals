#include "visualize.h"
#include <winsock2.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#define UDP_PORT 9999
#define SERVER "127.0.0.1"

static SOCKET sock;
static struct sockaddr_in server_addr;

int visualize_init() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        return 1;
    }

    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        return 1;
    }

    memset((char *) &server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_PORT);
    server_addr.sin_addr.S_un.S_addr = inet_addr(SERVER);
    
    return 0;
}

void visualize_cleanup() {
    closesocket(sock);
    WSACleanup();
}

void visualize(const char *message) {
    sendto(sock, message, strlen(message), 0, (struct sockaddr *) &server_addr, sizeof(server_addr));
}
