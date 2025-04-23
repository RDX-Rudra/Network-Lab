// server.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <process.h> // For _beginthreadex
#include <string.h> // For strchr, strlen, memset, strcpy, strcat, strcspn
#include <stdlib.h> // For sscanf, _stricmp, _strnicmp

#pragma comment(lib, "ws2_32.lib")

#define SERVER_PORT 9000
#define MAX_CLIENTS 100
#define BUFFER_SIZE 2048
#define INET_ADDRSTRLEN_IPV4 16 // Standard length for IPv4 dotted-decimal + null terminator
#define BROADCAST_ID 101 // Define the special ID for broadcasting

// Structure to hold client information
typedef struct {
    int id;
    SOCKET socket;
    char ip[INET_ADDRSTRLEN_IPV4]; // Use defined constant
    // Removed thread_handle as it was not effectively used
    int active; // Flag to indicate if the slot is in use
} Client;

Client clients[MAX_CLIENTS];
int next_client_id = 1; // Start normal IDs from 1
CRITICAL_SECTION cs; // Critical section for synchronizing access to shared data (clients array, next_client_id)

// --- Function Prototypes ---
// Thread function to handle communication with a single client
unsigned __stdcall handle_client(void *arg);
// Function to remove a client from the active list
void remove_client(SOCKET client_socket);
// Function to send a message from one client to another
void send_message_to_client(int target_id, const char* message, int sender_id);
// Function to broadcast informational messages to all clients (excluding sender)
void broadcast_info(const char* message, int exclude_id);
// Function to broadcast a user message to all clients (excluding sender)
void broadcast_message(const char* message, int sender_id);

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

    // Initialize the critical section for thread safety
    InitializeCriticalSection(&cs);

    // Initialize client array slots
    for(int i = 0; i < MAX_CLIENTS; ++i) {
        clients[i].active = 0;
        clients[i].socket = INVALID_SOCKET;
        clients[i].id = -1;
        // clients[i].ip remains uninitialized, but won't be used if active is 0
    }

    // Create server socket
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        printf("Could not create socket. Error Code: %d\n", WSAGetLastError());
        WSACleanup(); return 1;
    }
    printf("Server socket created.\n");

    // Prepare the sockaddr_in structure
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY; // Listen on any available network interface
    server.sin_port = htons(SERVER_PORT);

    // Bind the socket to the specified IP and port
    if (bind(server_socket, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
        printf("Bind failed. Error Code: %d\n", WSAGetLastError());
        closesocket(server_socket); WSACleanup(); return 1;
    }
    printf("Socket bound to port %d.\n", SERVER_PORT);

    // Start listening for incoming connections
    if (listen(server_socket, 5) == SOCKET_ERROR) { // Max 5 pending connections
        printf("Listen failed. Error Code: %d\n", WSAGetLastError());
        closesocket(server_socket); WSACleanup(); return 1;
    }

    printf("Server listening on port %d...\n", SERVER_PORT);
    printf("Broadcast ID is set to %d\n", BROADCAST_ID);

    // Accept incoming connections and handle them in new threads
    while ((client_socket = accept(server_socket, (struct sockaddr*)&client, &c)) != INVALID_SOCKET) {
        printf("Connection accepted from %s:%d\n", inet_ntoa(client.sin_addr), ntohs(client.sin_port));

        // Create a new thread to handle the client
        // Use uintptr_t for safe casting of the socket handle
        uintptr_t client_socket_ptr = (uintptr_t)client_socket;
        HANDLE threadHandle = (HANDLE)_beginthreadex(NULL, 0, handle_client, (void*)client_socket_ptr, 0, NULL);

        if (threadHandle == NULL) {
             printf("Failed to create thread for client. Error code: %d\n", GetLastError());
             closesocket(client_socket); // Close the socket if thread creation fails
        } else {
             // We don't need to wait on this thread handle, so close it to prevent resource leaks
             CloseHandle(threadHandle);
        }
    }

    // If the accept loop terminates, it's usually due to a critical error or server shutdown
    if (client_socket == INVALID_SOCKET) {
        printf("Accept failed. Error Code: %d\n", WSAGetLastError());
    }

    printf("Shutting down server...\n");
    // Clean up the critical section
    DeleteCriticalSection(&cs);
    // Close the server listening socket
    closesocket(server_socket);
    // Clean up Winsock
    WSACleanup();
    return 0;
}


