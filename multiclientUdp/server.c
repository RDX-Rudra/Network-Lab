// server_udp.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h> // Required for sockaddr_in definition
#include <windows.h>
#include <stdint.h>
#include <process.h>
#include <string.h>
#include <time.h>     // For timeout checking

#pragma comment(lib, "ws2_32.lib")

#define SERVER_PORT 9001 // Use a different port than TCP version maybe
#define MAX_CLIENTS 100
#define BUFFER_SIZE 2048
#define BROADCAST_ID 101
#define CLIENT_TIMEOUT_SECONDS 60 // Inactivity threshold

typedef struct {
    int id;
    struct sockaddr_in addr; // Store client address (IP + Port)
    char ip_str[INET_ADDRSTRLEN]; // Store string version for convenience
    time_t last_heard_time;   // For timeout detection
    int active;               // Flag if slot is used
} ClientInfoUDP;

ClientInfoUDP clients[MAX_CLIENTS];
int next_client_id = 1;
CRITICAL_SECTION cs;
SOCKET server_socket = INVALID_SOCKET; // Global server socket

// --- Function Prototypes ---
void initialize_clients();
int find_client_by_addr(const struct sockaddr_in* addr);
int register_client(const struct sockaddr_in* addr);
void update_client_time(int client_index);
void remove_client(int client_index);
void process_datagram(char* buffer, int len, const struct sockaddr_in* client_addr);
void send_to_client_addr(const struct sockaddr_in* addr, const char* message);
void send_message_to_client_id(int target_id, const char* message, int sender_id, const struct sockaddr_in* sender_addr);
void broadcast_message(const char* message, int sender_id, const struct sockaddr_in* sender_addr);
void broadcast_info(const char* message, const struct sockaddr_in* exclude_addr);
unsigned __stdcall check_timeouts_thread(void *arg);

// --- Main Function ---
int main() {
    WSADATA wsa;
    struct sockaddr_in server_addr;

    printf("Initializing Winsock...\n");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed. Error Code: %d\n", WSAGetLastError()); return 1;
    }
    printf("Winsock Initialized.\n");

    InitializeCriticalSection(&cs);
    initialize_clients();

    // Create UDP socket
    server_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_socket == INVALID_SOCKET) {
        printf("Could not create socket. Error Code: %d\n", WSAGetLastError());
        WSACleanup(); return 1;
    }
    printf("UDP Server socket created.\n");

    // Prepare the sockaddr_in structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces
    server_addr.sin_port = htons(SERVER_PORT);

    // Bind the socket
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Bind failed on port %d. Error Code: %d\n", SERVER_PORT, WSAGetLastError());
        closesocket(server_socket); WSACleanup(); return 1;
    }
    printf("Socket bound to port %d.\n", SERVER_PORT);
    printf("UDP Server listening on port %d...\n", SERVER_PORT);
    printf("Broadcast ID is %d. Client timeout is %d seconds.\n", BROADCAST_ID, CLIENT_TIMEOUT_SECONDS);

    // Start timeout checker thread
    HANDLE timeoutThreadHandle = (HANDLE)_beginthreadex(NULL, 0, check_timeouts_thread, NULL, 0, NULL);
     if (timeoutThreadHandle == NULL) {
          printf("Failed to create timeout checker thread. Error: %d\n", GetLastError());
          // Continue without timeout checking? Or exit? For simplicity, continue.
     } else {
          CloseHandle(timeoutThreadHandle); // Detach
     }


    // Main receive loop
    char recv_buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);

    while (1) {
        memset(recv_buffer, 0, BUFFER_SIZE);
        int bytes_received = recvfrom(server_socket, recv_buffer, BUFFER_SIZE - 1, 0,
                                     (struct sockaddr*)&client_addr, &client_addr_len);

        if (bytes_received == SOCKET_ERROR) {
            int error = WSAGetLastError();
            // WSAECONNRESET can happen in UDP, often ignored
            if (error == WSAECONNRESET) {
                printf("WSAECONNRESET received (normal for UDP sometimes).\n");
                continue;
            }
             // WSAEINTR might happen if the socket is closed while recvfrom is blocking
            if (error == WSAEINTR || error == WSAENOTSOCK || error == WSAEINVAL) {
                printf("recvfrom interrupted or socket closed. Shutting down? Error: %d\n", error);
                break; // Exit loop
            }
            printf("recvfrom failed. Error Code: %d\n", error);
            // Consider adding a small delay before retrying on other errors
            Sleep(10);
            continue; // Continue listening
        }

        if (bytes_received > 0) {
             recv_buffer[bytes_received] = '\0'; // Null-terminate
             // Process the received datagram
             process_datagram(recv_buffer, bytes_received, &client_addr);
        }
    }

    printf("Shutting down server...\n");
    DeleteCriticalSection(&cs);
    if (server_socket != INVALID_SOCKET) closesocket(server_socket);
    WSACleanup();
    return 0;
}

