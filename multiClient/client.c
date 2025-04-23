// client.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <process.h> // For _beginthreadex, _endthreadex
#include <string.h> // For strchr, strlen, memset, strcat, strcspn, strncpy
#include <stdlib.h> // For sscanf, _stricmp, _strnicmp

#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE 2048 // Increased buffer size for messages and lists
#define MAX_INPUT_SIZE 1024 // Maximum size for user command input

// --- Global Variables ---
// Use volatile for variables accessed by multiple threads and potentially changed unexpectedly
volatile SOCKET server_socket = INVALID_SOCKET; // Connection to the server
HANDLE receive_thread_handle = NULL; // Handle for the background receive thread
volatile int connected = 0; // Flag indicating connection state (0 = disconnected, 1 = connected)
volatile int my_id = -1; // Client ID assigned by the server

// --- Function Prototypes ---
// Thread function responsible for receiving messages from the server
unsigned __stdcall receive_from_server_thread(void *arg);

// --- Main Function ---
int main() {
    WSADATA wsa;
    struct sockaddr_in server_addr;
    char server_ip[20]; // Sufficient buffer for IPv4 string + null terminator
    int server_port;
    char input_buffer[MAX_INPUT_SIZE]; // Buffer for user input
    char message_buffer[BUFFER_SIZE]; // Buffer for constructing messages to send

    // 1. Get server details from user
    printf("Enter server IP: ");
    // Use %19s to prevent buffer overflow for server_ip
    if (scanf("%19s", server_ip) != 1) {
        printf("Error reading server IP.\n");
        return 1;
    }
    printf("Enter server port: ");
     if (scanf("%d", &server_port) != 1) {
        printf("Error reading server port.\n");
        return 1;
    }
    getchar(); // Consume the leftover newline character after reading the integer

    // 2. Initialize Winsock library
    printf("Initializing Winsock...\n");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed. Error Code: %d\n", WSAGetLastError());
        return 1;
    }
    printf("Winsock Initialized.\n");

    // 3. Create a TCP socket for connecting to the server
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        printf("Could not create server socket. Error Code: %d\n", WSAGetLastError());
        WSACleanup(); return 1;
    }

    // Prepare the server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port); // Convert port to network byte order

    // Convert IP string to network address structure
    // inet_addr is deprecated; consider using InetPton or WSAStringToAddress for robustness
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    if (server_addr.sin_addr.s_addr == INADDR_NONE && strcmp(server_ip, "255.255.255.255") != 0) {
         // Note: inet_addr treats "255.255.255.255" specially. Handle other invalid inputs.
         printf("Invalid server IP address format: %s\n", server_ip);
         closesocket(server_socket);
         WSACleanup();
         return 1;
    }


    // 4. Connect to the server
    printf("Connecting to server %s:%d...\n", server_ip, server_port);
    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Failed to connect to server. Error Code: %d\n", WSAGetLastError());
        closesocket(server_socket); WSACleanup(); return 1;
    }
    printf("Connected to server.\n");
    connected = 1; // Set connection flag to true

    // 5. Start the background thread to handle receiving messages from the server
    // This is done immediately after connecting so we don't miss the initial ID or other messages
    uintptr_t server_socket_ptr = (uintptr_t)server_socket; // Safe cast for passing socket handle
    receive_thread_handle = (HANDLE)_beginthreadex(NULL, 0, receive_from_server_thread, (void*)server_socket_ptr, 0, NULL);

    if (receive_thread_handle == NULL) {
         printf("Failed to create receive thread. Error: %d\n", GetLastError());
         // Signal disconnection if thread fails to start
         connected = 0;
         closesocket(server_socket);
         WSACleanup();
         return 1;
    }
    // Close the thread handle in the main thread as we don't need to manage its lifetime beyond waiting for it to exit
    CloseHandle(receive_thread_handle);
    receive_thread_handle = NULL; // Set to NULL to indicate handle is closed

    // 6. Wait for the server to send the client's ID
    printf("Waiting for ID from server...\n");
    // Poll the my_id variable until it's set by the receive thread, or until disconnected
    while (my_id == -1 && connected) {
        Sleep(50); // Wait briefly to avoid busy-waiting
    }

    // Check if connection was lost while waiting for ID
    if (!connected) {
         printf("Connection lost before receiving ID.\n");
         // Cleanup already handled by connected flag logic
         closesocket(server_socket);
         WSACleanup();
         return 1;
    }

    // 7. Display available commands and enter the main command loop
    printf("\n--- Commands ---\n");
    printf("LIST - Get list of clients\n");
    printf("<id> <message> - Send a message to client <id> (Use %d for broadcast)\n", 101); // Show broadcast ID
    printf("EXIT - Quit the application\n");
    printf("------------------\n");

    // Main loop for handling user input and sending commands to the server
    while (connected) {
        printf("> "); // Display prompt for user input
        fflush(stdout); // Ensure the prompt is displayed immediately

        // Read user input line by line
        if (fgets(input_buffer, MAX_INPUT_SIZE, stdin) == NULL) {
            printf("Input error or stream closed. Exiting.\n");
            connected = 0; // Assume disconnection or error
            break; // Exit main loop
        }
        // Remove the trailing newline character read by fgets
        input_buffer[strcspn(input_buffer, "\n")] = 0;

        // Ignore empty input lines
        if (strlen(input_buffer) == 0) {
            continue;
        }

        // Handle EXIT command
        if (_stricmp(input_buffer, "EXIT") == 0) {
            printf("Exiting...\n");
            break; // Exit the main command loop
        }

        // Handle LIST command
        if (_stricmp(input_buffer, "LIST") == 0) {
            // Send the LIST command to the server
            if (send(server_socket, "LIST", 4, 0) == SOCKET_ERROR) {
                 printf("Failed to send LIST command. Error: %d\n", WSAGetLastError());
                 connected = 0; // Assume connection lost if sending fails
            }
        }
        // Handle SEND command format: "<id> <message>"
        else {
            int target_id = -1;
            char *message_start = NULL;

            // Attempt to parse the target ID at the beginning of the input
            // Find the first space to separate ID from message
            message_start = strchr(input_buffer, ' ');

            if (message_start != NULL) {
                // Null-terminate the string temporarily at the space to parse the ID
                *message_start = '\0';
                // Parse the integer ID from the beginning of the buffer
                if (sscanf(input_buffer, "%d", &target_id) == 1) {
                    // Restore the space in the input buffer
                    *message_start = ' ';
                     // Move message_start to the character *after* the space
                    message_start++;

                    // Check if there is a message part after the ID and space
                    if (strlen(message_start) > 0) {
                        // Construct the full SEND command string as required by the server
                        sprintf(message_buffer, "SEND %d %s", target_id, message_start);

                        // Send the constructed command to the server
                        if (send(server_socket, message_buffer, strlen(message_buffer), 0) == SOCKET_ERROR) {
                             printf("Failed to send message. Error: %d\n", WSAGetLastError());
                             connected = 0; // Assume connection lost
                        }
                    } else {
                         // User provided an ID but no message
                         *message_start = ' '; // Restore space before printing error
                         printf("Invalid format: Message cannot be empty after ID %d.\n", target_id);
                    }
                } else {
                    // Failed to parse an integer ID at the start
                     *message_start = ' '; // Restore space
                     printf("Unknown command or invalid format. Expected ID or command. Use: LIST, EXIT, or <id> <message>\n");
                }
            } else {
                 // No space found, input is a single word
                 printf("Unknown command or invalid format. Expected ID or command. Use: LIST, EXIT, or <id> <message>\n");
            }
        }
    } // End of main command loop

    // 8. Cleanup: Disconnect and clean up resources
    printf("Cleaning up...\n");
    connected = 0; // Signal the receive thread to stop its loop

    // Gracefully shutdown the socket connection
    if (server_socket != INVALID_SOCKET) {
        // SD_BOTH means disable both sends and receives
        shutdown(server_socket, SD_BOTH);
        closesocket(server_socket);
        server_socket = INVALID_SOCKET; // Mark as invalid after closing
    }

    // Wait for the receive thread to finish execution
    if (receive_thread_handle != NULL) {
        // WaitForSingleObject returns WAIT_OBJECT_0 if the thread handle is signaled (thread exited)
        // or WAIT_TIMEOUT if the timeout (1000ms) elapsed.
        if (WaitForSingleObject(receive_thread_handle, 1000) == WAIT_TIMEOUT) {
            printf("Warning: Receive thread did not exit gracefully within timeout.\n");
            // In a real application, you might forcefully terminate the thread here,
            // but it's generally discouraged. Better to fix the thread's exit condition.
        }
        CloseHandle(receive_thread_handle); // Close the handle
    }

    // Clean up the Winsock library
    WSACleanup();
    printf("Cleanup complete. Goodbye.\n");
    return 0;
}


