#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include<sys/time.h>
#include <stdbool.h>

#include "rlib.h"
#include "buffer.h"



// Assume there is one reliable_state struct per connection PER HOST
struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    /* Add your own data fields below this */

    // Sender window
    uint32_t sender_first_unacked;
    uint32_t sender_next;
    uint32_t sender_max_window_size;

    // Receiver window
    uint32_t receiver_next;
    uint32_t receiver_window_size;

    // Sender sequence numbers
    uint32_t sender_next_sequence_number;

    // Timeout
    long timeout;

    // EOF requirements
    bool received_other_EOF;
    bool read_own_EOF;

    // ...
    buffer_t* send_buffer;
    // ...T
    buffer_t* rec_buffer;
    // ...

};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
const struct config_common *cc)
{
     // fprintf(stderr, "rel_create \n");
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
    rel_list->prev = &r->next;
    rel_list = r;

    /* Do any other initialization you need here... */
    // ...

    // struct config_common {
    //     int window;			/* # of unacknowledged packets in flight */
    //     int timer;			/* How often rel_timer called in milliseconds */
    //     int timeout;			/* Retransmission timeout in milliseconds */
    //     int single_connection;        /* Exit after first connection failure */
    // };

    // Sender window
    r->sender_first_unacked = 1;
    r->sender_next = 1;
    r->sender_max_window_size = cc->window;

    // Receiver window
    r->receiver_next = 1;
    r->receiver_window_size = cc->window;

    // Sender sequence numbers
    r->sender_next_sequence_number = 1;

    // Timeout
    r->timeout = (long) cc->timeout;

    // EOF requirements
    r->received_other_EOF = false;
    r->read_own_EOF = false;


    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    // ...
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;
    // ...
    
    return r;
}