// --- Client Management Functions ---

void initialize_clients() {
    EnterCriticalSection(&cs);
    for(int i = 0; i < MAX_CLIENTS; ++i) {
        clients[i].active = 0;
        clients[i].id = -1;
    }
    LeaveCriticalSection(&cs);
}

// Find client index by address. Returns index or -1 if not found.
int find_client_by_addr(const struct sockaddr_in* addr) {
    // No need for critical section here if only reading IDs/comparing addrs,
    // but safer to include if last_heard_time might be updated elsewhere.
    // Let's assume reads are safe without CS for performance in this specific function.
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].active &&
            clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            clients[i].addr.sin_port == addr->sin_port) {
            return i; // Found
        }
    }
    return -1; // Not found
}

// Register a new client or return existing index. Returns client ID or -1 on failure.
int register_client(const struct sockaddr_in* addr) {
    EnterCriticalSection(&cs);
    int client_index = -1;
    // Check if already registered
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].active &&
            clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            clients[i].addr.sin_port == addr->sin_port) {
             client_index = i;
             break;
        }
    }

    // If not found, find an inactive slot
    if (client_index == -1) {
         for (int i = 0; i < MAX_CLIENTS; ++i) {
             if (!clients[i].active) {
                 // Skip broadcast ID if needed
                 if (next_client_id == BROADCAST_ID) next_client_id++;

                 clients[i].id = next_client_id++;
                 clients[i].addr = *addr; // Copy the address structure
                 strcpy(clients[i].ip_str, inet_ntoa(addr->sin_addr));
                 clients[i].last_heard_time = time(NULL); // Set current time
                 clients[i].active = 1;
                 client_index = i;
                 printf("Registered new client ID %d from %s:%d\n", clients[i].id, clients[i].ip_str, ntohs(addr->sin_port));
                 break;
             }
         }
    }

    int client_id = (client_index != -1) ? clients[client_index].id : -1;
    LeaveCriticalSection(&cs);

    if (client_id != -1 && client_index != -1) {
         // Update time even if found (implicitly done by calling this function on message receipt)
         update_client_time(client_index);
    }

    return client_id; // Return assigned/found ID or -1 if server full
}

void update_client_time(int client_index) {
    if (client_index < 0 || client_index >= MAX_CLIENTS) return;
    EnterCriticalSection(&cs);
    if (clients[client_index].active) {
        clients[client_index].last_heard_time = time(NULL);
    }
    LeaveCriticalSection(&cs);
}

// Marks a client as inactive (e.g., due to timeout)
void remove_client(int client_index) {
    if (client_index < 0 || client_index >= MAX_CLIENTS) return;
    char info_buffer[128];
    int removed_id = -1;
    struct sockaddr_in removed_addr;

    EnterCriticalSection(&cs);
    if (clients[client_index].active) {
        removed_id = clients[client_index].id;
        removed_addr = clients[client_index].addr; // Copy before marking inactive
        printf("Removing client ID %d (%s:%d) due to timeout or error.\n",
               clients[client_index].id, clients[client_index].ip_str, ntohs(clients[client_index].addr.sin_port));
        clients[client_index].active = 0;
        clients[client_index].id = -1;
    }
    LeaveCriticalSection(&cs);

    // Broadcast departure info if successfully removed
    if(removed_id != -1) {
        sprintf(info_buffer, "INFO User %d has timed out or left.", removed_id);
        broadcast_info(info_buffer, &removed_addr); // Exclude the address that timed out
    }
}

