#include <stdio.h>
#include <winsock2.h>
#include <windows.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE 1024

SOCKET create_listener_socket(int port);
DWORD WINAPI listen_for_peers(LPVOID);
DWORD WINAPI chat_with_peer(LPVOID);

int main() {
    WSADATA wsa;
    SOCKET server_socket;
    struct sockaddr_in server;
    char server_ip[20];
    int server_port, listen_port;
    char buffer[BUFFER_SIZE];

    printf("Enter server IP: ");
    scanf("%s", server_ip);
    printf("Enter server port: ");
    scanf("%d", &server_port);
    printf("Enter your listening port (for peer connections): ");
    scanf("%d", &listen_port);
    getchar(); // flush newline

    WSAStartup(MAKEWORD(2,2), &wsa);

    // Start listening for peer connections
    CreateThread(NULL, 0, listen_for_peers, (LPVOID)(intptr_t)listen_port, 0, NULL);

    // Connect to server
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    server.sin_family = AF_INET;
    server.sin_port = htons(server_port);
    server.sin_addr.s_addr = inet_addr(server_ip);
    connect(server_socket, (struct sockaddr*)&server, sizeof(server));

    // Register port with server
    sprintf(buffer, "%d", listen_port);
    send(server_socket, buffer, strlen(buffer), 0);

    while (1) {
        printf("\nCommands:\n1. LIST - Get client list\n2. CONNECT <ip> <port> - Chat with a client\n> ");
        fgets(buffer, BUFFER_SIZE, stdin);
        buffer[strcspn(buffer, "\n")] = 0;

        if (strncmp(buffer, "LIST", 4) == 0) {
            send(server_socket, buffer, strlen(buffer), 0);
            int len = recv(server_socket, buffer, BUFFER_SIZE, 0);
            buffer[len] = 0;
            printf("Clients:\n%s", buffer);
        } else if (strncmp(buffer, "CONNECT", 7) == 0) {
            char ip[20];
            int port;
            sscanf(buffer + 8, "%s %d", ip, &port);
            SOCKET peer_socket = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in peer;
            peer.sin_family = AF_INET;
            peer.sin_port = htons(port);
            peer.sin_addr.s_addr = inet_addr(ip);

            if (connect(peer_socket, (struct sockaddr*)&peer, sizeof(peer)) == 0) {
                printf("Connected to peer %s:%d\n", ip, port);
                CreateThread(NULL, 0, chat_with_peer, (LPVOID)(intptr_t)peer_socket, 0, NULL);
            } else {
                printf("Could not connect to peer.\n");
                closesocket(peer_socket);
            }
        }
    }

    closesocket(server_socket);
    WSACleanup();
    return 0;
}

DWORD WINAPI listen_for_peers(LPVOID arg) {
    int port = (intptr_t)arg;
    SOCKET listen_socket = create_listener_socket(port);
    struct sockaddr_in client;
    int c = sizeof(client);

    while (1) {
        SOCKET peer_socket = accept(listen_socket, (struct sockaddr*)&client, &c);
        printf("\nPeer connected: %s\n", inet_ntoa(client.sin_addr));
        CreateThread(NULL, 0, chat_with_peer, (LPVOID)(intptr_t)peer_socket, 0, NULL);
    }
    return 0;
}

DWORD WINAPI chat_with_peer(LPVOID arg) {
    SOCKET peer_socket = (SOCKET)(intptr_t)arg;
    char buffer[BUFFER_SIZE];
    while (1) {
        printf("You: ");
        fgets(buffer, BUFFER_SIZE, stdin);
        buffer[strcspn(buffer, "\n")] = 0;
        if (send(peer_socket, buffer, strlen(buffer), 0) <= 0) break;

        int len = recv(peer_socket, buffer, BUFFER_SIZE, 0);
        if (len <= 0) break;
        buffer[len] = 0;
        printf("\nPeer: %s\n", buffer);
    }
    closesocket(peer_socket);
    return 0;
}

SOCKET create_listener_socket(int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in local;

    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(port);

    bind(sock, (struct sockaddr*)&local, sizeof(local));
    listen(sock, 5);
    return sock;
}