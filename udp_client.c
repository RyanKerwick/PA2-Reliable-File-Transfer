/* 
 * udpclient.c - A simple UDP client
 * usage: udpclient <host> <port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>

#define BUFSIZE 1024
#define FLAG_LAST 0x0001
#define FLAG_CMD  0x0002
#define FLAG_ACK  0x0004
#define MAX_DATA 8192
#define PKT_SIZE (sizeof(PktHdr) + MAX_DATA)

/**
* Protocol header struct
*/
typedef struct {
    uint32_t seq;   // sequence #
    uint16_t len;   // length of packet in bytes
    uint16_t flag;
} PktHdr;


/* 
 * error - wrapper for perror
 */
void error(char *msg) {
    perror(msg);
    exit(0);
}


/*
* send_ack - constructs and sends acknowledgement to server
*   seq - sequence number for acknowledgement
*/
void send_ack(int seq, int sockfd, struct sockaddr_in *dest, socklen_t destlen) {
    PktHdr hdr;

    hdr.seq = htonl(seq);
    hdr.len = htons(0);
    hdr.flag = htons(FLAG_ACK);

    ssize_t n = sendto(sockfd, &hdr, sizeof(hdr), 0,
                        (struct sockaddr *)dest, destlen);
    if (n < 0) error("ERROR in sendto\n");
}

int receive_ack(int sockfd, struct sockaddr_in *dest, socklen_t *destlen) {
  uint8_t pkt[PKT_SIZE];
  PktHdr hdr;
  
  ssize_t n = recvfrom(sockfd, pkt, sizeof(pkt), 0,
    (struct sockaddr *)dest, destlen);

  if (n <= 0) {
    printf("[RECEIVE_ACK] Packet is empty\n");
    return -1;
  }
  
  // Parse command out of request
  if ((size_t)n != sizeof(PktHdr)) {
      printf("Malformed request\n");
      return -1;
  }

  memcpy(&hdr, pkt, sizeof(hdr));
  uint32_t seq = ntohl(hdr.seq);
  uint16_t len = ntohs(hdr.len);
  uint16_t flag = ntohs(hdr.flag);
  
  if (!(flag & FLAG_ACK) || len != 0) {      // If the packet is not an ACK, return error (use most recent seq # in main function)
    printf("[RECEIVE_ACK] Flag is not set OR len is not 0\n");
    return -1;
  }

  return (int)seq;
}

void send_pkt_rcv_ack(int seq, uint8_t *pkt, int pkt_size, int sockfd, struct sockaddr_in *dest, socklen_t *destlen) {
    ssize_t n;
    int retries = 0;
    while (1) {
        n = sendto(sockfd, pkt, pkt_size, 0, (struct sockaddr *)dest, *destlen);
        if (n < 0) 
            error("ERROR in sendto\n");

        int ack = receive_ack(sockfd, dest, destlen);
        if (ack == seq) {
            break;
        }

        printf("[SPRA] Out of order packet seq: %d, expecting: %d\n", ack, seq);

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            retries++;
            if (retries > 2) {
                error("Server not responding\n");
            }
        }
    }
}

int rcv_pkt_send_ack(int seq, uint8_t *pkt, int pkt_size, int sockfd, struct sockaddr_in *dest, socklen_t *destlen) {
    ssize_t n;
    PktHdr hdr;
    n = recvfrom(sockfd, pkt, pkt_size, 0, (struct sockaddr *)dest, destlen);
    if (n < 0)
        return -1;


    if ((size_t)n < sizeof(PktHdr)) {
        printf("Malformed response\n");
        return -1;
    }

    memcpy(&hdr, pkt, sizeof(hdr));

    uint32_t curr_seq = ntohl(hdr.seq);
    uint16_t len = ntohs(hdr.len);

    if ((size_t)n != sizeof(PktHdr) + len) {
        printf("[RPSA] Incorrect size of packet");
        return -1;
    }

    if (seq == (int)curr_seq) {
        printf("Sending ack with #: %d\n", seq);
        send_ack(seq, sockfd, dest, *destlen);
        return 1;
    }
    else {
        printf("[RPSA] Out of order packet seq: %d, expecting: %d\n", curr_seq, seq);
        uint32_t last_good = seq - 1;
        send_ack(last_good, sockfd, dest, *destlen);
        return 0; // out of order
    }
}

