/**
 * @file
 * @author Edward A. Lee (eal@berkeley.edu)
 *
 * @section LICENSE
Copyright (c) 2020, The University of California at Berkeley.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 * @section DESCRIPTION
 * Utility functions for a federate in a federated execution.
 * The main entry point is synchronize_with_other_federates().
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>      // Defined perror(), errno
#include <sys/socket.h>
#include <netinet/in.h> // Defines struct sockaddr_in
#include <arpa/inet.h>  // inet_ntop & inet_pton
#include <unistd.h>     // Defines read(), write(), and close()
#include <netdb.h>      // Defines gethostbyname().
#include <strings.h>    // Defines bzero().
#include <pthread.h>
#include <assert.h>
#include "util.c"       // Defines error() and swap_bytes_if_big_endian().
#include "rti.h"        // Defines TIMESTAMP.
#include "reactor.h"    // Defines instant_t.

// Error messages.
char* ERROR_SENDING_HEADER = "ERROR sending header information to federate via RTI";
char* ERROR_SENDING_MESSAGE = "ERROR sending message to federate via RTI";

/**
 * The ID of this federate as assigned when the generated function
 * __initialize_trigger_objects() is called
 * (@see xtext/org.icyphy.linguafranca/src/org/icyphy/generator/CGenerator.xtend).
 */
ushort _lf_my_fed_id = 0;

/**
 * The socket descriptor for this federate to communicate with the RTI.
 * This is set by connect_to_rti(), which must be called before other
 * functions that communicate with the rti are called.
 */
int _lf_rti_socket = -1;

/**
 * Number of inbound physical connections to the federate.
 */
int _lf_number_of_inbound_physical_connections;

/**
 * Number of outbound physical connections from the federate.
 */
int _lf_number_of_outbound_physical_connections;

/**
 * An array that holds the socket descriptors for inbound physical
 * connections from each federate. The index will be the federate
 * ID of the remote sending federate. This is initialized at startup
 * to -1 and is set to a socket ID by handle_p2p_connections_from_federates()
 * when the socket is opened.
 * 
 * @note There will not be an inbound socket unless a physical connection
 * is specified in the Lingua Franca program where this federate is the
 * destination. Multiple incoming physical connections from the same remote
 * federate will use the same socket.
 */
int _lf_federate_sockets_for_inbound_physical_connections[NUMBER_OF_FEDERATES];

/**
 * An array that holds the socket descriptors for outbound physical
 * connections to each remote federate. The index will be the federate
 * ID of the remote receiving federate. This is initialized at startup
 * to -1 and is set to a socket ID by connect_to_federate()
 * when the socket is opened.
 * 
 * @note This federate will not open an outbound socket unless a physical
 * connection is specified in the Lingua Franca program where this federate 
 * acts as the source. Multiple outgoing physical connections to the same remote
 * federate will use the same socket.
 */
int _lf_federate_sockets_for_outbound_physical_connections[NUMBER_OF_FEDERATES];

/**
 * Thread ID for a thread that accepts sockets and then supervises
 * listening to those sockets for incoming P2P (physical) connections.
 */
pthread_t _lf_inbound_p2p_handling_thread_id;

/**
 * A socket descriptor for the socket server of the federate.
 * This is assigned in create_server().
 * This socket is used to listen to incoming physical connections from
 * remote federates. Once an incoming connection is accepted, the
 * opened socket will be stored in
 * _lf_federate_sockets_for_inbound_physical_connections.
 */
int _lf_server_socket;

/**
 * The port used for the server socket 
 * to listen for messages from other federates.
 * The federate informs the RTI of this port once
 * it has created its socket server by sending
 * an ADDRESS_AD message (@see rti.h).
 */
int _lf_server_port = -1;


/** 
 * Thread that listens for inputs from other federates.
 * This thread listens for P2P_MESSAGE_TIMED messages from the specified
 * peer federate and calls schedule to schedule an event.
 * If an error occurs or an EOF is received from the peer, then this
 * procedure returns, terminating the thread.
 * @param fed_id_ptr A pointer to a ushort containing federate ID being listened to.
 *  This procedure frees the memory pointed to before returning.
 */
void* listen_to_federates(void *args);

/** 
 * Create a server to listen to incoming physical
 * connections from remote federates. This function
 * only handles the creation of the server socket.
 * The reserved port for the server socket is then
 * sent to the RTI by sending an ADDRESS_AD message 
 * (@see rti.h). This function expects no response
 * from the RTI.
 * 
 * 
 * If a port is specified by the user, that will be used
 * as the only possibility for the server. This function
 * will fail if that port is not available. If a port is not
 * specified, the STARTING_PORT (@see rti.h) will be used.
 * The function will keep incrementing the port in this case 
 * until the number of tries reaches PORT_RANGE_LIMIT.
 * 
 * @note This function is similar to create_server(...) in rti.c.
 * However, it contains specific log messages for the peer to
 * peer connections between federates. It also additionally 
 * sends an address advertisement (ADDRESS_AD) message to the
 * RTI informing it of the port.
 * 
 * @param specified_port The specified port by the user.
 */
