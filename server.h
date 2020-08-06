#ifndef _SERVER_H_
#define _SERVER_H_
#include "performance-parameters.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
// for inet_pton, and the like
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <netdb.h>
#include <dirent.h>
#include <math.h>



#define CHECK_BIT(var,pos) ((var) & (0x80>>(pos)))
#define MIN(a, b) (a < b ? a : b)
#define MAXLINE (4096)
#define SA struct sockaddr
#define ERROR (-1)


struct config {
    uint32_t IP_addr;
    uint16_t server_port;
}__attribute__((packed));

struct message {
    uint8_t first;
    uint64_t p_len;
}__attribute__((packed));

struct message_header {
    uint8_t type;
};
struct compressed {
    uint8_t length;
    uint8_t bits[32];
    int complete_byte;
    int additional_bits;
};

struct tree_node {
    uint8_t bytes;
    struct tree_node* left;
    struct tree_node* right;
    int is_external;
};

struct tree {
    struct tree_node* root;
};


struct global {
    int end;
    pthread_mutex_t mutex_map;
    pthread_cond_t cond;
    int server_socket;
    struct compressed* code;
    struct tree* tree;
    char* directory;
    struct retrieve_info** requests;
    int num_of_ID;
};

struct argument {
    struct global* global;
    int client_socket;
};


struct retrieve_info {
    uint32_t session_id;
    uint64_t offset;
    uint64_t retrieve_length;
    char* file_name;
    int closed;
    int retrieving_finished;
    int request_num;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t* file_content;
    uint64_t file_content_len;
    int finished_num;
    int compressed;
    int sent;
};

int check(int exp, const char *msg);
struct global* global_create(int server_socket);
void global_destroy(struct global* global);
void free_nodes(struct tree_node* root);
char *bin2hex(const unsigned char *input, size_t len);
void* handle_connection(void* arg);
uint64_t swap_uint64( uint64_t val );
uint8_t *directory_listing(uint64_t* payload_length, char* directory);
struct compressed* read_compression();
struct tree* generate_huffman_tree(struct compressed* code);
uint8_t *file_size_query(char* directory, char* file_name);
uint8_t *compress(const uint8_t* message, size_t len,
        struct compressed* code, uint64_t* payload_length);
uint8_t* decompress(const uint8_t* sequences, struct tree* tree,
        uint64_t length, uint64_t* decompressed_length);
uint8_t* retrieve_file(struct retrieve_info info, char* directory);

#endif