// --- Client Handling Thread ---
unsigned __stdcall handle_client(void *arg) {
    SOCKET client_socket = (SOCKET)(uintptr_t)arg;
    char buffer[BUFFER_SIZE];
    char client_ip[INET_ADDRSTRLEN_IPV4] = {0}; // Initialize to zero
    int current_client_id = -1;
    int client_array_index = -1; // Store the index in the clients array

    // Get client IP address
    struct sockaddr_in addr;
    int len = sizeof(addr);
    if (getpeername(client_socket, (struct sockaddr*)&addr, &len) == SOCKET_ERROR) {
         printf("getpeername failed for a new client. Error: %d\n", WSAGetLastError());
         closesocket(client_socket);
         _endthreadex(1); // Exit the thread
         return 1;
    }
    // Using inet_ntoa which is deprecated, but fine for this example
    strncpy(client_ip, inet_ntoa(addr.sin_addr), sizeof(client_ip) - 1);
    client_ip[sizeof(client_ip) - 1] = '\0'; // Ensure null termination

    // Register client in the shared clients array
    EnterCriticalSection(&cs);
    for(int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].active) {
            // Skip the broadcast ID if it happens to be the next available ID
            if (next_client_id == BROADCAST_ID) {
                 next_client_id++;
            }
            clients[i].id = next_client_id++;
            clients[i].socket = client_socket;
            strncpy(clients[i].ip, client_ip, sizeof(clients[i].ip) - 1);
            clients[i].ip[sizeof(clients[i].ip) - 1] = '\0';
            clients[i].active = 1;
            client_array_index = i; // Store the index
            current_client_id = clients[i].id;
            printf("Registered client ID %d (%s) at index %d\n", current_client_id, client_ip, client_array_index);
            break;
        }
    }
    LeaveCriticalSection(&cs);

    // Handle case where server is full
    if (current_client_id == -1) {
        printf("Server full. Cannot register client %s\n", client_ip);
        const char *full_msg = "ERROR Server is full. Try again later.";
        // Attempt to send the error message before closing
        send(client_socket, full_msg, strlen(full_msg), 0);
        closesocket(client_socket);
        _endthreadex(1); // Exit the thread
        return 1;
    }

    // Send the assigned ID to the client
    sprintf(buffer, "ID %d", current_client_id);
    if (send(client_socket, buffer, strlen(buffer), 0) == SOCKET_ERROR) {
        printf("Failed to send ID to client %d. Error: %d\n", current_client_id, WSAGetLastError());
        // Removal will be handled by the receive loop breaking or during cleanup
        // remove_client(client_socket); // Avoid removing here, let the loop handle it
        _endthreadex(1); // Exit the thread
        return 1;
    }

    // Broadcast client joined information to others
    sprintf(buffer, "INFO User %d (%s) has joined.", current_client_id, client_ip);
    broadcast_info(buffer, current_client_id); // Exclude the joining client

    // Main receive loop for this client
    while (1) {
        memset(buffer, 0, BUFFER_SIZE); // Clear the buffer before receiving
        int bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received <= 0) {
            // Handle disconnection (graceful or error)
            if (bytes_received == 0) {
                printf("Client ID %d disconnected gracefully.\n", current_client_id);
            } else {
                printf("recv failed for client ID %d. Error: %d.\n", current_client_id, WSAGetLastError());
            }
            break; // Exit the receive loop on disconnection
        }

        buffer[bytes_received] = '\0'; // Null-terminate the received data

        // --- Process client commands ---
        if (_stricmp(buffer, "LIST") == 0) {
            // Handle LIST command: Send list of active clients
            char response[BUFFER_SIZE * 2]; // Use a larger buffer for the list
            response[0] = '\0'; // Ensure buffer is empty

            EnterCriticalSection(&cs); // Lock access to the clients array
            strcat(response, "--- Active Clients ---\n");
            int active_count = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active) {
                    char entry[128]; // Buffer for a single client entry
                    sprintf(entry, "ID: %d (%s) %s\n", clients[i].id, clients[i].ip, (clients[i].id == current_client_id) ? "(You)" : "");
                    // Check if adding this entry would overflow the response buffer
                    if (strlen(response) + strlen(entry) < sizeof(response) - 1) {
                        strcat(response, entry);
                    } else {
                         strcat(response, "... (list truncated)\n");
                         break; // Stop adding entries if buffer is full
                    }
                    active_count++;
                }
            }
            LeaveCriticalSection(&cs); // Release the lock

            if (active_count == 0) {
                 // This case should theoretically not happen for the requesting client, but good check
                 if (strlen(response) + strlen("(No active clients found)\n") < sizeof(response) - 1) {
                    strcat(response, "(No active clients found)\n");
                 }
            }
            if (strlen(response) + strlen("----------------------\n") < sizeof(response) - 1) {
                strcat(response, "----------------------\n");
            }


            // Send the generated list back to the requesting client
            if (send(client_socket, response, strlen(response), 0) == SOCKET_ERROR) {
                 printf("Failed to send list to client ID %d. Error: %d\n", current_client_id, WSAGetLastError());
                 break; // Assume connection lost if sending fails
            }

        } else if (_strnicmp(buffer, "SEND ", 5) == 0) {
            // Handle SEND command: Parse target ID and message, then send
            int target_id = -1;
            char *message_start = strchr(buffer + 5, ' '); // Find the space after the ID

            // Check if a space was found and parse the target ID
            if (message_start != NULL && sscanf(buffer + 5, "%d", &target_id) == 1) {
                message_start++; // Move past the space to the start of the message

                // Check if the message part is not empty
                if (strlen(message_start) > 0) {
                    // Check if the target ID is the special broadcast ID
                    if (target_id == BROADCAST_ID) {
                        printf("Client %d broadcasting: %s\n", current_client_id, message_start);
                        broadcast_message(message_start, current_client_id); // Broadcast to others
                    } else {
                        // Send message to a specific client ID
                        printf("Client %d sending to %d: %s\n", current_client_id, target_id, message_start);
                        send_message_to_client(target_id, message_start, current_client_id);
                    }
                } else {
                    // Message is empty
                    sprintf(buffer, "ERROR Message cannot be empty.");
                    send(client_socket, buffer, strlen(buffer), 0); // Send error back to sender
                }
            } else {
                // Invalid SEND command format
                 sprintf(buffer, "ERROR Invalid SEND format. Use: SEND <id> <message>");
                 send(client_socket, buffer, strlen(buffer), 0); // Send error back to sender
            }

        } else {
            // Handle unknown commands
            printf("Client ID %d sent unknown command: %s\n", current_client_id, buffer);
            sprintf(buffer, "ERROR Unknown command. Use LIST, SEND <id> <message>");
            send(client_socket, buffer, strlen(buffer), 0); // Send error back to sender
        }
    } // End of while(1) receive loop

    // --- Client Disconnected ---
    // Broadcast client left information
    sprintf(buffer, "INFO User %d (%s) has left.", current_client_id, client_ip);
    broadcast_info(buffer, current_client_id); // Exclude the leaving client

    // Remove the client from the active list
    remove_client(client_socket);

    _endthreadex(0); // Exit the thread cleanly
    return 0; // Should not be reached after _endthreadex
}