void create_server(int specified_port) {
    int port = specified_port;
    if (specified_port == 0) {
        // Use the default starting port.
        port = STARTING_PORT;
    }
    DEBUG_PRINT("Federate %d attempting to create a socket server on port %d.", _lf_my_fed_id, port);
    // Create an IPv4 socket for TCP (not UDP) communication over IP (0).
    int socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0) {
        error_print_and_exit("Federate %d failed to obtain a socket server.", _lf_my_fed_id);
    }

    // Server file descriptor.
    struct sockaddr_in server_fd;
    // Zero out the server address structure.
    bzero((char *) &server_fd, sizeof(server_fd));

    server_fd.sin_family = AF_INET;            // IPv4
    server_fd.sin_addr.s_addr = INADDR_ANY;    // All interfaces, 0.0.0.0.
    // Convert the port number from host byte order to network byte order.
    server_fd.sin_port = htons(port);

    int result = bind(
            socket_descriptor,
            (struct sockaddr *) &server_fd,
            sizeof(server_fd));
    // If the binding fails with this port and no particular port was specified
    // in the LF program, then try the next few ports in sequence.
    while (result != 0
            && specified_port == 0
            && port >= STARTING_PORT
            && port <= STARTING_PORT + PORT_RANGE_LIMIT) {
        printf("Federate %d failed to get port %d. Trying %d\n", _lf_my_fed_id, port, port + 1);
        port++;
        server_fd.sin_port = htons(port);
        result = bind(
                socket_descriptor,
                (struct sockaddr *) &server_fd,
                sizeof(server_fd));
    }
    if (result != 0) {
        if (specified_port == 0) {
            error_print_and_exit("ERROR on binding the socket for federate %d. Cannot find a usable port. Consider increasing PORT_RANGE_LIMIT in federate.c",
                                 _lf_my_fed_id);
        } else {
            error_print_and_exit("ERROR on binding socket for federate %d. Specified port is not available. Consider leaving the port unspecified",
                                 _lf_my_fed_id);
        }
    }
    printf("Server for federate %d started using port %d.\n", _lf_my_fed_id, port);

    // Enable listening for socket connections.
    // The second argument is the maximum number of queued socket requests,
    // which according to the Mac man page is limited to 128.
    listen(socket_descriptor, 128);

    // Set the global server port
    _lf_server_port = port;
    
    // Send the server port number to the RTI
    // on an ADDRESS_AD message (@see rti.h).
    unsigned char buffer[sizeof(int) + 1];
    buffer[0] = ADDRESS_AD;
    encode_int(_lf_server_port, &(buffer[1]));
    write_to_socket(_lf_rti_socket, sizeof(int) + 1, (unsigned char*)buffer,
                                        "Federate %d failed to send address advertisement.", _lf_my_fed_id);
    DEBUG_PRINT("Federate %d sent port %d to the RTI.\n", _lf_my_fed_id, _lf_server_port);

    // Set the global server socket
    _lf_server_socket = socket_descriptor;
}

/** 
 * Send the specified timestamped message to the specified port in the
 * specified federate via the RTI or directly to a federate depending on
 * the given socket. The port should be an input port of a reactor in 
 * the destination federate. This version does include the timestamp 
 * in the message. The caller can reuse or free the memory after this returns.
 * This method assumes that the caller does not hold the mutex lock,
 * which it acquires to perform the send.
 * 
 * @param socket The socket to send the message on
 * @param message_type The type of the message being sent. 
 *  Currently can be TIMED_MESSAGE for messages sent via
 *  RTI or P2P_TIMED_MESSAGE for messages sent between
 *  federates.
 * @param port The ID of the destination port.
 * @param federate The ID of the destination federate.
 * @param length The message length.
 * @param message The message.
 */
void send_message_timed(int socket, int message_type, unsigned int port, unsigned int federate, size_t length, unsigned char* message) {
    assert(port < 65536);
    assert(federate < 65536);
    unsigned char buffer[17];
    // First byte identifies this as a timed message.
    buffer[0] = message_type;
    // Next two bytes identify the destination port.
    // NOTE: Send messages little endian, not big endian.
    encode_ushort(port, &(buffer[1]));

    // Next two bytes identify the destination federate.
    encode_ushort(federate, &(buffer[3]));

    // The next four bytes are the message length.
    encode_int(length, &(buffer[5]));

    // Next 8 bytes are the timestamp.
    instant_t current_time = get_logical_time();

    encode_ll(current_time, &(buffer[9]));
    DEBUG_PRINT("Federate %d sending message with timestamp %lld to federate %d.", _lf_my_fed_id, current_time - start_time, federate);

    // Use a mutex lock to prevent multiple threads from simultaneously sending.
    DEBUG_PRINT("Federate %d pthread_mutex_lock send_timed", _lf_my_fed_id);
    pthread_mutex_lock(&mutex);
    DEBUG_PRINT("Federate %d pthread_mutex_locked", _lf_my_fed_id);
    write_to_socket(socket, 17, buffer, "Federate %d failed to send timed message header to the RTI.", _lf_my_fed_id);
    write_to_socket(socket, length, message, "Federate %d failed to send timed message body to the RTI.", _lf_my_fed_id);
    DEBUG_PRINT("Federate %d pthread_mutex_unlock", _lf_my_fed_id);
    pthread_mutex_unlock(&mutex);
}

/** Send a time to the RTI.
 *  This is not synchronized.
 *  It assumes the caller is.
 *  @param type The message type (NEXT_EVENT_TIME or LOGICAL_TIME_COMPLETE).
 *  @param time The time of this federate's next event.
 */
void send_time(unsigned char type, instant_t time) {
    DEBUG_PRINT("Sending time %lld to the RTI.", time);
    unsigned char buffer[9];
    buffer[0] = type;
    encode_ll(time, &(buffer[1]));
    write_to_socket(_lf_rti_socket, 9, buffer, "Federate %d failed to send time to the RTI.", _lf_my_fed_id);
}

/** Send a STOP message to the RTI, which will then broadcast
 *  the message to all federates.
 *  This function assumes the caller holds the mutex lock.
 */
void __broadcast_stop() {
    DEBUG_PRINT("Federate %d requesting a whole program stop.\n", _lf_my_fed_id);
    send_time(STOP, current_time);
}