int main(int argc, char **argv) {
    int sockfd, portno;
    socklen_t serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    char buf[BUFSIZE];
    int valid;

    /* check command line arguments */
    if (argc != 3) {
       fprintf(stderr,"usage: %s <hostname> <port>\n", argv[0]);
       exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket\n");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, 
	  (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);
    serverlen = sizeof(serveraddr);

    /* add in 5 second timeout */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 20000;

    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,
            &tv, sizeof(tv));

    while (1) {
        FILE *fptr = NULL;
        uint8_t pkt[PKT_SIZE];
        PktHdr hdr;
        uint32_t seq = 1;

        /* get a message from the user */
        bzero(buf, BUFSIZE);
        printf("Please enter any of the following commands:\n");
        printf(" get [file_name]\n put [file_name]\n delete [file_name]\n ls\n exit\n\n");
        if (fgets(buf, BUFSIZE, stdin) == NULL) {
            error("ERROR in fgets\n");
            continue;   // or break;
        }
        buf[strcspn(buf, "\n")] = '\0';

        char temp_buf[BUFSIZE];
        strcpy(temp_buf, buf);

        char *command = strtok(temp_buf, " ");
        char *file_name = strtok(NULL, "");
        
        if (!command) {
            printf("Invalid input\n");
            continue;
        }

        if (strcmp(command, "get") == 0) {
            /* Case for get */
            seq = 1;

            if (!file_name) {
                printf("File name required\n");
                continue;
            }

            fptr = fopen(file_name, "wb") ;
            if (!fptr) {
                error("ERROR in fopen\n");
            }

            hdr.seq = htonl(seq);
            hdr.len = htons(strlen(buf));
            hdr.flag = htons(FLAG_CMD | FLAG_LAST);

            memcpy(pkt, &hdr, sizeof(PktHdr));
            memcpy(pkt + sizeof(PktHdr), buf, strlen(buf));
            size_t pkt_size = sizeof(PktHdr) + strlen(buf);

            /* send the command to the server */
            send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &serveraddr, &serverlen);

            seq++;

            // seq = 2 here.
            while (1) {
                valid = rcv_pkt_send_ack(seq, pkt, sizeof(pkt), sockfd, &serveraddr, &serverlen);

                if (valid == -1) {
                    continue;
                }

                if (valid == 0) {
                    continue;
                }

                memcpy(&hdr, pkt, sizeof(hdr));

                uint16_t len = ntohs(hdr.len);
                uint16_t flag = ntohs(hdr.flag);

                if (flag & FLAG_CMD) {
                    continue;
                }
                
                if (len > 0) {
                    size_t written = fwrite(pkt + sizeof(PktHdr), 1, len, fptr);
                    if (written != len) {
                        fclose(fptr);
                        error("ERROR in fwrite\n");
                    }
                }

                if (flag & FLAG_LAST) {
                    break;
                }

                seq++;
            }
            fclose(fptr);
            printf("File content downloaded to %s\n", file_name);
            
            // printf("[GET] Final seq #: %d\n", seq);
        }
        else if (strcmp(command, "put") == 0) {
            /* Case for put */
            seq = 1;

            if (!file_name) {
                printf("File name required\n");
                continue;
            }

            fptr = fopen(file_name, "rb");
            if (!fptr) {
                error("ERROR in fopen\n");
            }

            hdr.seq = htonl(seq);
            hdr.len = htons(strlen(buf));
            hdr.flag = htons(FLAG_CMD | FLAG_LAST);

            memcpy(pkt, &hdr, sizeof(PktHdr));
            memcpy(pkt + sizeof(PktHdr), buf, strlen(buf));
            size_t pkt_size = sizeof(PktHdr) + strlen(buf);

            /* send the command to the server */
            send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &serveraddr, &serverlen);

            seq++;   
            
            // receiving mode
            uint8_t data[MAX_DATA];

            while(1) {
                size_t nread = fread(data, 1, MAX_DATA, fptr);

                if (nread == 0) {
                    if (feof(fptr)) break;
                    error("ERROR in fread\n");
                }

                hdr.seq = htonl(seq);
                hdr.len = htons((uint16_t)nread);
                hdr.flag = htons(0);

                memcpy(pkt, &hdr, sizeof(hdr));
                memcpy(pkt + sizeof(hdr), data, nread);

                size_t pkt_size = sizeof(hdr) + nread;

                send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &serveraddr, &serverlen);

                seq++;
            }

            fclose(fptr);

            hdr.seq = htonl(seq);
            hdr.len = htons(0);
            hdr.flag = htons(FLAG_LAST);

            memcpy(pkt, &hdr, sizeof(PktHdr));

            pkt_size = sizeof(PktHdr);

            send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &serveraddr, &serverlen);

            seq++;

            // Wait for status packet
            while (1) {

                int valid = rcv_pkt_send_ack(seq, pkt, sizeof(pkt), sockfd, &serveraddr, &serverlen);

                if (valid <= 0)
                    continue;

                // valid == 1
                break;
            }

            memcpy(&hdr, pkt, sizeof(hdr));

            uint16_t len = ntohs(hdr.len);
            uint16_t flag = ntohs(hdr.flag);

            /* Validate */
            if (!(flag & FLAG_CMD) || !(flag & FLAG_LAST)) {
                printf("Unexpected status packet\n");
                continue;
            }

            /* Extract status message */
            char status[1024];

            if (len >= sizeof(status))
                len = sizeof(status) - 1;

            memcpy(status, pkt + sizeof(PktHdr), len);
            status[len] = '\0';

            printf("%s", status);

            // printf("[PUT] Final seq #: %d\n", seq);
        }
        else if (strcmp(command, "delete") == 0) {
            /* Case for delete */
            seq = 1;


            if (!file_name) {
                printf("File name required\n");
                continue;
            }
            
            /* send the message to the server */
            hdr.seq = htonl(seq);
            hdr.len = htons(strlen(buf));
            hdr.flag = htons(FLAG_CMD | FLAG_LAST);

            memcpy(pkt, &hdr, sizeof(PktHdr));
            memcpy(pkt + sizeof(PktHdr), buf, strlen(buf));
            size_t pkt_size = sizeof(PktHdr) + strlen(buf);
            
            send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &serveraddr, &serverlen);

            seq++;      // Increment for status packet
            // seq = 2 here.
            while (1) {

                int valid = rcv_pkt_send_ack(seq, pkt, sizeof(pkt), sockfd, &serveraddr, &serverlen);

                if (valid <= 0) {
                    printf("[DELETE] wrong packet fish\n");
                    continue;
                }

                // valid == 1
                break;
            }

            memcpy(&hdr, pkt, sizeof(hdr));

            uint16_t len = ntohs(hdr.len);
            uint16_t flag = ntohs(hdr.flag);

            /* Validate */
            if (!(flag & FLAG_CMD) || !(flag & FLAG_LAST)) {
                printf("Unexpected status packet\n");
                continue;
            }

            /* Extract status message */
            char status[1024];

            if (len >= sizeof(status))
                len = sizeof(status) - 1;

            memcpy(status, pkt + sizeof(PktHdr), len);
            status[len] = '\0';

            printf("%s", status);

            // printf("[DELETE] Final seq #: %d\n", seq);
        }
        else if (strcmp(command, "ls") == 0) {
            
            /* send the message to the server */
            hdr.seq = htonl(seq);
            hdr.len = htons(strlen(buf));
            hdr.flag = htons(FLAG_CMD | FLAG_LAST);

            memcpy(pkt, &hdr, sizeof(PktHdr));
            memcpy(pkt + sizeof(PktHdr), buf, strlen(buf));
            size_t pkt_size = sizeof(PktHdr) + strlen(buf);
            
            send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &serveraddr, &serverlen);

            seq++;

            while (1) {
                valid = rcv_pkt_send_ack(seq, pkt, sizeof(pkt), sockfd, &serveraddr, &serverlen);

                if (valid == -1) {
                    continue;
                }

                if (valid == 0) {
                    continue;
                }

                memcpy(&hdr, pkt, sizeof(hdr));

                uint16_t len = ntohs(hdr.len);
                uint16_t flag = ntohs(hdr.flag);

                if (len > 0) {
                    fwrite(pkt + sizeof(PktHdr), 1, len, stdout);
                }

                if (flag & FLAG_LAST)
                    break;
                
                seq++;
            }

            // printf("[LS] Final seq #: %d\n", seq);
        }
        else if (strcmp(command, "exit") == 0) {
            
            /* send the message to the server */
            hdr.seq = htonl(seq);
            hdr.len = htons(strlen(buf));
            hdr.flag = htons(FLAG_CMD | FLAG_LAST);

            memcpy(pkt, &hdr, sizeof(PktHdr));
            memcpy(pkt + sizeof(PktHdr), buf, strlen(buf));
            size_t pkt_size = sizeof(PktHdr) + strlen(buf);

            send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &serveraddr, &serverlen);

            seq++;

            // printf("[EXIT] Final seq #: %d\n", seq);
        }
        else {
            printf("Invalid input");
        }
    }
    return 0;
}