// --- Server Receive Function (Thread) ---
// This thread continuously listens for and processes messages sent by the server
unsigned __stdcall receive_from_server_thread(void *arg) {
    SOCKET sock = (SOCKET)(uintptr_t)arg; // Cast the argument back to a SOCKET handle
    char buffer[BUFFER_SIZE];
    int bytes_received;

    printf("[Receive Thread] Started.\n");

    // Loop while the client is connected
    while (connected) {
        memset(buffer, 0, BUFFER_SIZE); // Clear the buffer
        // Attempt to receive data from the server
        bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        // Check the connection flag again after recv returns (recv can block)
        if (!connected) break;

        if (bytes_received == SOCKET_ERROR) {
            // Check if the error occurred while we were still actively connected
            if (connected) {
                int error_code = WSAGetLastError();
                // Ignore error 10004 (WSAEINTR) which can happen if socket is closed while recv is blocking
                if (error_code != WSAEINTR) {
                    printf("\n[Receive Thread] recv failed. Connection lost? Error: %d\n", error_code);
                }
                connected = 0; // Signal main thread connection is lost
            }
            break; // Exit the receive loop on error
        }

        if (bytes_received == 0) {
            // Server closed the connection gracefully
            if (connected) {
                printf("\n[Receive Thread] Server closed the connection gracefully.\n");
                connected = 0; // Signal main thread
            }
            break; // Exit the loop
        }

        // Null-terminate the received data to treat it as a string
        buffer[bytes_received] = '\0';

        // --- Process different message types received from the server ---

        // If we haven't received our ID yet, check for the "ID " message
        if (my_id == -1 && strncmp(buffer, "ID ", 3) == 0) {
            // Attempt to parse the integer ID from the message
            if (sscanf(buffer + 3, "%d", &my_id) == 1) {
                printf("\n*** Successfully registered with server. Your ID is: %d ***\n", my_id);
                 printf("> "); fflush(stdout); // Reprint prompt after ID message
            } else {
                 // Received "ID " but invalid format
                 printf("\n[Receive Thread] Received invalid ID format from server: %s\n", buffer);
                 connected = 0; // Cannot proceed without a valid ID
                 break;
            }
        }
        // Check for relayed user messages starting with "MSG "
        else if (strncmp(buffer, "MSG ", 4) == 0) {
            // Print the message directly. The server formats it as "MSG <sender_id>: <message>" or "MSG <sender_id> (Broadcast): <message>"
            printf("\n%s\n", buffer);
            printf("> "); fflush(stdout); // Reprint prompt after message
        }
        // Check for informational messages starting with "INFO "
        else if (strncmp(buffer, "INFO ", 5) == 0) {
            // Print informational messages (e.g., user joined/left)
            printf("\n[%s]\n", buffer);
            printf("> "); fflush(stdout); // Reprint prompt after info message
        }
        // Check for error messages from the server starting with "ERROR "
        else if (strncmp(buffer, "ERROR ", 6) == 0) {
            // Print error messages from the server (e.g., target ID not found)
            printf("\n[Server Error: %s]\n", buffer + 6); // Print message after "ERROR "
            printf("> "); fflush(stdout); // Reprint prompt after error message
        }
        // Assume anything else is potentially a LIST response or other unformatted message
        else {
            // Print the received data directly (e.g., the list of clients)
            printf("\n%s\n", buffer);
            printf("> "); fflush(stdout); // Reprint prompt after list/other message
        }
    } // End of while(connected) receive loop

    printf("[Receive Thread] Exiting...\n");
    connected = 0; // Ensure connection flag is 0 before exiting thread

    // Note: The socket cleanup (closesocket) should ideally be handled by the main thread
    // after signaling the receive thread to stop. This avoids potential race conditions
    // where both threads try to close the socket.

    _endthreadex(0); // Cleanly exit the thread
    return 0; // Should not be reached
}