/**
 * Thread to accept connections from other federates that send this federate
 * messages directly (not through the RTI). This thread starts a thread for
 * each accepted socket connection and then waits for all those threads to exit
 * before exiting itself.
 * @param ignored No argument needed for this thread.
 */
void* handle_p2p_connections_from_federates(void *ignored) {
    int received_federates = 0;
    pthread_t thread_ids[_lf_number_of_inbound_physical_connections];
    while (received_federates < _lf_number_of_inbound_physical_connections) {
        // Wait for an incoming connection request.
        struct sockaddr client_fd;
        uint32_t client_length = sizeof(client_fd);
        int socket_id = accept(_lf_server_socket, &client_fd, &client_length);
        // FIXME: Error handling here is too harsh maybe?
        if (socket_id < 0) {
            return NULL;
        }
        DEBUG_PRINT("Federate %d accepted new connection from remote federate.", _lf_my_fed_id);
        
        size_t header_length = 1 + sizeof(ushort) + 1;
        unsigned char buffer[header_length];
        int bytes_read = read_from_socket2(socket_id, header_length, (unsigned char*)&buffer);
        if(bytes_read != header_length || buffer[0] != P2P_SENDING_FED_ID) {
            printf("WARNING: Federate received invalid first message on P2P socket. Closing socket.\n");
            if (bytes_read >= 0) {
                unsigned char response[2];
                response[0] = REJECT;
                response[1] = WRONG_SERVER;
                // Ignore errors on this response.
                write_to_socket2(socket_id, 2, response);
                close(socket_id);
            }
            continue;
        }
        
        // Get the federation ID and check it.
        unsigned char federation_id_length = buffer[header_length - 1];
        char remote_federation_id[federation_id_length];
        bytes_read = read_from_socket2(socket_id, federation_id_length, (unsigned char*)remote_federation_id);
        if(bytes_read != federation_id_length
                || (strncmp(federation_id, remote_federation_id, strnlen(federation_id, 255)) != 0)) {
            printf("WARNING: Federate received invalid federation ID. Closing socket.\n");
            if (bytes_read >= 0) {
                unsigned char response[2];
                response[0] = REJECT;
                response[1] = FEDERATION_ID_DOES_NOT_MATCH;
                // Ignore errors on this response.
                write_to_socket2(socket_id, 2, response);
                close(socket_id);
            }
            continue;
        }

        // Extract the ID of the sending federate.
        ushort remote_fed_id = extract_ushort((unsigned char*)&(buffer[1]));
        DEBUG_PRINT("Federate %d received sending federate ID %d.", _lf_my_fed_id, remote_fed_id);
        _lf_federate_sockets_for_inbound_physical_connections[remote_fed_id] = socket_id;

        // Send an ACK message.
        unsigned char response[1];
        response[0] = ACK;
        write_to_socket(socket_id, 1, (unsigned char*)&response, "Federate %d failed to write ACK in response to federate %d.", _lf_my_fed_id, remote_fed_id);

        // Start a thread to listen for incoming messages from other federates.
        // We cannot pass a pointer to remote_fed_id to the thread we need to create
        // because that variable is on the stack. Instead, we malloc memory.
        // The created thread is responsible for calling free().
        ushort* remote_fed_id_copy = malloc(sizeof(ushort));
        if (remote_fed_id_copy == NULL) error_print("ERROR: malloc failed in federate %d.\n", _lf_my_fed_id);
        *remote_fed_id_copy = remote_fed_id;
        int result = pthread_create(&thread_ids[received_federates], NULL, listen_to_federates, remote_fed_id_copy);
        if (result != 0) {
            // Failed to create a listening thread.
            close(socket_id);
            error_print_and_exit(
                    "Federate %d failed to create a thread to listen for incoming physical connection. Error code: %d.",
                    _lf_my_fed_id, result
            );
        }

        received_federates++;
    }

    DEBUG_PRINT("All remote federates are connected to federate %d.", _lf_my_fed_id);

    void* thread_exit_status;
    for (int i = 0; i < _lf_number_of_inbound_physical_connections; i++) {
        pthread_join(thread_ids[i], &thread_exit_status);
        DEBUG_PRINT("Federate %d: thread listening for incoming P2P messages exited.", _lf_my_fed_id);
    }
    return NULL;
}

/**
 * Connect to the federate with the specified id. This established
 * connection will then be used in functions such as send_message_timed() 
 * to send messages directly to the specified federate. 
 * This function first sends an ADDRESS_QUERY message to the RTI to obtain 
 * the IP address and port number of the specified federate. It then attempts 
 * to establish a socket connection to the specified federate.
 * If this fails, the program exits. If it succeeds, it sets element [id] of 
 * the _lf_federate_sockets_for_outbound_physical_connections global array to 
 * refer to the socket for communicating directly with the federate.
 * @param id The ID of the remote federate.
 */