// --- Utility Functions ---

// Function to remove a client from the active list
void remove_client(SOCKET client_socket) {
    EnterCriticalSection(&cs); // Lock access to the clients array
    for (int i = 0; i < MAX_CLIENTS; i++) {
        // Find the client by their socket
        if (clients[i].active && clients[i].socket == client_socket) {
            printf("Removing client ID %d (%s) from index %d\n", clients[i].id, clients[i].ip, i);
            // Clean up socket resources
            closesocket(clients[i].socket);
            // Mark the slot as inactive and reset values
            clients[i].active = 0;
            clients[i].id = -1;
            clients[i].socket = INVALID_SOCKET;
            // No need to clear IP string explicitly, active flag is sufficient
            break; // Found and removed the client, exit loop
        }
    }
    LeaveCriticalSection(&cs); // Release the lock
}

// Function to send a message from one client to another
void send_message_to_client(int target_id, const char* message, int sender_id) {
    SOCKET target_socket = INVALID_SOCKET;
    char formatted_message[BUFFER_SIZE + 64]; // Buffer for formatted message

    EnterCriticalSection(&cs); // Lock access to the clients array
    // Find the target client's socket by ID
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].id == target_id) {
            target_socket = clients[i].socket;
            break; // Found the target client
        }
    }
    LeaveCriticalSection(&cs); // Release the lock

    // Format the message: MSG <sender_id>: <message>
    sprintf(formatted_message, "MSG %d: %s", sender_id, message);

    if (target_socket != INVALID_SOCKET) {
        // Send the formatted message to the target client
        if (send(target_socket, formatted_message, strlen(formatted_message), 0) == SOCKET_ERROR) {
            printf("Failed to relay message from %d to %d. Error: %d\n", sender_id, target_id, WSAGetLastError());
            // Note: A send failure here might indicate the client disconnected unexpectedly
            // You might consider calling remove_client(target_socket) here, but be careful
            // as it might be called concurrently if the receive thread also detected the disconnect.
            // Letting the receive thread's error handling manage removal is usually safer.
        }
    } else {
        // Target client ID not found or is inactive
        SOCKET sender_socket = INVALID_SOCKET;
        // Prepare an error message to send back to the original sender
        sprintf(formatted_message, "ERROR User ID %d not found or is inactive.", target_id);

        EnterCriticalSection(&cs); // Lock to find sender's socket
        // Find the sender's socket to send the error message back
        for (int i = 0; i < MAX_CLIENTS; i++) {
             if (clients[i].active && clients[i].id == sender_id) {
                 sender_socket = clients[i].socket;
                 break; // Found the sender
             }
        }
        LeaveCriticalSection(&cs); // Release the lock

        if (sender_socket != INVALID_SOCKET) {
            // Send the error message to the original sender
            send(sender_socket, formatted_message, strlen(formatted_message), 0);
        }
    }
}

