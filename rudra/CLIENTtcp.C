#include <stdio.h>      /* for printf() and fprintf() */
#include <winsock2.h>   /* for socket(), connect(), send(), and recv() */
#include <ws2tcpip.h>   /* for getaddrinfo() */
#include <stdlib.h>     /* for atoi() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() (on Windows, replace with closesocket()) */

#define RCVBUFSIZE 32   /* Size of receive buffer */

/* Error handling function */
void DieWithError(const char *errorMessage);

int main(int argc, char *argv[]) {
    WSADATA wsaData;                /* WinSock data structure */
    SOCKET sock;                     /* Socket descriptor */
    struct sockaddr_in echoServAddr; /* Echo server address */
    unsigned short echoServPort;     /* Echo server port */
    char *servIP;                   /* Server IP address (dotted quad) */
    char *echoString;               /* String to send to echo server */
    char echoBuffer[RCVBUFSIZE];     /* Buffer for echo string */
    unsigned int echoStringLen;      /* Length of string to echo */
    int bytesRcvd, totalBytesRcvd;   /* Bytes read in single recv() and total bytes read */

    /* Initialize Winsock */
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        DieWithError("WSAStartup failed");
    }

    if ((argc < 3) || (argc > 4)) {  /* Test for correct number of arguments */
        fprintf(stderr, "Usage: %s <Server IP> <Echo Word> [<Echo Port>]\n", argv[0]);
        WSACleanup();  /* Cleanup Winsock before exiting */
        exit(1);
    }

    servIP = argv[1];      /* First arg: server IP address (dotted quad) */
    echoString = argv[2];  /* Second arg: string to echo */

    if (argc == 4)
        echoServPort = atoi(argv[3]); /* Use given port, if any */
    else
        echoServPort = 7;  /* 7 is the well-known port for the echo service */

    /* Create a reliable, stream socket using TCP */
    if ((sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
        DieWithError("socket() failed");

    /* Construct the server address structure */
    memset(&echoServAddr, 0, sizeof(echoServAddr));  /* Zero out structure */
    echoServAddr.sin_family = AF_INET;                /* Internet address family */
    echoServAddr.sin_port = htons(echoServPort);      /* Server port */

    /* Convert the server IP address to binary form using inet_addr() */
    echoServAddr.sin_addr.s_addr = inet_addr(servIP);
    if (echoServAddr.sin_addr.s_addr == INADDR_NONE) {
        DieWithError("inet_addr() failed - Invalid address");
    }

    /* Establish the connection to the echo server */
    if (connect(sock, (struct sockaddr *)&echoServAddr, sizeof(echoServAddr)) == SOCKET_ERROR)
        DieWithError("connect() failed");

    echoStringLen = strlen(echoString);  /* Determine input length */

    /* Send the string to the server */
    if (send(sock, echoString, echoStringLen, 0) != echoStringLen)
        DieWithError("send() sent a different number of bytes than expected");

    /* Receive the same string back from the server */
    totalBytesRcvd = 0;
    printf("Received: ");  /* Setup to print the echoed string */

    while (totalBytesRcvd < echoStringLen) {
        /* Receive up to the buffer size (minus 1 to leave space for a null terminator) bytes from the sender */
        if ((bytesRcvd = recv(sock, echoBuffer, RCVBUFSIZE - 1, 0)) <= 0)
            DieWithError("recv() failed or connection closed prematurely");
        totalBytesRcvd += bytesRcvd; /* Keep tally of total bytes */
        echoBuffer[bytesRcvd] = '\0'; /* Terminate the string! */
        printf("%s", echoBuffer); /* Print the echo buffer */
    }

    printf("\n"); /* Print a final linefeed */

    closesocket(sock);  /* Close the socket */
    WSACleanup();       /* Clean up Winsock */
    return 0;
}

void DieWithError(const char *errorMessage) {
    fprintf(stderr, "%s\n", errorMessage);
    WSACleanup();  /* Clean up Winsock before exiting */
    exit(1);
}
