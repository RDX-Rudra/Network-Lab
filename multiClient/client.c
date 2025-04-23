// client.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <process.h> // For _beginthreadex
#include <string.h> // For strchr

#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE 2048 // Increased buffer size

// --- Global Variables ---
volatile SOCKET server_socket = INVALID_SOCKET; // Connection to the server
HANDLE receive_thread_handle = NULL;
volatile int connected = 0; // Flag for connection state
int my_id = -1;

// --- Function Prototypes ---
unsigned __stdcall receive_from_server_thread(void *arg);

// --- Main Function ---
int main() {
    WSADATA wsa;
    struct sockaddr_in server_addr;
    char server_ip[20];
    int server_port;
    char input_buffer[BUFFER_SIZE];
    char message_buffer[BUFFER_SIZE]; // For constructing messages

    // 1. Get server details
    printf("Enter server IP: ");
    scanf("%19s", server_ip);
    printf("Enter server port: ");
    scanf("%d", &server_port);
    getchar(); // Consume newline

    // 2. Initialize Winsock
    printf("Initializing Winsock...\n");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed. Error Code: %d\n", WSAGetLastError());
        return 1;
    }
    printf("Winsock Initialized.\n");

    // 3. Create socket for server connection
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        printf("Could not create server socket. Error Code: %d\n", WSAGetLastError());
        WSACleanup(); return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    // 4. Connect to the server
    printf("Connecting to server %s:%d...\n", server_ip, server_port);
    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Failed to connect to server. Error Code: %d\n", WSAGetLastError());
        closesocket(server_socket); WSACleanup(); return 1;
    }
    printf("Connected to server.\n");
    connected = 1;

    // 5. Start receive thread *before* potentially blocking on initial ID receive
    uintptr_t server_socket_ptr = (uintptr_t)server_socket;
    receive_thread_handle = (HANDLE)_beginthreadex(NULL, 0, receive_from_server_thread, (void*)server_socket_ptr, 0, NULL);
    if (receive_thread_handle == NULL) {
         printf("Failed to create receive thread. Error: %d\n", GetLastError());
         closesocket(server_socket); WSACleanup(); return 1;
    }


    // 6. Main Command Loop
    printf("Waiting for ID from server...\n"); // Wait for receive thread to get ID
    while (my_id == -1 && connected) {
        Sleep(50); // Wait briefly for ID to be assigned by receive thread
    }

    if (!connected) {
         printf("Connection lost before receiving ID.\n");
         // Wait for receive thread to finish (optional but good practice)
         if (receive_thread_handle != NULL) {
             WaitForSingleObject(receive_thread_handle, 1000); // Wait up to 1 sec
             CloseHandle(receive_thread_handle);
         }
         closesocket(server_socket);
         WSACleanup();
         return 1;
    }


    printf("\n--- Commands ---\n");
    printf("LIST             - Get list of clients\n");
    printf("<id> <message>   - Send a message to client <id>\n");
    printf("EXIT             - Quit the application\n");
    printf("------------------\n");

    while (connected) {
        printf("> "); // Prompt
        if (fgets(input_buffer, BUFFER_SIZE, stdin) == NULL) {
            printf("Input error. Exiting.\n");
            break; // Exit loop on input error
        }
        input_buffer[strcspn(input_buffer, "\n")] = 0; // Remove trailing newline

        if (strlen(input_buffer) == 0) {
            continue; // Ignore empty input
        }

        // Handle EXIT command
        if (_stricmp(input_buffer, "EXIT") == 0) {
            printf("Exiting...\n");
            break; // Exit main loop
        }

        // Handle LIST command
        if (_stricmp(input_buffer, "LIST") == 0) {
            if (send(server_socket, "LIST", 4, 0) == SOCKET_ERROR) {
                printf("Failed to send LIST command. Error: %d\n", WSAGetLastError());
                connected = 0; // Assume connection lost on send failure
            }
        }
        // Handle <id> <message> command
        else {
            int target_id = -1;
            char *message_start = strchr(input_buffer, ' ');

            // Try to parse ID if there's a space
            if (message_start != NULL && sscanf(input_buffer, "%d", &target_id) == 1 && target_id > 0)
            {
                message_start++; // Move past the space
                if (strlen(message_start) > 0) {
                    // Construct the SEND command
                    sprintf(message_buffer, "SEND %d %s", target_id, message_start);
                    if (send(server_socket, message_buffer, strlen(message_buffer), 0) == SOCKET_ERROR) {
                        printf("Failed to send message. Error: %d\n", WSAGetLastError());
                        connected = 0; // Assume connection lost
                    }
                } else {
                     printf("Invalid format: Message cannot be empty.\n");
                }
            } else {
                 // Treat as unknown command if not LIST or <id> <message>
                 printf("Unknown command or invalid format. Use: LIST, EXIT, or <id> <message>\n");
            }
        }
    } // End of main command loop

    // 7. Cleanup
    printf("Cleaning up...\n");
    connected = 0; // Signal receive thread to stop
    if (server_socket != INVALID_SOCKET) {
        shutdown(server_socket, SD_BOTH); // Signal server we're closing
        closesocket(server_socket);
        server_socket = INVALID_SOCKET;
    }

    // Wait for receive thread to finish
    if (receive_thread_handle != NULL) {
        WaitForSingleObject(receive_thread_handle, 1000); // Wait up to 1 sec
        CloseHandle(receive_thread_handle);
    }

    WSACleanup();
    printf("Cleanup complete. Goodbye.\n");
    return 0;
}


