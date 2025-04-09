#include <stdio.h>
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

#define INET_ADDRSTRLEN 16 // Define for IPv4 address string length

#pragma comment(lib, "ws2_32.lib")
#define MAX_CLIENTS 100

typedef struct {
    SOCKET socket;
    char ip[INET_ADDRSTRLEN];
    int port;
} Client;

Client clients[MAX_CLIENTS];
int client_count = 0;
CRITICAL_SECTION cs;

DWORD WINAPI handle_client(LPVOID arg);

int main() {
    WSADATA wsa;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server, client;
    int c = sizeof(struct sockaddr_in);

    InitializeCriticalSection(&cs);

    printf("Starting server...\n");

    WSAStartup(MAKEWORD(2,2), &wsa);
    server_socket = socket(AF_INET, SOCK_STREAM, 0);

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(9000);

    bind(server_socket, (struct sockaddr *)&server, sizeof(server));
    listen(server_socket, 5);
    printf("Server listening on port 9000\n");

    while ((client_socket = accept(server_socket, (struct sockaddr *)&client, &c)) != INVALID_SOCKET) {
        DWORD threadId;
        CreateThread(NULL, 0, handle_client, (LPVOID)(intptr_t)client_socket, 0, &threadId);
    }

    DeleteCriticalSection(&cs);
    closesocket(server_socket);
    WSACleanup();
    return 0;
}

DWORD WINAPI handle_client(LPVOID arg) {
    SOCKET client_socket = (SOCKET)(intptr_t)arg;
    char buffer[512];
    char client_ip[INET_ADDRSTRLEN];
    int port;

    recv(client_socket, buffer, sizeof(buffer), 0); // expecting port
    sscanf(buffer, "%d", &port);

    struct sockaddr_in addr;
    int len = sizeof(addr);
    getpeername(client_socket, (struct sockaddr *)&addr, &len);
    strcpy(client_ip, inet_ntoa(addr.sin_addr));

    EnterCriticalSection(&cs);
    clients[client_count].socket = client_socket;
    strcpy(clients[client_count].ip, client_ip);
    clients[client_count].port = port;
    client_count++;
    LeaveCriticalSection(&cs);

    printf("Registered client %s:%d\n", client_ip, port);

    while (1) {
        int bytes = recv(client_socket, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break;

        if (strncmp(buffer, "LIST", 4) == 0) {
            char response[1024] = "";
            EnterCriticalSection(&cs);
            for (int i = 0; i < client_count; i++) {
                char entry[64];
                sprintf(entry, "%s:%d\n", clients[i].ip, clients[i].port);
                strcat(response, entry);
            }
            LeaveCriticalSection(&cs);
            send(client_socket, response, strlen(response), 0);
        }
    }

    closesocket(client_socket);
    return 0;
}