void connect_to_federate(ushort id) {
    int result = -1;
    int count_retries = 0;

    // Ask the RTI for port number of the remote federate.
    // The buffer is used for both sending and receiving replies.
    // The size is what is needed for receiving replies.
    unsigned char buffer[sizeof(int) + INET_ADDRSTRLEN];
    int port = -1;
    struct in_addr host_ip_addr;
    int count_tries = 0;
    while (port == -1) {
        buffer[0] = ADDRESS_QUERY;
        // NOTE: Sending messages in little endian.
        encode_ushort(id, &(buffer[1]));

        // debug_print("Sending address query for federate %d.\n", id);

        write_to_socket(_lf_rti_socket, sizeof(ushort) + 1, buffer, "Federate %d failed to send address query for federate %d to RTI.", _lf_my_fed_id, id);

        // Read RTI's response.
        read_from_socket(_lf_rti_socket, sizeof(int), buffer, "Federate %d failed to read the requested port number for federate %d from RTI.", _lf_my_fed_id, id);

        port = extract_int(buffer);

        read_from_socket(_lf_rti_socket, sizeof(host_ip_addr), (unsigned char *)&host_ip_addr, "Federate %d failed to read the ip address for federate %d from RTI.", _lf_my_fed_id, id);

        // A reply of -1 for the port means that the RTI does not know
        // the port number of the remote federate, presumably because the
        // remote federate has not yet sent an ADDRESS_AD message to the RTI.
        // Sleep for some time before retrying.
        if (port == -1) {
            if (count_tries++ >= CONNECT_NUM_RETRIES) {
                fprintf(stderr, "TIMEOUT on federate %d obtaining IP/port for federate %d from the RTI.\n", _lf_my_fed_id, id);
                exit(1);
            }
            struct timespec wait_time = {0L, ADDRESS_QUERY_RETRY_INTERVAL};
            struct timespec remaining_time;
            if (nanosleep(&wait_time, &remaining_time) != 0) {
                // Sleep was interrupted.
                continue;
            }
        }
    }
    assert(port < 65536);
    
    if(DEBUG) {
        // Print the received IP address in a human readable format
        // Create the human readable format of the received address.
        char hostname[INET_ADDRSTRLEN];
        inet_ntop( AF_INET, &host_ip_addr , hostname, INET_ADDRSTRLEN );
        DEBUG_PRINT("Received address %s port %d for federate %d from RTI.", hostname, port, id);
    }

    while (result < 0) {
        // Create an IPv4 socket for TCP (not UDP) communication over IP (0).
        _lf_federate_sockets_for_outbound_physical_connections[id] = socket(AF_INET , SOCK_STREAM , 0);
        if (_lf_federate_sockets_for_outbound_physical_connections[id] < 0) {
            error_print_and_exit("ERROR on federate %d creating socket to federate %d.", _lf_my_fed_id, id);
        }

        // Server file descriptor.
        struct sockaddr_in server_fd;
        // Zero out the server_fd struct.
        bzero((char *) &server_fd, sizeof(server_fd));

        // Set up the server_fd fields.
        server_fd.sin_family = AF_INET;    // IPv4
        server_fd.sin_addr = host_ip_addr; // Received from the RTI

        // Convert the port number from host byte order to network byte order.
        server_fd.sin_port = htons(port);
        result = connect(
            _lf_federate_sockets_for_outbound_physical_connections[id],
            (struct sockaddr *)&server_fd,
            sizeof(server_fd));
        
        if (result != 0) {
            error_print("Federate %d failed to connect to federate %d on port %d.", _lf_my_fed_id, id, port);

            // Try again after some time if the connection failed.
            // Note that this should not really happen since the remote federate should be
            // accepting socket connections. But possibly it will be busy (in process of accepting
            // another socket connection?). Hence, we retry.
            count_retries++;
            if (count_retries > CONNECT_NUM_RETRIES) {
                // If the remote federate is not accepting the connection after CONNECT_NUM_RETRIES
                // treat it as a soft error condition and return.
                error_print("Federate %d failed to connect to federate %d after %d retries. Giving up.",
                            _lf_my_fed_id, id, CONNECT_NUM_RETRIES);
                return;
            }
            printf("Federate %d could not connect to federate %d. Will try again every %d nanoseconds.\n",
                    _lf_my_fed_id, id, ADDRESS_QUERY_RETRY_INTERVAL);
            // Wait CONNECT_RETRY_INTERVAL seconds.
            struct timespec wait_time = {0L, ADDRESS_QUERY_RETRY_INTERVAL};
            struct timespec remaining_time;
            if (nanosleep(&wait_time, &remaining_time) != 0) {
                // Sleep was interrupted.
                continue;
            }
        } else {
            size_t buffer_length = 1 + sizeof(ushort) + 1;
            unsigned char buffer[buffer_length];
            buffer[0] = P2P_SENDING_FED_ID;
            encode_ushort(_lf_my_fed_id, (unsigned char *)&(buffer[1]));
            unsigned char federation_id_length = strnlen(federation_id, 255);
            buffer[sizeof(ushort) + 1] = federation_id_length;
            write_to_socket(_lf_federate_sockets_for_outbound_physical_connections[id], buffer_length, buffer,
                            "Federate %d failed to send fed_id to federate %d.", _lf_my_fed_id, id);
            write_to_socket(_lf_federate_sockets_for_outbound_physical_connections[id], federation_id_length, (unsigned char *)federation_id,
                            "Federate %d failed to send federation id to federate %d.", _lf_my_fed_id, id);

            read_from_socket(_lf_federate_sockets_for_outbound_physical_connections[id], 1, (unsigned char *)buffer,
                            "Federate %d failed to read ACK from federate %d in response to sending fed_id.", _lf_my_fed_id, id);
            if (buffer[0] != ACK) {
                // Get the error code.
                read_from_socket(_lf_federate_sockets_for_outbound_physical_connections[id], 1, (unsigned char *)buffer,
                                "Federate %d failed to read error code from federate %d in response to sending fed_id.", _lf_my_fed_id, id);
                error_print("Received REJECT message from remote federate (%d).", buffer[0]);
                result = -1;
                continue;
            } else {
                printf("Federate %d: connected to federate %d, port %d.\n", _lf_my_fed_id, id, port);
            }
        }
    }
}

/**
 * Connect to the RTI at the specified host and port and return
 * the socket descriptor for the connection. If this fails, the
 *  program exits. If it succeeds, it sets the rti_socket global
 *  variable to refer to the socket for communicating with the RTI.
 *  @param id The assigned ID of the federate.
 *  @param hostname A hostname, such as "localhost".
 *  @param port A port number.
 */