// --- Server Receive Function (Thread) ---
// Receives all messages from the server (ID, Lists, Relayed Msgs, Errors)
unsigned __stdcall receive_from_server_thread(void *arg) {
    SOCKET sock = (SOCKET)(uintptr_t)arg;
    char buffer[BUFFER_SIZE];
    int bytes_received;

    printf("[Receive Thread] Started.\n");

    while (connected) {
        memset(buffer, 0, BUFFER_SIZE);
        bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        if (!connected) break; // Check flag again after potential block in recv

        if (bytes_received == SOCKET_ERROR) {
            if (connected) { // Avoid error message if we initiated the disconnect
                 int error_code = WSAGetLastError();
                 printf("\n[Receive Thread] recv failed. Connection lost? Error: %d\n", error_code);
                 connected = 0; // Signal main thread connection is lost
            }
            break; // Exit loop on error
        }

        if (bytes_received == 0) {
             if (connected) {
                 printf("\n[Receive Thread] Server closed the connection.\n");
                 connected = 0; // Signal main thread
             }
             break; // Exit loop if server closed connection gracefully
        }

        // Null-terminate and process the received message
        buffer[bytes_received] = '\0';

        // --- Process different message types from server ---

        // Check for initial ID assignment
        if (my_id == -1 && strncmp(buffer, "ID ", 3) == 0) {
            if (sscanf(buffer + 3, "%d", &my_id) == 1) {
                printf("\n*** Successfully registered with server. Your ID is: %d ***\n> ", my_id);
            } else {
                 printf("\n[Receive Thread] Received invalid ID format: %s\n", buffer);
                 connected = 0; // Cannot proceed without valid ID
                 break;
            }
        }
        // Check for relayed messages
        else if (strncmp(buffer, "MSG ", 4) == 0) {
            printf("\n%s\n> ", buffer); // Print "MSG <sender_id>: <message>"
        }
        // Check for informational messages
        else if (strncmp(buffer, "INFO ", 5) == 0) {
            printf("\n[%s]\n> ", buffer); // Print "[INFO User X has joined/left]"
        }
        // Check for error messages
        else if (strncmp(buffer, "ERROR ", 6) == 0) {
             printf("\n[Server Error: %s]\n> ", buffer + 6);
        }
        // Assume anything else is potentially a LIST response or unknown
        else {
            printf("\n%s\n> ", buffer); // Print directly (e.g., client list)
        }
        fflush(stdout); // Ensure prompt is displayed immediately after received message
    }

    printf("[Receive Thread] Exiting...\n");
    connected = 0; // Ensure flag is set if loop exited unexpectedly
    if (server_socket != INVALID_SOCKET && my_id != -1) {
        // Attempt to close socket from here if main thread hasn't already
        // Be cautious with closing sockets from multiple threads.
        // closesocket(server_socket); // It's usually safer for the main thread to handle cleanup
    }
    return 0; // Thread exits
}