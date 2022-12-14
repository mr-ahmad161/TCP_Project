#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"
#define buffer_size 50 // Max receiver buffer size

tcp_packet *recvpkt;
tcp_packet *sndpkt;
tcp_packet *receiver_buffer[buffer_size]; // recevier buffer array of tcp_packets

int main(int argc, char **argv) {
        int sockfd; /* socket */
        int portno; /* port to listen on */
        int clientlen; /* byte size of client's address */
        struct sockaddr_in serveraddr; /* server's addr */
        struct sockaddr_in clientaddr; /* client addr */
        int optval; /* flag value for setsockopt */
        FILE *fp;
        char buffer[MSS_SIZE];
        struct timeval tp;
        int next_seqno = 0; // Variable that holds the value for the current required seqno

        // Loop for assigning packets with data_size 0 to the array of the tcp packets
        int iter = 0; // Iterator variable
        while ( iter < buffer_size ) {
                receiver_buffer[iter] = make_packet(MSS_SIZE);
                receiver_buffer[iter]->hdr.data_size = 0; // Assign all created packets to have a data_size of 0 for empty packets
                receiver_buffer[iter]->hdr.seqno = -1; // Assign all packet hdr.seqno's to -1 for exmpty packets
                iter++;
        }

        /*
         * check command line arguments
         */
        if (argc != 3) {
                fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
                exit(1);
        }
        portno = atoi(argv[1]);

        fp  = fopen(argv[2], "w");
        if (fp == NULL) {
                error(argv[2]);
        }

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
                   (const void *)&optval, sizeof(int));

        /*
         * build the server's Internet address
         */
        bzero((char *) &serveraddr, sizeof(serveraddr));
        serveraddr.sin_family = AF_INET;
        serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
        serveraddr.sin_port = htons((unsigned short)portno);

        /*
         * bind: associate the parent socket with a port
         */
        if (bind(sockfd, (struct sockaddr *) &serveraddr,
                 sizeof(serveraddr)) < 0)
                error("ERROR on binding");

        /*
         * main loop: wait for a datagram, then echo it
         */
        VLOG(DEBUG, "epoch time, bytes received, sequence number");
        clientlen = sizeof(clientaddr);
        while (1) {
                /*
                 * recvfrom: receive a UDP datagram from a client
                 */
                //VLOG(DEBUG, "waiting from server \n");
                if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                             (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
                        error("ERROR in recvfrom");
                }
                recvpkt = (tcp_packet *) buffer;
                assert(get_data_size(recvpkt) <= DATA_SIZE);

                if ( recvpkt->hdr.data_size == 0 ) {
                        VLOG(INFO, "End Of File has been reached");
                        fclose(fp);
                        break;
                }

                // If a packet with a higher seqno than currently expected is received
                // check if the packet can be stored in the receiver buffer
                if ( next_seqno < recvpkt->hdr.seqno ) {
                        //printf("Out of order packet received\n");
                        /*
                         * For the receiver buffer, empty buffer packet seqno is -1 with data_size 0 so
                         * the rcvpkt's seqno number does not match with empty packet seqno's
                         */

                        // Check for duplicate buffered packets in the receiver buffer
                        iter = 0;
                        int duplicate = 0;
                        while ( iter<buffer_size ) {
                                if ( receiver_buffer[iter]->hdr.seqno == recvpkt->hdr.seqno ) {
                                        //printf("Duplicate packet not added to present buffer\n");
                                        duplicate = 1;
                                        break;
                                }
                                iter++;
                        }

                        // Only continue processing rcvpkt if it is not a duplicate otherwise drop it
                        if ( duplicate == 0 ) {
                                // Check if buffer is filled up
                                iter = 0;
                                while ( iter<buffer_size ) {
                                        // Once an empty packet has been discovered replace it with our current recvpkt packet
                                        if ( receiver_buffer[iter]->hdr.seqno == -1 && receiver_buffer[iter]->hdr.data_size == 0 )
                                        {
                                                memcpy(receiver_buffer[iter], recvpkt, MSS_SIZE);
                                                break;
                                        }
                                        iter++;
                                }
                        }
                }

                // If the packet is received in order
                else if ( next_seqno == recvpkt->hdr.seqno ) {
                        gettimeofday(&tp, NULL);
                        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

                        fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
                        fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);

                        // Update next_seqno to the next required pkt
                        next_seqno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;

                        // Keep looping through receiver buffer till next required pkt with the correct ack is not found in the buffer
                        int keep_reading = 0;
                        while (1) {
                                iter = 0;
                                keep_reading = 0; // Set to zero at each start till a matching pkt found in receiver buffer
                                while ( iter < buffer_size ) {
                                        // If next required packet is already present in the receiver buffer
                                        if ( receiver_buffer[iter]->hdr.seqno == next_seqno ) {
                                                // Write form the packet in the recevier buffer to the file
                                                fseek(fp, receiver_buffer[iter]->hdr.seqno, SEEK_SET);
                                                fwrite(receiver_buffer[iter]->data, 1, receiver_buffer[iter]->hdr.data_size, fp);

                                                // Update next_seqno to next required pkt
                                                next_seqno = receiver_buffer[iter]->hdr.seqno + receiver_buffer[iter]->hdr.data_size;

                                                // Set the written packet from buffer to be an empty packet
                                                receiver_buffer[iter]->hdr.seqno = -1;
                                                receiver_buffer[iter]->hdr.data_size = 0;
                                                keep_reading = 1; // Set this to 1 if a matching pkt has been found
                                        }
                                        iter++;
                                }
                                // No matching packets were found in the buffer
                                if ( keep_reading == 0) {
                                        break;
                                }
                        }
                }
                // printf("SEND PACKAGE LOOP next seqno = %i and recvpkt->hdr.seqno = %i and sndpck hdr ACK NO %i \n\n", next_seqno, recvpkt->hdr.seqno,sndpkt->hdr.ackno );
                // For out of order packets, send orginial required packet ACK
                sndpkt = make_packet(0);
                sndpkt->hdr.ackno = next_seqno;
                sndpkt->hdr.ctr_flags = ACK;
                // Add the time_stamp value to sndpkt
                sndpkt->hdr.time_stamp = recvpkt->hdr.time_stamp;

                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                           (struct sockaddr *) &clientaddr, clientlen) < 0) {
                        error("ERROR in sendto");
                }
                //printf("sendpacket ACK no %i\n", sndpkt->hdr.ackno );
        }
        return 0;
}