void
rel_destroy (rel_t *r)
{
    // fprintf(stderr, "rel_destroy \n");
    if (r->next) {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy (r->c);

    /* Free any other allocated memory here */
    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    // ...

    // TODO: should I do this?
    free(r);

}

void
destroy_if_EOF(rel_t* state) {
    if (!state->read_own_EOF) {
        // // fprintf(stderr, "not destroyed because hasn't read own EOF \n");
        return;
    }
    if (!state->received_other_EOF) {
        // // fprintf(stderr, "not destroyed because hasn't received other EOF \n");
        return;
    }

    if (buffer_size(state->send_buffer) > (uint32_t) 0) {
        if (buffer_get_first(state->send_buffer)->packet.len != (uint16_t) 12) {
            // // fprintf(stderr, "not destroyed because send buffer is not empty or contains not EOF \n");
            return;
        }
    }

    if (buffer_size(state->rec_buffer) != (uint32_t) 0) {
        // // fprintf(stderr, "not destroyed because receive buffer is not empty \n");
        return;
    }
    rel_destroy(state);
}

uint32_t
highest_consequtive_seqno(rel_t* r) {
    buffer_t* receiver_buffer = r->rec_buffer;
    buffer_node_t* first = buffer_get_first(receiver_buffer);
    if (first == NULL) {
        return (uint32_t) 0;
    }
    buffer_node_t* current = first;
    uint32_t highest = (uint32_t) ntohl(current->packet.seqno);
    while (current != NULL
    && current->next != NULL
    && ntohl(current->packet.seqno) + 1 == ntohl(current->next->packet.seqno)) {
        highest = (uint32_t) ntohl(current->next->packet.seqno);
        current = current->next;
    }
    return highest;
}

void
write_buffer_to_output(rel_t *r) {
    if (buffer_size(r->rec_buffer) == (uint32_t) 0) {
        return;
    }

    if (buffer_contains(r->rec_buffer, r->receiver_next)) {
        // fprintf(stderr, "ERROR: buffer contains the next package expected (%u) \n", r->receiver_next);
        buffer_print(r->rec_buffer);
        return;
    }

    // TODO: you can release any packet as long as it has a lower seqno than r->receiver_next

    buffer_node_t* first_buffer_node = buffer_get_first(r->rec_buffer);
    packet_t* first_packet = &first_buffer_node->packet;
    if (ntohl(first_packet->seqno) > r->receiver_next)  {
        // fprintf(stderr, "Buffer has no packets that can be released yet \n");
        return;
    }

    conn_t* c = r->c;   
    buffer_node_t* current_buffer_node = first_buffer_node;
    while (current_buffer_node != NULL) {
        packet_t* current_packet = &current_buffer_node->packet;
        uint32_t current_seqno = ntohl(current_packet->seqno);
        if (current_seqno > r->receiver_next) {
            // fprintf(stderr, "current_seqno > r->receiver_next \n");
            return;  
        }
        size_t data_len = (size_t) ntohs(current_packet->len)-12;
        // char data[data_len];
        // memset(data, '0', data_len);
        // strncpy(data, current_packet->data, data_len);
        if (conn_bufspace(c) < data_len) {
            // fprintf(stderr, "ERROR: conn_bufspace has too little space to write data_len=%zu bytes \n", data_len);
            assert(false);
            return;
        }
        // fprintf(stderr, "conn_output called with data =%s \n", current_packet->data);
        conn_output (c, current_packet->data, data_len);
        buffer_node_t* next_buffer_node = current_buffer_node->next;
        buffer_remove(r->rec_buffer, current_seqno+1);
        current_buffer_node = next_buffer_node;
    }
     // fprintf(stderr, "write_buffer_to_output (end) \n");
    
}   

// n is the expected length of pkt
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{   
    print_pkt (pkt, "rel_recvpkt called with pkt=", n);
    if (ntohs(pkt->len) != (uint16_t) n) {
        // fprintf(stderr, "can't accept packet since pkt->len != n");
        return;
    }
    if (ntohs(pkt->len) > 12) {
        char data[ntohs(pkt->len)-12+1];
        memset(data, '\0', ntohs(pkt->len)-12+1);
        strncpy(data, pkt->data, ntohs(pkt->len)-12);
        // fprintf(stderr, "rel_recvpkt pkt contains length=%u data=%s \n", ntohs(pkt->len)-12+1, data);

        // fprintf(stderr, "data again =");
        for (int i=0; i<ntohs(pkt->len)-12+1; i++) {
            // fprintf(stderr, "%c", data[i]);
        }
        // fprintf(stderr, "\n");
    } else {
        // fprintf(stderr, "rel_recvpkt couldn't print packet \n");
    }
    
    
    // Unexpected length of packet
    // if ((int) htons(pkt->len) != (int) n) {
    //     // fprintf(stderr, "UNEXPECTED LENGTH OF PACKET! htons(pkt->len)=%i, n=%i \n", (int) htons(pkt->len), (int) n);
    //     return;
    // }

    // ACK
    if (htons(pkt->len) == 8) {
        struct ack_packet * ack_packet = (struct ack_packet *) pkt;
        struct ack_packet ack_packet_copy = *ack_packet;
        ack_packet_copy.cksum = (uint16_t) 0;
        uint16_t checksum = (uint16_t) 0;
        checksum = (uint16_t) cksum((char*) &ack_packet_copy, (int) 8);
        if (checksum != ack_packet->cksum) {
            // fprintf(stderr, "CHECKSUM ERROR when receiving ACK packet! \n");
            return;
        }
        uint32_t ACK = (uint32_t) htonl(ack_packet->ackno);
        if (ACK > r->sender_first_unacked) {
            r->sender_first_unacked = ACK;
            buffer_remove(r->send_buffer, r->sender_first_unacked);
        }
        // TODO: should this function call rel_read or send_whole_window
        rel_read(r);
        return;
    }

    packet_t packet_copy = *pkt;
    packet_copy.cksum = (uint16_t) 0;
    uint16_t checksum = (uint16_t) 0;
    checksum = (uint16_t) cksum((char*) &packet_copy, (int) htons(packet_copy.len));
    if (checksum != pkt->cksum) {
        // fprintf(stderr, "CHECKSUM ERROR when receiving (non-ACK) packet! \n");
        return;
    }
    if (htons(pkt->seqno) >= r->receiver_next + r->receiver_window_size) {
        // fprintf(stderr, "ERROR: TOO HIGH SEQUENCE NUMBER \n");
        return;
    }

    // EOF
    if (htons(pkt->len) == 12) {
        r->received_other_EOF = true;
        // Send EOF here
        // packet_t packet;
        // packet.len = htons((uint16_t) 12);
        // packet.ackno = htonl(r->receiver_next); //htonl(s->receiver_next); // TODO: set ackno later too (might be outdated if packet waits in the buffer)
        // packet.seqno = htonl((r->sender_next_sequence_number)++);
        // // memset(packet.data, '\0', sizeof(packet.data));
        // packet.cksum = (uint16_t) 0;
        // packet.cksum = (uint16_t) cksum((char*) &packet, (int) 12);

        // buffer_t* send_buffer = r->send_buffer;
        // buffer_insert(send_buffer, &packet, (long) 0);


        //
        // conn_output(r->c, NULL, (size_t) 0); // Sends EO
    }

    buffer_t* receiver_buffer = r->rec_buffer;
    if (!buffer_contains(receiver_buffer, htonl(pkt->seqno)) && htonl(pkt->seqno) >= r->receiver_next) {
        buffer_insert(receiver_buffer, pkt, (long) -1);
    }

    if (htonl(pkt->seqno) == r->receiver_next) {
         // fprintf(stderr, "highest_consequtive_seqno(r) = %u \n", highest_consequtive_seqno(r));
        r->receiver_next = highest_consequtive_seqno(r) + 1;
    }

    write_buffer_to_output(r);

    // TODO: send ACK
    // CONTINUE HERE BY SENDING THE ACK!!
    struct ack_packet ack_packet;
    ack_packet.len = ntohs(8);
    ack_packet.ackno = ntohl(r->receiver_next);
    ack_packet.cksum = (uint16_t) 0;
    uint16_t ack_checksum = (uint16_t) 0;
    ack_checksum = (uint16_t) cksum((char*) &ack_packet, (int) 8);
    ack_packet.cksum = ack_checksum;
    const packet_t * ack_packet_packet_t = (const packet_t *) &ack_packet;
    conn_t* c = r->c;
    conn_sendpkt(c, ack_packet_packet_t, 8);
    // fprintf(stderr, "should've sent ACK \n");

    
    // TODO: call a function that calls conn_output on the buffer

    /* Your logic implementation here */
    
    destroy_if_EOF(r);
    
}

long get_time_ms(void) {
    struct timeval tval;
    gettimeofday(&tval,NULL);
    long long time = (((long long)tval.tv_sec)*1000)+(tval.tv_usec/1000);
    return (long) (time % __LONG_MAX__);
}


// TODO: assume that all ACKed packets get instantly removed from the buffer
void
send_whole_window(rel_t *s) {
    conn_t *c = s->c;
    buffer_t* send_buffer = s->send_buffer;
    buffer_node_t* first = buffer_get_first(send_buffer);
    buffer_node_t* current = first;
    while (current != NULL
            && ntohl(current->packet.seqno) < s->sender_first_unacked + s->sender_max_window_size ) {
            // if (ntohs(current->packet.len) == (uint16_t) 12 && buffer_size(s->rec_buffer) > (uint32_t) 1) {
            //     // fprintf(stderr, "can't send EOF yet because rec_buffer is not empty \n");
            //     continue;
            // }
            
            if (get_time_ms() - current->last_retransmit > s->timeout) {
                    const packet_t * packet = &(current->packet);
                    conn_sendpkt(c, packet, htons(packet->len));
                    // fprintf(stderr, "should've sent a package \n");
                    print_pkt(packet, "sent this package", htons(packet->len));
                    current->last_retransmit = get_time_ms();
                if (ntohl(current->packet.seqno) == s->sender_next) {
                    s->sender_next++;
                }      
            }
        current = current->next;
    }
}

void
rel_read (rel_t *s)
{
    /* Your logic implementation here */
    // "Do not accept more into your send buffer than the sliding window permits"
    if (s->read_own_EOF == true) {
        // fprintf(stderr, "can't read because s->read_own_EOF \n");
        return;
    }
    
    if (buffer_size(s->send_buffer) >= s->sender_max_window_size) {
        // fprintf(stderr, "can't read because buffer_size(s->send_buffer) >= s->sender_max_window_size) \n");
        return;
    }

    conn_t *c = s->c;

    packet_t packet;
    // char data[500];
    // memset(data, '\0', sizeof(data));
    int bytes_received = conn_input (c, packet.data, (size_t) 500);
    
    // fprintf(stderr, "conn_input wrote to data =%s \n", packet.data);
     // fprintf(stderr, "conn_input read so many bytes = %i \n", bytes_received);
    if (bytes_received > 0 && bytes_received < 500) {
        // fprintf(stderr, "read less than 500 bytes from input but that's ok \n");
    }
    if (bytes_received == 0) {
        // fprintf(stderr, "read 0 bytes from conn_input \n");
        return;
    }
    if (bytes_received == -1) {
        // fprintf(stderr, "read -1 bytes from conn_input \n");
        s->read_own_EOF = true;
        bytes_received = 0;
    }

    
    packet.len = htons((uint16_t) bytes_received+12);
    packet.ackno = htonl(s->receiver_next); //htonl(s->receiver_next); // TODO: set ackno later too (might be outdated if packet waits in the buffer)
    packet.seqno = htonl((s->sender_next_sequence_number)++);
    //memset(packet.data, '\0', bytes_received);
    //memcpy(packet.data, data, bytes_received); // AVOID USING STRNCPY
    packet.cksum = (uint16_t) 0;
    packet.cksum = (uint16_t) cksum((char*) &packet, (int) bytes_received+12);

    buffer_t* send_buffer = s->send_buffer;
    buffer_insert(send_buffer, &packet, (long) 0);
    send_whole_window(s);
    destroy_if_EOF(s);
    if (bytes_received > 0) {
        rel_read(s);
    }
     // fprintf(stderr, "rel_read (end) \n");
}

void
rel_output (rel_t *r)
{
    /* Your logic implementation here */
     // fprintf(stderr, "ERROR SHOULD NEVER BE CALLED: rel_output \n");
     assert(1 == 0);
}

void
rel_timer ()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    while (current != NULL) {
        // ...
        send_whole_window(current);
        write_buffer_to_output(current);
        destroy_if_EOF(current);
        current = current->next;
    }
    
}
