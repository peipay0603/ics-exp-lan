#include "icslab2_net.h"

#define MAX_CLIENTS 16

int main(int argc, char **argv)
{
    int sock, n, i, index, clients_count = 0;
    int port = argc > 1 ? atoi(argv[1]) : 10001;
    struct sockaddr_in serverAddr, clientAddr, clients[MAX_CLIENTS];
    socklen_t addrLen;
    char buf[BUF_LEN + 1];

    if(port < 1 || port > 65535)
        return 1;
    /* STEP 1: UDPソケットを作り、ポートを設定する */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0) {
        perror("socket");
        return 1;
    }
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    while(1) {
        /* STEP 2: 送信元を参加者として登録する */
        addrLen = sizeof(clientAddr);
        n = recvfrom(sock, buf, BUF_LEN, 0, (struct sockaddr *)&clientAddr, &addrLen);
        if(n < 0) {
            perror("recvfrom");
            break;
        }
        index = -1;
        for(i = 0; i < clients_count; i++) {
            if(clients[i].sin_addr.s_addr == clientAddr.sin_addr.s_addr
               && clients[i].sin_port == clientAddr.sin_port)
                index = i;
        }
        if(n >= 4 && strncmp(buf, "quit", 4) == 0) {
            sendto(sock, buf, n, 0, (struct sockaddr *)&clientAddr, addrLen);
            if(index >= 0)
                clients[index] = clients[--clients_count];
            continue;
        }
        if(index < 0) {
            if(clients_count == MAX_CLIENTS) {
                const char *message = "server full\n";
                sendto(sock, message, strlen(message), 0, (struct sockaddr *)&clientAddr, addrLen);
                continue;
            }
            clients[clients_count++] = clientAddr;
        }

        /* STEP 3: 受信したデータを参加者全員へ送る */
        for(i = 0; i < clients_count; i++) {
            if(sendto(sock, buf, n, 0, (struct sockaddr *)&clients[i], sizeof(clients[i])) != n)
                perror("sendto");
        }
    }
    close(sock);
    return 1;
}