// --- Datagram Processing ---
// server_udp.c
// ... (Includes, Defines, Structs, Globals - same as before) ...
// ... (Function Prototypes - same as before) ...
// ... (main function - same as before) ...
// ... (initialize_clients, find_client_by_addr, register_client, update_client_time, remove_client - same as before) ...

// --- Datagram Processing ---
void process_datagram(char* buffer, int len, const struct sockaddr_in* client_addr) {
    char response_buffer[BUFFER_SIZE];
    int client_index = find_client_by_addr(client_addr);
    int client_id;

    // --- Start: Client Registration/Timestamp Update (No Change) ---
    if (client_index == -1) {
        client_id = register_client(client_addr);
        if (client_id == -1) {
             printf("Server full, dropping datagram from %s:%d\n", inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));
             send_to_client_addr(client_addr, "ERROR Server is full.");
             return;
        }
         client_index = find_client_by_addr(client_addr); // Find index again
         sprintf(response_buffer, "ID %d", client_id);
         send_to_client_addr(client_addr, response_buffer);
         sprintf(response_buffer, "INFO User %d (%s:%d) has joined.", client_id, clients[client_index].ip_str, ntohs(client_addr->sin_port));
         broadcast_info(response_buffer, client_addr);
    } else {
         client_id = clients[client_index].id;
         update_client_time(client_index); // Crucial: Update time on ANY received packet
    }
    // --- End: Client Registration/Timestamp Update ---


    // Now process the command in the buffer
    buffer[len] = '\0'; // Ensure null termination

    // --- Add check for PING ---
    if (_stricmp(buffer, "PING") == 0) {
         // Received keep-alive ping. Timestamp was already updated above.
         // No response needed. Just ignore it otherwise.
         // printf("Received PING from client %d\n", client_id); // Optional debug log
         return; // Don't process further as unknown command
    }
    // --- End check for PING ---

    // Handle LIST (No Change)
     if (_stricmp(buffer, "LIST") == 0) {
        char list_response[BUFFER_SIZE * 2] = "";
        EnterCriticalSection(&cs);
        // ... (build list response - same as before) ...
        strcat(list_response, "--- Active Clients ---\n");
        int count = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) { /* ... */ if (clients[i].active) {sprintf(response_buffer, "ID: %d...", clients[i].id); if(strlen(list_response)+strlen(response_buffer) < sizeof(list_response)-1) strcat(list_response, response_buffer); else {strcat(list_response,"...\n"); break;} count++;}} if(!count) strcat(list_response,"(Empty?)\n"); strcat(list_response,"----------------------\n");
        LeaveCriticalSection(&cs);
        send_to_client_addr(client_addr, list_response);
    }
    // Handle SEND (No Change)
    else if (_strnicmp(buffer, "SEND ", 5) == 0) {
        int target_id;
        char *message_start = strchr(buffer + 5, ' ');
        if (message_start != NULL && sscanf(buffer + 5, "%d", &target_id) == 1) {
            message_start++;
            if (strlen(message_start) > 0) {
                 if (target_id == BROADCAST_ID) {
                     broadcast_message(message_start, client_id, client_addr);
                 } else {
                     send_message_to_client_id(target_id, message_start, client_id, client_addr);
                 }
            } else {
                 send_to_client_addr(client_addr, "ERROR Message cannot be empty.");
            }
        } else {
             send_to_client_addr(client_addr, "ERROR Invalid SEND format. Use: SEND <id> <message>");
        }
    }
    // Handle unknown commands (PING is now handled above)
    else {
         printf("Client ID %d sent unknown command: %s\n", client_id, buffer);
         send_to_client_addr(client_addr, "ERROR Unknown command. Use LIST or SEND <id> <message>");
    }
}

// ... (send_to_client_addr, send_message_to_client_id, broadcast_message, broadcast_info - same as before) ...
// ... (check_timeouts_thread - same as before) ...

// --- Sending Functions ---

