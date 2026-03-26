/* 
 * udpserver.c - A simple UDP echo server 
 * usage: udpserver <port>
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/time.h>

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
  exit(1);
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
          printf("Received ack with #: %d\n", seq);
            break;
        }

        printf("[SPRA] Out of order packet seq: %d, expecting: %d\n", ack, seq);

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            retries++;
            if (retries > 5) {
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

    if (seq == (int)curr_seq) {
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
  int sockfd; /* socket */
  int portno; /* port to listen on */
  socklen_t clientlen; /* byte size of client's address */
  struct sockaddr_in serveraddr; /* server's addr */
  struct sockaddr_in clientaddr; /* client addr */
  struct hostent *hostp; /* client host info */
  char *hostaddrp; /* dotted decimal host addr string */
  int optval; /* flag value for setsockopt */


  /* 
   * check command line arguments 
   */
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  portno = atoi(argv[1]);

  /* 
   * socket: create the parent socket 
   */
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) 
    error("ERROR opening socket");

  /* setsockopt: Handy debugging trick that lets 
   * us rerun the server immediately after we kill it; 
   * otherwise we have to wait about 20 secs. 
   * Eliminates "ERROR on binding: Address already in use" error. 
   */
  optval = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
	     (const void *)&optval , sizeof(int));

  /*
   * build the server's Internet address
   */
  bzero((char *) &serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons((unsigned short)portno);


  /* add in 5 second timeout */
  struct timeval tv;
  tv.tv_sec = 0;      // 5 seconds
  tv.tv_usec = 20000;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,
          &tv, sizeof(tv));
  /* 
   * bind: associate the parent socket with a port 
   */
  if (bind(sockfd, (struct sockaddr *) &serveraddr, 
	   sizeof(serveraddr)) < 0) 
    error("ERROR on binding");

  /* 
   * main loop: wait for a datagram, then echo it
   */
  clientlen = sizeof(clientaddr);
  while (1) {

    FILE *fptr = NULL;
    uint8_t pkt[PKT_SIZE];
    PktHdr hdr;
    uint32_t seq = 1;

    /*
     * recvfrom: receive a UDP datagram from a client
     * The server expects a command first, so we pull that out here.
     */
    int valid = rcv_pkt_send_ack(seq, pkt, sizeof(pkt), sockfd, &clientaddr, &clientlen);

    if (valid == -1) {
      continue; // malformed packet
    }

    if (valid == 0) {
      continue; // out of order packet
    }

    seq++;

    /* 
     * gethostbyaddr: determine who sent the datagram
     */
    hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, 
			  sizeof(clientaddr.sin_addr.s_addr), AF_INET);
    if (hostp == NULL)
      error("ERROR on gethostbyaddr");
    hostaddrp = inet_ntoa(clientaddr.sin_addr);
    if (hostaddrp == NULL)
      error("ERROR on inet_ntoa\n");
    printf("server received datagram from %s (%s)\n", 
	   hostp->h_name, hostaddrp);
    // printf("server received %ld/%d bytes: %s\n", strlen(pkt), n, pkt);
    
    // Parse command out of request
    memcpy(&hdr, pkt, sizeof(hdr));

    uint16_t len  = ntohs(hdr.len);
    uint16_t flag = ntohs(hdr.flag);

    if (!(flag & FLAG_CMD)) {
        // Ignore and keep waiting
        continue;
    }

    char request[MAX_DATA + 1];
    memcpy(request, pkt + sizeof(PktHdr), len);
    request[len] = '\0';

    char *command = strtok(request, " ");
    char *file_name = strtok(NULL, "");

    if (command == NULL) {
      printf("malformed request");
      continue;
    }
    
    /*
    * Case PUT: go into data receiving mode, and write data from client to file
    */
    if (strcmp(command, "put") == 0) {
      // Case for put
      // seq = 2

      fptr = fopen(file_name, "wb") ;
      if (!fptr) {
          error("ERROR in fopen\n");
      }

      while (1) {
          valid = rcv_pkt_send_ack(seq, pkt, sizeof(pkt), sockfd, &clientaddr, &clientlen);

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
            continue;         // Unexpected command in data receiving
          }

          if (len > 0) {
              size_t written = fwrite(pkt + sizeof(PktHdr), 1, len, fptr);
              if (written != len) {
                  fclose(fptr);
                  error("ERROR in fwrite\n");
              }
          }

          seq++;

          if (flag & FLAG_LAST) {
              break;
          }

      }
      fclose(fptr);
      printf("File content uploaded to %s\n", file_name);

      // Send success packet for put
      const char *msg = "200 OK\n";

      hdr.seq = htonl(seq);
      hdr.len = htons(strlen(msg));
      hdr.flag = htons(FLAG_CMD | FLAG_LAST);
      
      memcpy(pkt, &hdr, sizeof(PktHdr));
      memcpy(pkt + sizeof(PktHdr), msg, strlen(msg));
      size_t pkt_size = sizeof(PktHdr) + strlen(msg);

      clientlen = sizeof(clientaddr);
      send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &clientaddr, &clientlen);
            
      // printf("[PUT] Final seq #: %d\n", seq);
    }

    /*
    * Case GET: Go into data sending mode, read data from file and send
    */
    else if (strcmp(command, "get") == 0) {
      // Case for get
      // seq = 2

      fptr = fopen(file_name, "rb") ;
      if (!fptr) {
          error("ERROR in fopen\n");
      }

      uint8_t data[MAX_DATA];

      while (1) {
        size_t nread = fread(data, 1, MAX_DATA, fptr);

        if (nread == 0) {
          if (feof(fptr)) {
            break;
          }
          error("ERROR in fread\n");
        }

        hdr.seq = htonl(seq);
        hdr.len = htons((uint16_t)nread);
        hdr.flag = htons(0);

        memcpy(pkt, &hdr, sizeof(hdr));
        memcpy(pkt + sizeof(hdr), data, nread);

        size_t pkt_size = sizeof(hdr) + nread;

        send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &clientaddr, &clientlen);

        seq++;
      }
      fclose(fptr);

      hdr.seq = htonl(seq);
      hdr.len = htons(0);
      hdr.flag = htons(FLAG_LAST);

      memcpy(pkt, &hdr, sizeof(hdr));

      size_t pkt_size = sizeof(PktHdr);

      send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &clientaddr, &clientlen);

      printf("Sequence number for server after get: %d\n", seq);
            
      // printf("[GET] Final seq #: %d\n", seq);
    }

    /*
    * Case DELETE: Delete file, send confirmation
    */
    else if (strcmp(command, "delete") == 0) {
      // Case for delete
      // seq = 2

      if (remove(file_name) != 0) {
          error("ERROR in remove\n");
          continue;
      }

      // Send success packet for put
      const char *msg = "200 OK\n";

      hdr.seq = htonl(seq);
      hdr.len = htons(strlen(msg));
      hdr.flag = htons(FLAG_CMD | FLAG_LAST);
      
      memcpy(pkt, &hdr, sizeof(PktHdr));
      memcpy(pkt + sizeof(PktHdr), msg, strlen(msg));
      size_t pkt_size = sizeof(PktHdr) + strlen(msg);

      clientlen = sizeof(clientaddr);

      send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &clientaddr, &clientlen);
            
      // printf("[DELETE] Final seq #: %d\n", seq);
    }

    /*
    * Case LS: Read directory, send back information
    */
    else if (strcmp(command, "ls") == 0) {
      // Case for ls
      // seq = 2

      DIR *dir = opendir(".");
      if (!dir) {
          error("ERROR in opendir\n");
      }

      char outbuf[BUFSIZE];
      outbuf[0] = '\0';

      struct dirent *entry;

      while ((entry = readdir(dir)) != NULL) {
         // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        // Check remaining space before appending
        size_t remaining = BUFSIZE - strlen(outbuf) - 1;
        if (remaining <= strlen(entry->d_name) + 1) {
            // No more room
            break;
        }

        strcat(outbuf, entry->d_name);
        strcat(outbuf, "\n");
      }

      closedir(dir);

      hdr.seq = htonl(seq);
      hdr.len = htons(strlen(outbuf));
      hdr.flag = htons(FLAG_CMD | FLAG_LAST);
      
      memcpy(pkt, &hdr, sizeof(PktHdr));
      memcpy(pkt + sizeof(PktHdr), outbuf, strlen(outbuf));
      size_t pkt_size = sizeof(PktHdr) + strlen(outbuf);

      clientlen = sizeof(clientaddr);
      send_pkt_rcv_ack(seq, pkt, pkt_size, sockfd, &clientaddr, &clientlen);
            
      // printf("[LS] Final seq #: %d\n", seq);
    }

    /*
    * Case EXIT: return
    */
    else if (strcmp(command, "exit") == 0) {
      // Case for exit

      return 0;
            
      // printf("[EXIT] Final seq #: %d\n", seq);
    }
    else {
      printf("Unknown command ignored.\n");
    }
  }
}