void connect_to_rti(ushort id, char* hostname, int port) {
    // Repeatedly try to connect, one attempt every 2 seconds, until
    // either the program is killed, the sleep is interrupted,
    // or the connection succeeds.
    // If the specified port is 0, set it instead to the start of the
    // port range.
    bool specific_port_given = true;
    if (port == 0) {
        port = STARTING_PORT;
        specific_port_given = false;
    }
    int result = -1;
    int count_retries = 0;

    bool failure_message = false;
    while (result < 0) {
        // Create an IPv4 socket for TCP (not UDP) communication over IP (0).
        _lf_rti_socket = socket(AF_INET , SOCK_STREAM , 0);
        if (_lf_rti_socket < 0) {
            error_print_and_exit("Federate %d creating socket to RTI.", _lf_my_fed_id);
        }

        struct hostent *server = gethostbyname(hostname);
        if (server == NULL) {
            error_print_and_exit("ERROR, no such host for RTI: %s\n", hostname);
        }
        // Server file descriptor.
        struct sockaddr_in server_fd;
        // Zero out the server_fd struct.
        bzero((char *) &server_fd, sizeof(server_fd));

        // Set up the server_fd fields.
        server_fd.sin_family = AF_INET;    // IPv4
        bcopy((char *)server->h_addr,
             (char *)&server_fd.sin_addr.s_addr,
             server->h_length);
        // Convert the port number from host byte order to network byte order.
        server_fd.sin_port = htons(port);
        result = connect(
            _lf_rti_socket,
            (struct sockaddr *)&server_fd,
            sizeof(server_fd));
        // If this failed, try more ports, unless a specific port was given.
        if (result != 0
                && !specific_port_given
                && port >= STARTING_PORT
                && port <= STARTING_PORT + PORT_RANGE_LIMIT
        ) {
            if (!failure_message) {
                printf("Federate %d failed to connect to RTI on port %d. Trying %d", _lf_my_fed_id, port, port + 1);
                failure_message = true;
            } else {
                printf(", %d", port);
            }
            port++;
            continue;
        }
        if (failure_message) {
            printf("\n");
            failure_message = false;
        }
        // If this still failed, try again with the original port after some time.
        if (result < 0) {
            if (!specific_port_given && port == STARTING_PORT + PORT_RANGE_LIMIT + 1) {
                port = STARTING_PORT;
            }
            count_retries++;
            if (count_retries > CONNECT_NUM_RETRIES) {
                error_print_and_exit("Federate %d failed to connect to the RTI after %d retries. Giving up.\n",
                                     _lf_my_fed_id, CONNECT_NUM_RETRIES);
            }
            printf("Federate %d could not connect to RTI at %s. Will try again every %d seconds.\n",
                    _lf_my_fed_id, hostname, CONNECT_RETRY_INTERVAL);
            // Wait CONNECT_RETRY_INTERVAL seconds.
            struct timespec wait_time = {(time_t)CONNECT_RETRY_INTERVAL, 0L};
            struct timespec remaining_time;
            if (nanosleep(&wait_time, &remaining_time) != 0) {
                // Sleep was interrupted.
                continue;
            }
        } else {
            // Have connected to an RTI, but not sure it's the right RTI.
            // Send a FED_ID message and wait for a reply.
            // Notify the RTI of the ID of this federate and its federation.
            unsigned char buffer[4];

            // Send the message type first.
            buffer[0] = FED_ID;
            // Next send the federate ID.
            encode_ushort(id, &buffer[1]);
            // Next send the federation ID length.
            // The federation ID is limited to 255 bytes.
            size_t federation_id_length = strnlen(federation_id, 255);
            buffer[3] = federation_id_length & 0xff;

            // FIXME: Retry rather than exit.
            write_to_socket(_lf_rti_socket, 4, buffer, "Federate %d failed to send federate ID to RTI.", _lf_my_fed_id);

            // Next send the federation ID itself.
            // FIXME: Retry rather than exit.
            write_to_socket(_lf_rti_socket, federation_id_length, (unsigned char*)federation_id,
                            "Federate %d failed to send federation ID to RTI.", _lf_my_fed_id);

            // Wait for a response.
            unsigned char response;
            read_from_socket(_lf_rti_socket, 1, &response, "Federate %d failed to read response from RTI.", _lf_my_fed_id);
            if (response == REJECT) {
                // Read one more byte to determine the cause of rejection.
                unsigned char cause;
                read_from_socket(_lf_rti_socket, 1, &cause, "Federate %d failed to read the cause of rejection by the RTI.", _lf_my_fed_id);
                if (cause == FEDERATION_ID_DOES_NOT_MATCH || cause == WRONG_SERVER) {
                    printf("Federate %d connected to the wrong RTI on port %d. Trying %d.\n", _lf_my_fed_id, port, port + 1);
                    port++;
                    result = -1;
                    continue;
                }
                error_print_and_exit("RTI Rejected FED_ID message with response (see rti.h): %d. Error code: %d. Federate quits.\n", response, cause);
            }
            printf("Federate %d: connected to RTI at %s:%d.\n", _lf_my_fed_id, hostname, port);

        }
    }
}

/** Send the specified timestamp to the RTI and wait for a response.
 *  The specified timestamp should be current physical time of the
 *  federate, and the response will be the designated start time for
 *  the federate. This proceedure blocks until the response is
 *  received from the RTI.
 *  @param my_physical_time The physical time at this federate.
 *  @return The designated start time for the federate.
 */
