// server.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <process.h> // For _beginthreadex
#include <string.h> // For strchr

#pragma comment(lib, "ws2_32.lib")

#define SERVER_PORT 9000
#define MAX_CLIENTS 100
#define BUFFER_SIZE 2048 // Increased buffer size for messages
#define INET_ADDRSTRLEN 16

typedef struct {
    int id;
    SOCKET socket; // This is the socket connected TO THE SERVER
    char ip[INET_ADDRSTRLEN];
    // int port; // Listening port no longer needed
    HANDLE thread_handle;
    int active; // Flag to mark if the slot is in use
} Client;

Client clients[MAX_CLIENTS];
// int client_count = 0; // Not reliable for active count with inactive slots
int next_client_id = 1;
CRITICAL_SECTION cs;

// --- Function Prototypes ---
unsigned __stdcall handle_client(void *arg);
void remove_client(SOCKET client_socket);
void send_message_to_client(int target_id, const char* message, int sender_id);
void broadcast_info(const char* message, int exclude_id);

// --- Main Function ---
int main() {
    WSADATA wsa;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server, client;
    int c = sizeof(struct sockaddr_in);

    printf("Initializing Winsock...\n");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed. Error Code: %d\n", WSAGetLastError());
        return 1;
    }
    printf("Winsock Initialized.\n");

    InitializeCriticalSection(&cs);

    for(int i = 0; i < MAX_CLIENTS; ++i) {
        clients[i].active = 0;
        clients[i].socket = INVALID_SOCKET;
        clients[i].id = -1;
    }

    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        printf("Could not create socket. Error Code: %d\n", WSAGetLastError());
        WSACleanup(); return 1;
    }
    printf("Server socket created.\n");

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(SERVER_PORT);

    if (bind(server_socket, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
        printf("Bind failed. Error Code: %d\n", WSAGetLastError());
        closesocket(server_socket); WSACleanup(); return 1;
    }
    printf("Socket bound to port %d.\n", SERVER_PORT);

    if (listen(server_socket, 5) == SOCKET_ERROR) {
        printf("Listen failed. Error Code: %d\n", WSAGetLastError());
        closesocket(server_socket); WSACleanup(); return 1;
    }

    printf("Server listening on port %d...\n", SERVER_PORT);

    while ((client_socket = accept(server_socket, (struct sockaddr*)&client, &c)) != INVALID_SOCKET) {
        printf("Connection accepted from %s:%d\n", inet_ntoa(client.sin_addr), ntohs(client.sin_port));

        uintptr_t client_socket_ptr = (uintptr_t)client_socket;
        HANDLE threadHandle = (HANDLE)_beginthreadex(NULL, 0, handle_client, (void*)client_socket_ptr, 0, NULL);

        if (threadHandle == NULL) {
             printf("Failed to create thread for client. Error code: %d\n", GetLastError());
             closesocket(client_socket);
        } else {
             CloseHandle(threadHandle); // Detach thread handle if not needed later
        }
    }

    if (client_socket == INVALID_SOCKET) {
        printf("Accept failed. Error Code: %d\n", WSAGetLastError());
    }

    printf("Shutting down server...\n");
    DeleteCriticalSection(&cs);
    closesocket(server_socket);
    WSACleanup();
    return 0;
}

