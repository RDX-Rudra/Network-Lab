#include <stdio.h>          /* for printf() and fprintf() */
#include <winsock2.h>       /* for socket(), bind(), listen(), accept() */
#include <ws2tcpip.h>       /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>         /* for atoi() */
#include <string.h>         /* for memset() */
#include <unistd.h>         /* for close() (on Windows, replace with closesocket()) */

#define MAXPENDING 5        /* Maximum outstanding connection requests */
#define RCVBUFSIZE 100      /* Buffer size for receiving messages */

void DieWithError(const char *errorMessage);  /* Error handling function */
void HandleTCPClient(SOCKET clntSocket);  /* TCP client handling function */

int main(int argc, char *argv[]) {
    WSADATA wsaData;                    /* Winsock data structure */
    SOCKET servSock;                    /* Socket descriptor for server */
    SOCKET clntSock;                    /* Socket descriptor for client */
    struct sockaddr_in echoServAddr;    /* Local address */
    struct sockaddr_in echoClntAddr;   /* Client address */
    unsigned short echoServPort;        /* Server port */
    int clntLen;                        /* Length of client address data structure (changed to int) */

    /* Initialize Winsock */
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        DieWithError("WSAStartup failed");
    }

    /* Test for correct number of arguments */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <Server Port>\n", argv[0]);
        WSACleanup();  /* Cleanup Winsock before exiting */
        exit(1);
    }

    echoServPort = atoi(argv[1]);  /* First arg: local port */

    /* Create socket for incoming connections */
    if ((servSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
        DieWithError("socket() failed");

    /* Construct local address structure */
    memset(&echoServAddr, 0, sizeof(echoServAddr));  /* Zero out structure */
    echoServAddr.sin_family = AF_INET;                /* Internet address family */
    echoServAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
    echoServAddr.sin_port = htons(echoServPort);      /* Local port */

    /* Bind to the local address */
    if (bind(servSock, (struct sockaddr *) &echoServAddr, sizeof(echoServAddr)) == SOCKET_ERROR)
        DieWithError("bind() failed");

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servSock, MAXPENDING) == SOCKET_ERROR)
        DieWithError("listen() failed");
    else
        printf("Server is listening.\n");

    /* Run forever */
    for (;;) {
        /* Set the size of the in-out parameter */
        clntLen = sizeof(echoClntAddr);

        /* Wait for a client to connect */
        if ((clntSock = accept(servSock, (struct sockaddr *) &echoClntAddr, &clntLen)) == INVALID_SOCKET)
            DieWithError("accept() failed");

        /* clntSock is connected to a client! */
        printf("Handling client %s\n", inet_ntoa(echoClntAddr.sin_addr));

        HandleTCPClient(clntSock);
    }

    /* NOT REACHED */
    return 0;
}

void DieWithError(const char *errorMessage) {
    fprintf(stderr, "%s\n", errorMessage);
    WSACleanup();  /* Clean up Winsock before exiting */
    exit(1);
}

void HandleTCPClient(SOCKET clntSocket) {
    char echoBuffer[RCVBUFSIZE];        /* Buffer for echo string */
    int recvMsgSize;                    /* Size of received message */

    /* Receive message from client */
    if ((recvMsgSize = recv(clntSocket, echoBuffer, RCVBUFSIZE, 0)) < 0)
        DieWithError("recv() failed");

    /* Print the received message to the terminal */
    printf("Received message: %s\n", echoBuffer);

    /* Send received string and receive again until end of transmission */
    while (recvMsgSize > 0) {  /* zero indicates end of transmission */
        /* Echo message back to client */
        if (send(clntSocket, echoBuffer, recvMsgSize, 0) != recvMsgSize)
            DieWithError("send() failed");

        /* See if there is more data to receive */
        if ((recvMsgSize = recv(clntSocket, echoBuffer, RCVBUFSIZE, 0)) < 0)
            DieWithError("recv() failed");
    }

    closesocket(clntSocket);  /* Close client socket */
}