instant_t get_start_time_from_rti(instant_t my_physical_time) {
    // Send the timestamp marker first.
    unsigned char message_marker = TIMESTAMP;
    // FIXME: Retry rather than exit.
    write_to_socket(_lf_rti_socket, 1, &message_marker, 
                    "Federate %d failed to send TIMESTAMP message ID to RTI.", _lf_my_fed_id);

    // Send the timestamp.
    long long message = swap_bytes_if_big_endian_ll(my_physical_time);

    write_to_socket(_lf_rti_socket, sizeof(long long), (void*)(&message),
                    "Federate %d failed to send TIMESTAMP message to RTI.", _lf_my_fed_id);

    // Get a reply.
    // Buffer for message ID plus timestamp.
    int buffer_length = sizeof(long long) + 1;
    unsigned char buffer[buffer_length];

    // Read bytes from the socket. We need 9 bytes.
    read_from_socket(_lf_rti_socket, 9, &(buffer[0]), "Federate %d failed to read TIMESTAMP message from RTI.", _lf_my_fed_id);
    DEBUG_PRINT("Federate %d read 9 bytes.", _lf_my_fed_id);

    // First byte received is the message ID.
    if (buffer[0] != TIMESTAMP) {
        error_print_and_exit("ERROR: Federate expected a TIMESTAMP message from the RTI. Got %u (see rti.h).\n",
                             buffer[0]);
    }

    instant_t timestamp = swap_bytes_if_big_endian_ll(*((long long*)(&(buffer[1]))));
    printf("Federate %d: starting timestamp is: %lld.\n", _lf_my_fed_id, timestamp);

    return timestamp;
}

/**
 * Placeholder for a generated function that returns a pointer to the
 * trigger_t struct for the action corresponding to the specified port ID.
 * @param port_id The port ID.
 * @return A pointer to a trigger_t struct or null if the ID is out of range.
 */
trigger_t* __action_for_port(int port_id);


/**
 * Version of schedule_value() identical to that in reactor_common.c
 * except that it does not acquire the mutex lock.
 * @param action The action or timer to be triggered.
 * @param extra_delay Extra offset of the event release.
 * @param value Dynamically allocated memory containing the value to send.
 * @param length The length of the array, if it is an array, or 1 for a
 *  scalar and 0 for no payload.
 * @return A handle to the event, or 0 if no event was scheduled, or -1 for error.
 */
handle_t schedule_value_already_locked(
    trigger_t* trigger, interval_t extra_delay, void* value, int length) {
    token_t* token = create_token(trigger->element_size);
    token->value = value;
    token->length = length;
    int return_value = __schedule(trigger, extra_delay, token);
    // Notify the main thread in case it is waiting for physical time to elapse.
    DEBUG_PRINT("Federate %d pthread_cond_broadcast(&event_q_changed).", _lf_my_fed_id);
    pthread_cond_broadcast(&event_q_changed);
    return return_value;
}

/**
 * Handle a timestamped message being received from a remote federate via the RTI
 * or directly from other federates.
 * This will read the timestamp, which is appended to the header,
 * and calculate an offset to pass to the schedule function.
 * This function assumes the caller does not hold the mutex lock,
 * which it acquires to call schedule.
 * @param socket The socket to read the message from.
 * @param buffer The buffer to read.
 */
void handle_timed_message(int socket, unsigned char* buffer) {
    // Read the header.
    read_from_socket(socket, 16, buffer, "Federate %d failed to read timed message header.", _lf_my_fed_id);
    // Extract the header information.
    unsigned short port_id;
    unsigned short federate_id;
    unsigned int length;
    extract_header(buffer, &port_id, &federate_id, &length);
    // Check if the message is intended for this federate
    assert (_lf_my_fed_id == federate_id);
    DEBUG_PRINT("Federate receiving message to port %d to federate %d of length %d.", port_id, federate_id, length);

    // Read the timestamp.
    instant_t timestamp = extract_ll(buffer + 8);
    DEBUG_PRINT("Message timestamp: %lld.", timestamp - start_time);

    // Read the payload.
    // Allocate memory for the message contents.
    unsigned char* message_contents = (unsigned char*)malloc(length);
    read_from_socket(socket, length, message_contents, "Federate %d failed to read timed message body.", _lf_my_fed_id);
    DEBUG_PRINT("Message received by federate: %s.", message_contents);

    // Acquire the one mutex lock to prevent logical time from advancing
    // between the time we get logical time and the time we call schedule().
    DEBUG_PRINT("Federate %d pthread_mutex_lock handle_timed_message.", _lf_my_fed_id);
    pthread_mutex_lock(&mutex);
    DEBUG_PRINT("Federate %d pthread_mutex_locked.", _lf_my_fed_id);

    interval_t delay = timestamp - get_logical_time();
    // NOTE: Cannot call schedule_value(), which is what we really want to call,
    // because pthreads is too incredibly stupid and deadlocks trying to acquire
    // a lock that the calling thread already holds.
    schedule_value_already_locked(__action_for_port(port_id), delay, message_contents, length);
    DEBUG_PRINT("Called schedule with delay %lld.", delay);

    DEBUG_PRINT("Federate %d pthread_mutex_unlock.", _lf_my_fed_id);
    pthread_mutex_unlock(&mutex);
}

/** Most recent TIME_ADVANCE_GRANT received from the RTI, or NEVER if none
 *  has been received.
 *  This is used to communicate between the listen_to_rti thread and the
 *  main federate thread.
 */
volatile instant_t __tag = NEVER;

/** Indicator of whether a NET has been sent to the RTI and no TAG
 *  yet received in reply.
 */
volatile bool __tag_pending = false;

/** Handle a time advance grant (TAG) message from the RTI.
 *  This function assumes the caller does not hold the mutex lock,
 *  which it acquires to interact with the main thread, which may
 *  be waiting for a TAG (this broadcasts a condition signal).
 */
