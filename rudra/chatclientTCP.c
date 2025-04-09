#include <stdio.h>
#include <winsock2.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE 1024

int main() {
    WSADATA wsa;
    SOCKET client_socket;
    struct sockaddr_in server_addr;
    char server_ip[16];
    int server_port;
    char buffer[BUFFER_SIZE];
    int recv_size;

    // Get server IP and port from user
    printf("Enter server IP address: ");
    fgets(server_ip, sizeof(server_ip), stdin);
    server_ip[strcspn(server_ip, "\n")] = 0; // Remove newline

    printf("Enter server port: ");
    scanf("%d", &server_port);
    getchar(); // Clear newline after scanf

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock. Error Code: %d\n", WSAGetLastError());
        return 1;
    }

    // Create socket
    if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("Socket creation failed. Error Code: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Setup server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    // Connect to server
    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Connection failed. Error Code: %d\n", WSAGetLastError());
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    printf("Connected to server at %s:%d\n", server_ip, server_port);

    // Chat loop
    while (1) {
        printf("Enter message (type 'exit' to close): ");
        fgets(buffer, BUFFER_SIZE, stdin);
        buffer[strcspn(buffer, "\n")] = 0; // Remove newline

        // Send to server
        if (send(client_socket, buffer, strlen(buffer), 0) == SOCKET_ERROR) {
            printf("Send failed. Error Code: %d\n", WSAGetLastError());
            break;
        }

        if (strcmp(buffer, "exit") == 0) {
            printf("Exiting chat.\n");
            break;
        }

        // Receive from server
        recv_size = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (recv_size == SOCKET_ERROR) {
            printf("Receive failed. Error Code: %d\n", WSAGetLastError());
            break;
        } else if (recv_size == 0) {
            printf("Server disconnected.\n");
            break;
        }

        buffer[recv_size] = 0; // Null-terminate
        printf("Server: %s\n", buffer);

        if (strcmp(buffer, "exit") == 0) {
            printf("Server closed the connection.\n");
            break;
        }
    }

    // Cleanup
    closesocket(client_socket);
    WSACleanup();
    return 0;
}