// Basic sendto wrapper
void send_to_client_addr(const struct sockaddr_in* addr, const char* message) {
    if (sendto(server_socket, message, strlen(message), 0,
              (struct sockaddr*)addr, sizeof(*addr)) == SOCKET_ERROR)
    {
        // Log error, but don't necessarily remove client here, could be temporary
        printf("sendto failed to %s:%d. Error: %d\n", inet_ntoa(addr->sin_addr), ntohs(addr->sin_port), WSAGetLastError());
    }
}

// Send to a specific client ID (finds address first)
void send_message_to_client_id(int target_id, const char* message, int sender_id, const struct sockaddr_in* sender_addr) {
    struct sockaddr_in target_addr;
    int found = 0;
    char formatted_message[BUFFER_SIZE + 64];

    EnterCriticalSection(&cs);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].id == target_id) {
            target_addr = clients[i].addr; // Copy target address
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&cs);

    if (found) {
        sprintf(formatted_message, "MSG %d: %s", sender_id, message);
        send_to_client_addr(&target_addr, formatted_message);
    } else {
        // Inform sender that target was not found
        sprintf(formatted_message, "ERROR User ID %d not found or is inactive.", target_id);
        send_to_client_addr(sender_addr, formatted_message);
    }
}

// Broadcast a user message to all clients except the sender
void broadcast_message(const char* message, int sender_id, const struct sockaddr_in* sender_addr) {
    char formatted_message[BUFFER_SIZE + 64];
    sprintf(formatted_message, "MSG %d (Broadcast): %s", sender_id, message);
    printf("Broadcasting MSG from %d: %s\n", sender_id, message);

    EnterCriticalSection(&cs);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        // Send to all active clients EXCEPT the original sender (compare address)
        if (clients[i].active &&
            !(clients[i].addr.sin_addr.s_addr == sender_addr->sin_addr.s_addr &&
              clients[i].addr.sin_port == sender_addr->sin_port) )
        {
             send_to_client_addr(&clients[i].addr, formatted_message);
        }
    }
    LeaveCriticalSection(&cs);
}

// Broadcast an informational message (e.g., join/leave)
void broadcast_info(const char* message, const struct sockaddr_in* exclude_addr) {
     printf("Broadcasting INFO: %s\n", message);
     EnterCriticalSection(&cs);
     for (int i = 0; i < MAX_CLIENTS; i++) {
          if (clients[i].active) {
               // Exclude specific address if provided
               if (exclude_addr != NULL &&
                   clients[i].addr.sin_addr.s_addr == exclude_addr->sin_addr.s_addr &&
                   clients[i].addr.sin_port == exclude_addr->sin_port)
               {
                    continue; // Skip excluded client
               }
                send_to_client_addr(&clients[i].addr, message);
          }
     }
     LeaveCriticalSection(&cs);
}


// --- Timeout Checking Thread ---
unsigned __stdcall check_timeouts_thread(void *arg) {
     printf("[Timeout Thread] Started. Checking every %d seconds.\n", CLIENT_TIMEOUT_SECONDS / 2);
     while(1) {
          // Sleep for half the timeout duration for reasonable responsiveness
          Sleep((CLIENT_TIMEOUT_SECONDS / 2) * 1000);

          time_t current_time = time(NULL);
          // Check clients under critical section to avoid race conditions with registration/removal
          EnterCriticalSection(&cs);
           for (int i = 0; i < MAX_CLIENTS; i++) {
               if (clients[i].active) {
                   double time_diff = difftime(current_time, clients[i].last_heard_time);
                   if (time_diff > CLIENT_TIMEOUT_SECONDS) {
                        // Use client index for removal to avoid issues if ID changed somehow
                        LeaveCriticalSection(&cs); // Leave CS before calling remove_client which takes CS itself
                        printf("[Timeout Thread] Client ID %d timed out (%.1f seconds inactivity).\n", clients[i].id, time_diff);
                        remove_client(i);
                        EnterCriticalSection(&cs); // Re-acquire CS to continue loop safely
                        // Need to be careful if remove_client modified the array structure (it doesn't here)
                   }
               }
           }
          LeaveCriticalSection(&cs);

          // Check if server socket is still valid periodically
          if (server_socket == INVALID_SOCKET) {
              printf("[Timeout Thread] Server socket closed. Exiting thread.\n");
              break;
          }
     }
     printf("[Timeout Thread] Exiting.\n");
     return 0;
}