void handle_time_advance_grant() {
    union {
        long long ull;
        unsigned char c[sizeof(long long)];
    } result;
    read_from_socket(_lf_rti_socket, sizeof(long long), (unsigned char*)&result.c,
                     "Federate %d failed to read the time advance grant from the RTI.", _lf_my_fed_id);

    DEBUG_PRINT("Federate %d pthread_mutex_lock handle_time_advance_grant.", _lf_my_fed_id);
    pthread_mutex_lock(&mutex);
    DEBUG_PRINT("Federate %d pthread_mutex_locked\n", _lf_my_fed_id);
    __tag = swap_bytes_if_big_endian_ll(result.ull);
    __tag_pending = false;
    DEBUG_PRINT("Federate %d received TAG %lld.", _lf_my_fed_id, __tag - start_time);
    // Notify everything that is blocked.
    pthread_cond_broadcast(&event_q_changed);
    DEBUG_PRINT("Federate %d pthread_mutex_unlock.", _lf_my_fed_id);
    pthread_mutex_unlock(&mutex);
}

/** Handle a STOP message from the RTI.
 *  NOTE: The stop time is ignored. This federate will stop as soon
 *  as possible.
 *  FIXME: It should be possible to at least handle the situation
 *  where the specified stop time is larger than current time.
 *  This would require implementing a shutdown action.
 *  @param buffer A pointer to the bytes specifying the stop time.
 */
void handle_incoming_stop_message() {
    union {
        long long ull;
        unsigned char c[sizeof(long long)];
    } time;
    read_from_socket(_lf_rti_socket, sizeof(long long), (unsigned char*)&time.c, "Federate %d failed to read stop time from RTI.", _lf_my_fed_id);

    // Acquire a mutex lock to ensure that this state does change while a
    // message is transport or being used to determine a TAG.
    pthread_mutex_lock(&mutex);

    instant_t stop_time = swap_bytes_if_big_endian_ll(time.ull);
    DEBUG_PRINT("Federate %d received from RTI a STOP request with time %lld.", FED_ID, stop_time - start_time);
    stop_requested = true;
    pthread_cond_broadcast(&event_q_changed);

    pthread_mutex_unlock(&mutex);
}

/** 
 * Thread that listens for inputs from other federates.
 * This thread listens for P2P_MESSAGE_TIMED messages from the specified
 * peer federate and calls schedule to schedule an event.
 * If an error occurs or an EOF is received from the peer, then this
 * procedure returns, terminating the thread.
 * @param fed_id_ptr A pointer to a ushort containing federate ID being listened to.
 *  This procedure frees the memory pointed to before returning.
 */
void* listen_to_federates(void *fed_id_ptr) {

    ushort fed_id = *((ushort*)fed_id_ptr);

    DEBUG_PRINT("Federate %d listening to federate %d.", _lf_my_fed_id, fed_id);

    int socket_id = _lf_federate_sockets_for_inbound_physical_connections[fed_id];

    // Buffer for incoming messages.
    // This does not constrain the message size
    // because the message will be put into malloc'd memory.
    unsigned char buffer[BUFFER_SIZE];

    // Listen for messages from the federate.
    while (1) {
        // Read one byte to get the message type.
        DEBUG_PRINT("Federate %d waiting for a P2P message.", _lf_my_fed_id);
        int bytes_read = read_from_socket2(socket_id, 1, buffer);
        DEBUG_PRINT("Federate %d received a P2P message of type %d.", _lf_my_fed_id, buffer[0]);
        if (bytes_read == 0) {
            // EOF occurred. This breaks the connection.
            DEBUG_PRINT("Federate %d received EOF from peer federate %d. Closing the socket.", _lf_my_fed_id, fed_id);
            close(socket_id);
            _lf_federate_sockets_for_inbound_physical_connections[fed_id] = -1;
            break;
        } else if (bytes_read < 0) {
            error_print("P2P socket between federate %d and %d broken.", _lf_my_fed_id, fed_id);
            close(socket_id);
            _lf_federate_sockets_for_inbound_physical_connections[fed_id] = -1;
            break;
        }
        switch(buffer[0]) {
        case P2P_TIMED_MESSAGE:
            DEBUG_PRINT("Federate %d handling timed p2p message from federate %d.", _lf_my_fed_id, fed_id);
            handle_timed_message(socket_id, buffer + 1);
            break;
        default:
            error_print("Federate %d received erroneous message type: %d. Closing the socket.", _lf_my_fed_id, buffer[0]);
            close(socket_id);
            _lf_federate_sockets_for_inbound_physical_connections[fed_id] = -1;
            break;
        }
    }
    free(fed_id_ptr);
    return NULL;
}

/** Thread that listens for inputs from the RTI.
 *  When a physical message arrives, this calls schedule.
 */
void* listen_to_rti(void* args) {
    // Buffer for incoming messages.
    // This does not constrain the message size
    // because the message will be put into malloc'd memory.
    unsigned char buffer[BUFFER_SIZE];

    // Listen for messages from the federate.
    while (1) {
        // Read one byte to get the message type.
        read_from_socket(_lf_rti_socket, 1, buffer, "Federate %d failed to read message header coming from RTI.", _lf_my_fed_id);
        switch(buffer[0]) {
        case TIMED_MESSAGE:
            handle_timed_message(_lf_rti_socket, buffer + 1);
            break;
        case TIME_ADVANCE_GRANT:
            handle_time_advance_grant();
            break;
        case STOP:
            handle_incoming_stop_message();
            break;
        default:
            error_print_and_exit("Received from RTI an unrecognized message type: %d.", buffer[0]);
        }
    }
    return NULL;
}

