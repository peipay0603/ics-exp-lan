#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#define MAX_PACKET_SIZE 65536

/**
 * チェックサムの計算。今回は触らなくて良い
 */
uint16_t checksum(const void *buf, int len) {
    const uint8_t *data = buf;
    uint32_t sum;

    for (sum = 0; len >= 2; data += 2, len -= 2) {
        sum += data[0] << 8 | data[1];
    }
    if (len > 0) {
        sum += data[0] << 8;
    }
    while (sum > 0xffff) {
        sum = (sum >> 16) + (sum & 0xffff);
    }
    sum = htons(~sum);
    return sum ? sum : 0xffff;
}

/**
 * UDPのチェックサム計算に用いるための疑似ヘッダ
 */
struct pseudoHeader {
    struct in_addr ip_src;
    struct in_addr ip_dst;
    uint8_t zero;
    uint8_t ip_p;
    uint16_t len;
} __attribute__((packed)); // パディングを抑制

/**
 * UDPのチェックサム計算
 */
uint16_t udpChecksum(const void *packet)
{
    const struct ip *ipHeader = (struct ip *)packet;
    const struct udphdr *udpHeader = (struct udphdr *)(((const uint8_t *)packet) + sizeof(struct ip));

    uint16_t udpLen = udpHeader->len;

    int checksumLen = sizeof(struct pseudoHeader) + ntohs(udpLen);
    uint8_t *checksumBuf = malloc(checksumLen);

    struct pseudoHeader *phdr = (struct pseudoHeader *)checksumBuf;
    phdr->ip_src = ipHeader->ip_src;
    phdr->ip_dst = ipHeader->ip_dst;
    phdr->zero = 0;
    phdr->ip_p = ipHeader->ip_p;
    phdr->len = udpLen;

    memcpy(checksumBuf + sizeof(struct pseudoHeader), udpHeader, ntohs(udpLen));

    uint16_t result = checksum(checksumBuf, checksumLen);
    free(checksumBuf);
    return result;
}

int main(int argc, char **argv) {
    in_addr_t localhost = inet_addr("127.0.0.1");
    in_addr_t srcIp = argc > 1 ? inet_addr(argv[1]) : localhost;
    in_addr_t dstIp = argc > 2 ? inet_addr(argv[2]) : localhost;

    /**
     * Raw socketの作成
     */
    int s = socket(PF_INET, SOCK_RAW, IPPROTO_RAW);
    if (s < 0) {
        perror("Socket作成に失敗");
        return 1;
    }

    /**
     * IPヘッダ、UDPヘッダ、ペイロードを詰め込むためのバッファ
     * (必ずしも全部使い切る必要はない)
     */
    uint8_t *packet = malloc(MAX_PACKET_SIZE);

    /**
     * 穴埋め: 以下のIPヘッダ部を正しく埋めよ
     * ip_v: Version
     * ip_hl: IHL
     * ip_ttl: TTL
     * ip_p: Protocol
     */
    struct ip *ipHeader = (struct ip *)packet;
    ipHeader->ip_v = XXX;
    ipHeader->ip_hl = XXX;
    ipHeader->ip_tos = 0;
    ipHeader->ip_id = 0;
    ipHeader->ip_ttl = XXX;
    ipHeader->ip_p = IPPROTO_UDP;
    ipHeader->ip_src.s_addr = srcIp;
    ipHeader->ip_dst.s_addr = dstIp;

    /**
     * 穴埋め: 以下のUDPヘッダ部を正しく埋めよ
     * source: 送信元ポート
     * dest: 送信先ポート
     */
    struct udphdr *udpHeader = (struct udphdr *)(packet + sizeof(struct ip));
    udpHeader->source = XXX;
    udpHeader->dest = XXX;
    udpHeader->check = 0;

    uint8_t *payload = packet + sizeof(struct ip) + sizeof(struct udphdr);

    size_t n = 0;
    char *line = NULL;
    while (1) {
        /**
         * 標準入力から文字を受け取る
         */
        ssize_t len = getline(&line, &n, stdin);
        if (len == -1) {
            break;
        }

        /**
         * 受け取った文字数に応じてTotal Lengthを設定
         */
        size_t totalLength = sizeof(struct ip) + sizeof(struct udphdr) + len;
        if (totalLength > MAX_PACKET_SIZE) {
            printf("文字が多すぎてパケットの最大容量を超えました\n");
            continue;
        }

        memcpy(payload, line, len);

        /**
         * 穴埋め: ip_len = IPパケット全体の長さ、Total Lengthとなっている。正しく埋めよ
         * (ip_sumはチェックサム)
         */
        ipHeader->ip_len = XXX;
        ipHeader->ip_sum = checksum(ipHeader, sizeof(struct ip));

        /**
         * 穴埋め: len = UDPヘッダ+ペイロードの長さ、となっている。正しく埋めよ
         * (checkはチェックサム)
         */
        udpHeader->len = XXX;
        udpHeader->check = udpChecksum(packet);

        /**
         * 送信先の設定
         */
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = dstIp;
        memset(addr.sin_zero, 0, sizeof(addr.sin_zero));

        ssize_t bytes_sent = sendto(s, packet, totalLength, 0, (const struct sockaddr *)&addr, sizeof(addr));
        if (bytes_sent == -1) {
            printf("パケット送信中にエラー\n");
            continue;
        }
        printf("%ld bytes 送信しました\n", bytes_sent);
    }

    free(packet);
    free(line);
    close(s);
    return 0;
}