// Function to broadcast informational messages to all clients (excluding sender)
void broadcast_info(const char* message, int exclude_id) {
    printf("Broadcasting INFO: %s (excluding %d)\n", message, exclude_id);
    EnterCriticalSection(&cs); // Lock access to the clients array
    // Iterate through all client slots
    for (int i = 0; i < MAX_CLIENTS; i++) {
         // If the client is active AND their ID is not the excluded ID
         if (clients[i].active && clients[i].id != exclude_id) {
               // Send the message
               if (send(clients[i].socket, message, strlen(message), 0) == SOCKET_ERROR) {
                    printf("INFO Broadcast failed for client %d. Error: %d\n", clients[i].id, WSAGetLastError());
                    // Similar to send_message_to_client, handle removal in the receive thread
               }
         }
    }
    LeaveCriticalSection(&cs); // Release the lock
}

// Function to broadcast a user message to all clients (excluding sender)
void broadcast_message(const char* message, int sender_id) {
    char formatted_message[BUFFER_SIZE + 64]; // Buffer for formatted message

    // Format message: MSG <sender_id> (Broadcast): <message>
    sprintf(formatted_message, "MSG %d (Broadcast): %s", sender_id, message);
    printf("Broadcasting MSG: %s\n", formatted_message); // Log the broadcast action on the server

    EnterCriticalSection(&cs); // Lock access to the clients array
    // Iterate through all client slots
    for (int i = 0; i < MAX_CLIENTS; i++) {
        // If the client is active AND their ID is not the original sender's ID
        if (clients[i].active && clients[i].id != sender_id) {
            // Send the formatted message
            if (send(clients[i].socket, formatted_message, strlen(formatted_message), 0) == SOCKET_ERROR) {
                 printf("MSG Broadcast failed for client %d. Error: %d\n", clients[i].id, WSAGetLastError());
                 // Handle removal in the receive thread
            }
        }
    }
    LeaveCriticalSection(&cs); // Release the lock
}