/** Synchronize the start with other federates via the RTI.
 *  This initiates a connection with the RTI, then
 *  sends the current logical time to the RTI and waits for the
 *  RTI to respond with a specified time.
 *  It starts a thread to listen for messages from the RTI.
 *  It then waits for physical time to match the specified time,
 *  sets current logical time to the time returned by the RTI,
 *  and then returns. If --fast was specified, then this does
 *  not wait for physical time to match the logical start time
 *  returned by the RTI.
 */
void synchronize_with_other_federates() {
                
    DEBUG_PRINT("Federate %d synchronizing with other federates.", _lf_my_fed_id);

    // Reset the start time to the coordinated start time for all federates.
    current_time = get_start_time_from_rti(get_physical_time());

    start_time = current_time;

    if (duration >= 0LL) {
        // A duration has been specified. Recalculate the stop time.
        stop_time = current_time + duration;
    }

    // Start a thread to listen for incoming messages from the RTI.
    pthread_t thread_id;
    pthread_create(&thread_id, NULL, listen_to_rti, NULL);

    // If --fast was not specified, wait until physical time matches
    // or exceeds the start time.
    wait_until(current_time);
    DEBUG_PRINT("Done waiting for start time %lld.", current_time);
    DEBUG_PRINT("Physical time is ahead of current time by %lld.", get_physical_time() - current_time);

    // Reinitialize the physical start time to match the current physical time.
    // This will be different on each federate. If --fast was given, it could
    // be very different.
    physical_start_time = get_physical_time();
}

/** Indicator of whether this federate has upstream federates.
 *  The default value of false may be overridden in __initialize_trigger_objects.
 */
bool __fed_has_upstream = false;

/** Indicator of whether this federate has downstream federates.
 *  The default value of false may be overridden in __initialize_trigger_objects.
 */
bool __fed_has_downstream = false;

/** Send a logical time complete (LTC) message to the RTI
 *  if there are downstream federates. Otherwise, do nothing.
 *  This function assumes the caller holds the mutex lock.
 */
void __logical_time_complete(instant_t time) {
    if (__fed_has_downstream) {
        DEBUG_PRINT("Federate %d is handling the completion of logical time %lld.", _lf_my_fed_id, time);
        send_time(LOGICAL_TIME_COMPLETE, time);
    }
}

/** If this federate depends on upstream federates or sends data to downstream
 *  federates, then notify the RTI of the next event on the event queue.
 *  If there are upstream federates, then this will block until either the
 *  RTI grants the advance to the requested time or the wait for the response
 *  from the RTI is interrupted by a change in the event queue (e.g., a
 *  physical action triggered).  This returns either the specified time or
 *  a lesser time when it is safe to advance logical time to the returned time.
 *  The returned time may be less than the specified time if there are upstream
 *  federates and either the RTI responds with a lesser time or
 *  the wait for a response from the RTI is interrupted by a
 *  change in the event queue.
 *  This function assumes the caller holds the mutex lock.
 */
 instant_t __next_event_time(instant_t time) {
     if (!__fed_has_downstream && !__fed_has_upstream) {
         // This federate is not connected (except possibly by physical links)
         // so there is no need for the RTI to get involved.

         // FIXME: If the event queue is empty, then the time argument is either
         // the stop_time or FOREVER. In this case, it matters whether there are
         // upstream federates connected by physical connections, which do not
         // affect __fed_has_upstream. We should not return immediately because
         // then the execution will hit its stop_time and fail to receive any
         // messages sent by upstream federates.
         return time;
     }

     // FIXME: The returned value t is a promise that, absent inputs from
     // another federate, this federate will not produce events earlier than t.
     // But if there are downstream federates and there is
     // a physical action (not counting receivers from upstream federates),
     // then we can only promise up to current physical time.
     // This will result in this federate busy waiting, looping through this code
     // and notifying the RTI with next_event_time(current_physical_time())
     // repeatedly.

     // If there are upstream federates, then we need to wait for a
     // reply from the RTI.

     // If time advance has already been granted for this time or a larger
     // time, then return immediately.
     if (__tag >= time) {
         return time;
     }

     send_time(NEXT_EVENT_TIME, time);
     DEBUG_PRINT("Federate %d sent next event time %lld to RTI.", _lf_my_fed_id, time - start_time);

     // If there are no upstream federates, return immediately, without
     // waiting for a reply. This federate does not need to wait for
     // any other federate.
     // FIXME: If fast execution is being used, it may be necessary to
     // throttle upstream federates.
     // FIXME: As noted above, this is not correct if the time is the stop_time.
     if (!__fed_has_upstream) {
         return time;
     }

     __tag_pending = true;
     while (__tag_pending) {
         // Wait until either something changes on the event queue or
         // the RTI has responded with a TAG.
         DEBUG_PRINT("Federate %d pthread_cond_wait", _lf_my_fed_id);
         if (pthread_cond_wait(&event_q_changed, &mutex) != 0) {
             fprintf(stderr, "ERROR: pthread_cond_wait errored.\n");
         }
         DEBUG_PRINT("Federate %d pthread_cond_wait returned", _lf_my_fed_id);

         if (__tag_pending) {
             // The RTI has not replied, so the wait must have been
             // interrupted by activity on the event queue.
             // If there is now an earlier event on the event queue,
             // then we should return with the time of that event.
             event_t* head_event = (event_t*)pqueue_peek(event_q);
             if (head_event != NULL && head_event->time < time) {
                 return head_event->time;
             }
             // If we get here, any activity on the event queue is not relevant.
             // Either the queue is empty or whatever appeared on it
             // has a timestamp greater than this request.
             // Keep waiting for the TAG.
         }
     }
     return __tag;
}
