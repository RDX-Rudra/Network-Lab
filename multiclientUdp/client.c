// client_udp.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h> // Include for sockaddr_in
#include <windows.h>
#include <stdint.h>
#include <process.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE 2048
#define KEEP_ALIVE_INTERVAL_MS 20000 // Send keep-alive every 20 seconds

// --- Global Variables ---
SOCKET client_socket = INVALID_SOCKET;
struct sockaddr_in server_addr;
char server_ip_str[INET_ADDRSTRLEN];
int server_port_int;
HANDLE receive_thread_handle = NULL;
HANDLE keep_alive_thread_handle = NULL; // Handle for the new thread
volatile int running = 0;
int my_id = -1;

// --- Function Prototypes ---
unsigned __stdcall receive_thread(void *arg);
unsigned __stdcall keep_alive_thread(void *arg); // New keep-alive thread function

// --- Main Function ---
int main() {
    WSADATA wsa;
    char input_buffer[BUFFER_SIZE];
    char message_buffer[BUFFER_SIZE];

    // 1. Get server details
    printf("Enter server IP: ");
    scanf("%19s", server_ip_str);
    printf("Enter server port: ");
    scanf("%d", &server_port_int);
    getchar(); // Consume newline

    // 2. Initialize Winsock
    printf("Initializing Winsock...\n");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed. Error Code: %d\n", WSAGetLastError()); return 1;
    }
    printf("Winsock Initialized.\n");

    // 3. Create UDP socket
    client_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client_socket == INVALID_SOCKET) {
        printf("Could not create socket. Error Code: %d\n", WSAGetLastError());
        WSACleanup(); return 1;
    }
    printf("UDP Socket created.\n");

    // *** Ensure the socket is bound ***
    // This helps maintain a consistent source port.
    struct sockaddr_in client_bind_addr;
    client_bind_addr.sin_family = AF_INET;
    client_bind_addr.sin_addr.s_addr = INADDR_ANY;
    client_bind_addr.sin_port = htons(0); // System assigns ephemeral port
    if (bind(client_socket, (struct sockaddr*)&client_bind_addr, sizeof(client_bind_addr)) == SOCKET_ERROR) {
         printf("Warning: Failed to bind client socket. Error: %d. Port stability might be affected.\n", WSAGetLastError());
         // Continue even if bind fails, but it's less ideal
    } else {
         // Optional: Get the assigned port if needed
         int addr_len = sizeof(client_bind_addr);
         if(getsockname(client_socket, (struct sockaddr*)&client_bind_addr, &addr_len) == 0) {
              printf("Client bound to local port %d\n", ntohs(client_bind_addr.sin_port));
         }
    }


    // 4. Prepare server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port_int);
    server_addr.sin_addr.s_addr = inet_addr(server_ip_str);

    if (server_addr.sin_addr.s_addr == INADDR_NONE) {
        printf("Invalid server IP address provided: %s\n", server_ip_str);
        closesocket(client_socket); WSACleanup(); return 1;
    }

    // 5. Start Threads
    running = 1; // Set running flag BEFORE starting threads

    // Start Receive Thread
    receive_thread_handle = (HANDLE)_beginthreadex(NULL, 0, receive_thread, NULL, 0, NULL);
    if (receive_thread_handle == NULL) {
         printf("Failed to create receive thread. Error: %d\n", GetLastError());
         running = 0; closesocket(client_socket); WSACleanup(); return 1;
    }

    // Start Keep-Alive Thread
    keep_alive_thread_handle = (HANDLE)_beginthreadex(NULL, 0, keep_alive_thread, NULL, 0, NULL);
     if (keep_alive_thread_handle == NULL) {
         printf("Failed to create keep-alive thread. Error: %d\n", GetLastError());
         // Continue without keep-alive? Or exit? For robustness, let's exit.
         running = 0; // Signal receive thread to stop too
         WaitForSingleObject(receive_thread_handle, 1000);
         CloseHandle(receive_thread_handle);
         closesocket(client_socket); WSACleanup(); return 1;
     }

    // 6. Send initial message
    printf("Sending initial LIST command to register with server...\n");
    const char* initial_msg = "LIST";
    if (sendto(client_socket, initial_msg, strlen(initial_msg), 0,
              (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Initial sendto failed. Error: %d\n", WSAGetLastError());
        running = 0; // Signal threads
        WaitForSingleObject(receive_thread_handle, 1000);
        WaitForSingleObject(keep_alive_thread_handle, 100); // Doesn't block long
        CloseHandle(receive_thread_handle);
        CloseHandle(keep_alive_thread_handle);
        closesocket(client_socket); WSACleanup(); return 1;
    }

    // 7. Wait briefly for ID assignment (as before)
    printf("Waiting for ID from server...\n");
    int wait_count = 0;
    while (my_id == -1 && running && wait_count < 100) { // Wait max ~5 seconds
        Sleep(50);
        wait_count++;
    }
     if (my_id == -1 && running) {
         printf("Did not receive ID from server in time. Proceeding anyway...\n");
     } else if (!running) {
         printf("Connection lost before receiving ID.\n");
     }


    printf("\n--- Commands ---\n");
    printf("LIST             - Get list of clients\n");
    printf("<id> <message>   - Send a message to client <id> (Use 101 for broadcast)\n");
    printf("EXIT             - Quit the application\n");
    printf("------------------\n");

    // 8. Main Command Loop (no changes needed here)
    while (running) {
        printf("> "); // Prompt
        if (fgets(input_buffer, BUFFER_SIZE, stdin) == NULL) {
            if (running) printf("Input error. Exiting.\n");
            break;
        }
        input_buffer[strcspn(input_buffer, "\n")] = 0;

        if (strlen(input_buffer) == 0) continue;

        if (_stricmp(input_buffer, "EXIT") == 0) {
            printf("Exiting...\n");
            break;
        }

        if (_stricmp(input_buffer, "LIST") == 0) {
             if (sendto(client_socket, "LIST", 4, 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
                 printf("Failed to send LIST command. Error: %d\n", WSAGetLastError());
                 running = 0; // Assume connection issue
             }
        }
        else {
            int target_id = -1;
            char *message_start = strchr(input_buffer, ' ');

            if (message_start != NULL && sscanf(input_buffer, "%d", &target_id) == 1 && target_id > 0)
            {
                message_start++;
                if (strlen(message_start) > 0) {
                    sprintf(message_buffer, "SEND %d %s", target_id, message_start);
                     if (sendto(client_socket, message_buffer, strlen(message_buffer), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
                         printf("Failed to send message. Error: %d\n", WSAGetLastError());
                         running = 0;
                     }
                } else {
                     printf("Invalid format: Message cannot be empty.\n");
                }
            } else {
                 printf("Unknown command or invalid format. Use: LIST, EXIT, or <id> <message>\n");
            }
        }
    }

    // 9. Cleanup
    printf("Cleaning up...\n");
    running = 0; // Signal threads to stop

    if (client_socket != INVALID_SOCKET) {
         closesocket(client_socket);
         client_socket = INVALID_SOCKET;
    }

    // Wait for threads to finish
    if (receive_thread_handle != NULL) {
        printf("Waiting for receive thread to exit...\n");
        WaitForSingleObject(receive_thread_handle, 2000);
        CloseHandle(receive_thread_handle);
    }
     if (keep_alive_thread_handle != NULL) {
        printf("Waiting for keep-alive thread to exit...\n");
        WaitForSingleObject(keep_alive_thread_handle, 100); // Should exit quickly
        CloseHandle(keep_alive_thread_handle);
    }

    WSACleanup();
    printf("Cleanup complete. Goodbye.\n");
    return 0;
}


// --- Receive Thread --- (No changes needed in receive logic)
unsigned __stdcall receive_thread(void *arg) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in sender_addr;
    int sender_addr_len = sizeof(sender_addr);

    printf("[Receive Thread] Started.\n");

    while (running) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recvfrom(client_socket, buffer, BUFFER_SIZE - 1, 0,
                                     (struct sockaddr*)&sender_addr, &sender_addr_len);

        if (!running) break;

        if (bytes_received == SOCKET_ERROR) {
             if (running) {
                 int error_code = WSAGetLastError();
                  if (error_code != WSAECONNRESET && error_code != WSAEINTR && error_code != WSAENOTSOCK && error_code != WSAEINVAL) {
                      printf("\n[Receive Thread] recvfrom failed. Error: %d\n", error_code);
                      running = 0; // Signal main thread on critical errors
                 } else if (error_code != WSAECONNRESET) {
                      // Error likely due to socket closure by main thread
                      // printf("\n[Receive Thread] Socket closed or interrupted. Error: %d\n", error_code);
                 }
             }
             if(!running) break; // Exit if main thread stopped us
             else if (WSAGetLastError() != WSAECONNRESET) break; // Exit on critical errors
             else continue; // Ignore reset and continue
        }


        if (sender_addr.sin_addr.s_addr != server_addr.sin_addr.s_addr ||
            sender_addr.sin_port != server_addr.sin_port)
        {
            // printf("\n[Receive Thread] Received datagram from unexpected source %s:%d. Ignored.\n",
            //        inet_ntoa(sender_addr.sin_addr), ntohs(sender_addr.sin_port));
            continue;
        }


        buffer[bytes_received] = '\0';

        // Process different message types from server
        if (my_id == -1 && strncmp(buffer, "ID ", 3) == 0) {
            if (sscanf(buffer + 3, "%d", &my_id) == 1) {
                printf("\n*** Successfully registered with server. Your ID is: %d ***\n> ", my_id);
            } else {
                 printf("\n[Receive Thread] Received invalid ID format: %s\n", buffer);
            }
        }
        else if (strncmp(buffer, "MSG ", 4) == 0) { printf("\n%s\n> ", buffer); }
        else if (strncmp(buffer, "INFO ", 5) == 0) { printf("\n[%s]\n> ", buffer); }
        else if (strncmp(buffer, "ERROR ", 6) == 0) { printf("\n[Server Error: %s]\n> ", buffer + 6); }
        else { printf("\n%s\n> ", buffer); } // Assume LIST response or unknown
        fflush(stdout);
    }

    printf("[Receive Thread] Exiting...\n");
    return 0;
}


// --- Keep-Alive Thread --- (NEW)
unsigned __stdcall keep_alive_thread(void *arg) {
     const char* ping_msg = "PING";
     printf("[Keep-Alive Thread] Started. Sending PING every %d ms.\n", KEEP_ALIVE_INTERVAL_MS);

     while(running) {
          // Wait for the interval
          Sleep(KEEP_ALIVE_INTERVAL_MS);

          // Check if still running after sleep
          if (!running) break;

          // Send PING message
          if (client_socket != INVALID_SOCKET) {
               if (sendto(client_socket, ping_msg, strlen(ping_msg), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
               {
                   int error = WSAGetLastError();
                   // Don't necessarily stop the client on PING failure, could be temporary network issue
                   // But log it. If it fails consistently, the receive thread will likely detect connection loss.
                   printf("[Keep-Alive Thread] sendto PING failed. Error: %d\n", error);
                   // Maybe add logic to stop after N consecutive failures? For simplicity, just log.
               } else {
                    // printf("."); // Optional: print something small to show pings are sending
                    // fflush(stdout);
               }
          }
     }

     printf("[Keep-Alive Thread] Exiting.\n");
     return 0;
}