// --- Client Handling Thread ---
unsigned __stdcall handle_client(void *arg) {
    SOCKET client_socket = (SOCKET)(uintptr_t)arg;
    char buffer[BUFFER_SIZE];
    char client_ip[INET_ADDRSTRLEN];
    int current_client_id = -1;
    int current_client_index = -1;


    // 1. Get client IP (for logging/info)
    struct sockaddr_in addr;
    int len = sizeof(addr);
    if (getpeername(client_socket, (struct sockaddr*)&addr, &len) == SOCKET_ERROR) {
         printf("getpeername failed. Error: %d\n", WSAGetLastError());
         closesocket(client_socket);
         return 1;
    }
    strcpy(client_ip, inet_ntoa(addr.sin_addr));

    // 2. Register the client
    EnterCriticalSection(&cs);
    // Find an inactive slot
    for(int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].active) {
            clients[i].id = next_client_id++;
            clients[i].socket = client_socket;
            strcpy(clients[i].ip, client_ip);
            // clients[i].port = 0; // Port no longer needed
            clients[i].active = 1;
            clients[i].thread_handle = GetCurrentThread(); // Store handle (rarely needed)
            current_client_id = clients[i].id;
            current_client_index = i;
            printf("Registered client ID %d (%s)\n", current_client_id, client_ip);
            break;
        }
    }
    LeaveCriticalSection(&cs);

    if (current_client_id == -1) {
        printf("Server full. Cannot register client %s\n", client_ip);
        const char *full_msg = "ERROR Server is full.";
        send(client_socket, full_msg, strlen(full_msg), 0);
        closesocket(client_socket);
        return 1; // Exit thread
    }


    // 3. Send the assigned ID back to the client (as confirmation)
    sprintf(buffer, "ID %d", current_client_id); // Send ID clearly marked
    if (send(client_socket, buffer, strlen(buffer), 0) == SOCKET_ERROR) {
        printf("Failed to send ID to client %d. Error: %d\n", current_client_id, WSAGetLastError());
        remove_client(client_socket); // Use remove_client to clean up
        return 1;
    }

    // 3.5 Broadcast join message
    sprintf(buffer, "INFO User %d (%s) has joined.", current_client_id, client_ip);
    broadcast_info(buffer, current_client_id); // Inform others

    // 4. Command loop (LIST, SEND)
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received == SOCKET_ERROR || bytes_received == 0) {
            if (bytes_received == 0) {
                 printf("Client ID %d disconnected gracefully.\n", current_client_id);
            } else {
                 printf("recv failed for client ID %d. Error: %d. Closing connection.\n", current_client_id, WSAGetLastError());
            }
            break; // Exit loop, will trigger cleanup
        }

        buffer[bytes_received] = '\0'; // Null-terminate received data

        // Handle LIST command
        if (_stricmp(buffer, "LIST") == 0) {
            char response[BUFFER_SIZE * 2] = "";
            EnterCriticalSection(&cs);
            strcat(response, "--- Active Clients ---\n");
            int active_count = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active) {
                    char entry[128];
                    sprintf(entry, "ID: %d (%s) %s\n", clients[i].id, clients[i].ip, (clients[i].id == current_client_id) ? "(You)" : "");
                     if (strlen(response) + strlen(entry) < sizeof(response) -1) {
                       strcat(response, entry);
                    } else {
                        strcat(response, "... (list truncated)\n"); break;
                    }
                    active_count++;
                }
            }
             if (active_count == 0) strcat(response, "(No clients connected? Problem!)\n"); // Should at least see self
             strcat(response, "----------------------\n");
            LeaveCriticalSection(&cs);

            if (send(client_socket, response, strlen(response), 0) == SOCKET_ERROR) {
                 printf("Failed to send list to client ID %d. Error: %d\n", current_client_id, WSAGetLastError());
            }
        }
        // Handle SEND <id> <message> command
        else if (_strnicmp(buffer, "SEND ", 5) == 0) {
            int target_id;
            char *message_start = strchr(buffer + 5, ' '); // Find space after ID
            if (message_start != NULL && sscanf(buffer + 5, "%d", &target_id) == 1) {
                message_start++; // Move past the space to the actual message
                if (strlen(message_start) > 0) {
                    // Relay the message
                    send_message_to_client(target_id, message_start, current_client_id);
                } else {
                     sprintf(buffer, "ERROR Message cannot be empty.");
                     send(client_socket, buffer, strlen(buffer), 0);
                }
            } else {
                 // Invalid format
                 sprintf(buffer, "ERROR Invalid SEND format. Use: SEND <id> <message>");
                 send(client_socket, buffer, strlen(buffer), 0);
            }
        } else {
            // Unknown command
             printf("Client ID %d sent unknown command: %s\n", current_client_id, buffer);
             sprintf(buffer, "ERROR Unknown command. Use LIST or SEND <id> <message>");
             send(client_socket, buffer, strlen(buffer), 0);
        }
    }

    // 5. Cleanup: Announce departure and remove client
    sprintf(buffer, "INFO User %d (%s) has left.", current_client_id, client_ip);
    broadcast_info(buffer, current_client_id); // Inform others before removing
    remove_client(client_socket);
    return 0; // Thread exits
}


// --- Utility Functions ---

// Function to find and remove a client by socket descriptor
void remove_client(SOCKET client_socket) {
    int removed_id = -1;
    EnterCriticalSection(&cs);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].socket == client_socket) {
            removed_id = clients[i].id;
            printf("Removing client ID %d (%s)\n", clients[i].id, clients[i].ip);
            closesocket(clients[i].socket); // Close the socket
            clients[i].active = 0; // Mark as inactive
            clients[i].id = -1;
            clients[i].socket = INVALID_SOCKET;
            break;
        }
    }
    LeaveCriticalSection(&cs);
}

// Function to send a message to a specific client ID
void send_message_to_client(int target_id, const char* message, int sender_id) {
    SOCKET target_socket = INVALID_SOCKET;
    char formatted_message[BUFFER_SIZE + 64]; // Extra space for formatting

    // Find the target client's socket
    EnterCriticalSection(&cs);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].id == target_id) {
            target_socket = clients[i].socket;
            break;
        }
    }
    LeaveCriticalSection(&cs);

    // Format message: MSG <sender_id>: <message>
    sprintf(formatted_message, "MSG %d: %s", sender_id, message);

    if (target_socket != INVALID_SOCKET) {
        // Send the message to the target client
        if (send(target_socket, formatted_message, strlen(formatted_message), 0) == SOCKET_ERROR) {
            printf("Failed to relay message from %d to %d. Error: %d\n", sender_id, target_id, WSAGetLastError());
            // Optional: Inform sender? Handle potential disconnection of target here?
            // If send fails, the target might have disconnected concurrently.
            // remove_client(target_socket) could be called, but be careful about recursion/deadlock.
        }
    } else {
        // Target client not found, inform the original sender
        SOCKET sender_socket = INVALID_SOCKET;
        sprintf(formatted_message, "ERROR User ID %d not found or is inactive.", target_id);

        EnterCriticalSection(&cs);
         for (int i = 0; i < MAX_CLIENTS; i++) {
             if (clients[i].active && clients[i].id == sender_id) {
                 sender_socket = clients[i].socket;
                 break;
             }
         }
        LeaveCriticalSection(&cs);

        if (sender_socket != INVALID_SOCKET) {
            send(sender_socket, formatted_message, strlen(formatted_message), 0);
        }
    }
}

// Function to broadcast an informational message to all active clients (except one)
void broadcast_info(const char* message, int exclude_id) {
     printf("Broadcasting: %s (excluding %d)\n", message, exclude_id);
     EnterCriticalSection(&cs);
     for (int i = 0; i < MAX_CLIENTS; i++) {
          if (clients[i].active && clients[i].id != exclude_id) {
               if (send(clients[i].socket, message, strlen(message), 0) == SOCKET_ERROR) {
                    // Handle error, maybe client disconnected during broadcast
                    printf("Broadcast failed for client %d. Error: %d\n", clients[i].id, WSAGetLastError());
                    // Consider removing the client if send fails consistently
               }
          }
     }
     LeaveCriticalSection(&cs);
}