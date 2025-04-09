#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")  // Link Winsock library

#define RCVBUFSIZE 32  // Size of receive buffer

// Function to print error message and exit
void DieWithError(char *errorMessage) {
    printf("%s\n", errorMessage);
    exit(1);
}

int main(int argc, char *argv[]) {
    WSADATA wsaData;  // Stores Winsock initialization data
    SOCKET sock;  // Client socket
    struct sockaddr_in echoServAddr;  // Server address
    char *servIP;  // Server IP address
    char *echoString;  // Message to send
    unsigned short echoServPort;  // Server port
    char echoBuffer[RCVBUFSIZE];  // Buffer for received data
    int echoStringLen, bytesRcvd, totalBytesRcvd;

    // Check for correct number of arguments
    if (argc < 3 || argc > 4) {
        printf("Usage: %s <Server IP> <Echo Word> [Echo Port]\n", argv[0]);
        exit(1);
    }

    servIP = argv[1];  // Get server IP from command line
    echoString = argv[2];  // Get message from command line
    echoServPort = (argc == 4) ? atoi(argv[3]) : 7;  // Default port is 7 if not specified

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        DieWithError("WSAStartup() failed");

    // Create a TCP socket
    if ((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
        DieWithError("socket() failed");

    // Set up server address structure
    memset(&echoServAddr, 0, sizeof(echoServAddr));
    echoServAddr.sin_family = AF_INET;
    echoServAddr.sin_addr.s_addr = inet_addr(servIP);
    echoServAddr.sin_port = htons(echoServPort);

    // Establish connection to the server
    if (connect(sock, (struct sockaddr *)&echoServAddr, sizeof(echoServAddr)) == SOCKET_ERROR)
        DieWithError("connect() failed");

    echoStringLen = strlen(echoString);  // Get length of message

    // Send the message to the server
    if (send(sock, echoString, echoStringLen, 0) != echoStringLen)
        DieWithError("send() sent a different number of bytes than expected");

    // Receive the echoed message back
    totalBytesRcvd = 0;
    printf("Received: ");
    while (totalBytesRcvd < echoStringLen) {
        if ((bytesRcvd = recv(sock, echoBuffer, RCVBUFSIZE - 1, 0)) <= 0)
            DieWithError("recv() failed or connection closed prematurely");

        totalBytesRcvd += bytesRcvd;
        echoBuffer[bytesRcvd] = '\0';  // Null terminate string
        printf("%s", echoBuffer);
    }

    printf("\n");

    // Close the socket and clean up Winsock
    closesocket(sock);
    WSACleanup();
    return 0;
}
