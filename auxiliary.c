#include "server.h"

int check(int exp, const char *msg) {
    if (exp == ERROR) {
        perror(msg);
        exit(1);
    }
    return exp;
}

/* Thread Pool*/
struct global* global_create(int server_socket) {
    struct global* global = malloc(sizeof(struct global));
    global->end = 0;
    pthread_mutex_init(&global->mutex_map, NULL);
    pthread_cond_init(&global->cond, NULL);
    global->code = read_compression();
    global->tree = generate_huffman_tree(global->code);
    global->num_of_ID = 0;
    return global;
}

void global_destroy(struct global* global) {
    pthread_mutex_destroy(&global->mutex_map);
    pthread_cond_destroy(&global->cond);
    for (int i = 0; i < global->num_of_ID; i++) {
        free(global->requests[i]->file_name);
        free(global->requests[i]);
    }
    if (global->num_of_ID > 0) {
        free(global->requests);
    }
    free(global->code);
    free(global->directory);
    free_nodes(global->tree->root);
    free(global->tree);
    free(global);
}

/* compression related */

// free the binary tree (huffman)
void free_nodes(struct tree_node* root) {
    if (root == NULL)
        return;
    free_nodes(root->left);
    free_nodes(root->right);
    free(root);

}

// read the compression dictionary -> return a array of compressed bits
struct compressed* read_compression() {
    FILE* fpt = fopen("compression.dict", "rb");
    if (fpt == NULL)
        perror("open failed");
    uint8_t length = 0;
    int i = 0;
    uint8_t buf[32];

    fseek(fpt, 0L, SEEK_END);
    size_t sz = ftell(fpt);
    fseek(fpt, 0L, SEEK_SET);
    char* all = malloc(sz);
    fread(all, sz, 1, fpt);
    size_t already_read = 0;
    unsigned int read_byte;
    fseek(fpt, 0L, SEEK_SET);
    unsigned int nbits = 8;
    uint8_t bits[35];
    memset(bits, 0, 35);
    struct compressed* code = malloc(sizeof(struct compressed) * 256);

    memset(code, 0, 256 * sizeof(struct compressed));

    while(i < 256) {
        // read the length
        uint8_t next_byte;
        fread(&next_byte, 1, 1, fpt);
        already_read++;
        length = length | (next_byte >> (8u - nbits));
        code[i].length = length;

        // initialise
        memset(&buf, 0, 32);

        // special case
        if (length + nbits < 8) {

            uint8_t temp = (next_byte << nbits)
                            >> nbits >> (8 - (nbits + length))
                            << (8 - (nbits + length)) << nbits;
            memcpy(bits, &temp, 1);  // set the first byte (completed)
            memcpy(code[i].bits, bits, 32);

            uint8_t temp_length = length;
            length = next_byte << (nbits + length);
            nbits = nbits + temp_length;

            memset(bits, 0, 32);
            i++;
            continue;
        }

        uint8_t temp = next_byte << nbits;
        memcpy(bits, &temp, 1);
        // set the first byte (not completed) -> only want first (8-nbits)


        // the num of bytes of bits -> floor
        read_byte = (int) floor(((double)(length - (8 - nbits))) / 8);

        // read read_byte bytes

        fread(&buf, read_byte, 1, fpt);
        already_read += read_byte;

        // copy the current binary bits
        memcpy(bits + 1, &buf, 31);

        int z = 0;
        uint8_t first = bits[z];
        uint8_t second = bits[z + 1];
        first = first >> nbits << nbits;
        second = second >> (8 - nbits);
        bits[z] = first | second;
        z++;

        // move each byte to have no padding between
        for (; z < 7; z++) {
            first = bits[z];
            second = bits[z + 1];
            first = first << nbits;
            second = second >> (8 - nbits);
            bits[z] = first | second;
        }


        // now we have first (valid_bits) bits
        unsigned int valid_bits = read_byte * 8 + 8 - nbits;
        // (num_valid_bytes) bytes are completed
        int num_valid_bytes = (int) floor(((double)(valid_bits)) / 8);
        // add that one more byte attached to these bits
        int mod_bits = valid_bits % 8;
        bits[num_valid_bytes] = bits[num_valid_bytes] >>
                (8 - mod_bits) << (8 - mod_bits);


        // one more byte to read!
        nbits = (length - (8 - nbits)) % 8;
        if (nbits != 0) {
            uint8_t byte_buf;
            fread(&length, 1, 1, fpt);
            already_read += 1;
            // we only want first nbits!
            byte_buf = (length >> (8u - nbits)) << (8u - nbits);
            bits[num_valid_bytes] =  bits[num_valid_bytes]
                    | (byte_buf >> mod_bits);
            bits[num_valid_bytes + 1] = byte_buf << (8 - mod_bits);
            memcpy(code[i].bits, bits, 32);
            length = (length << nbits); // len = 8 - nbits
        } else {
            memcpy(code[i].bits, bits, 32);

            nbits = 8;
            length = 0;
        }
        memset(bits, 0, 32);
        i++;

    }
    free(all);

    return code;
}

// Generate a binary tree based on compressed bits
// in order to decompress later
struct tree* generate_huffman_tree(struct compressed* code) {
    struct tree* tree = malloc(sizeof(struct tree));
    tree->root = malloc(sizeof(struct tree_node));
    tree->root->is_external = 0;
    tree->root->right = NULL;
    tree->root->left = NULL;
    struct tree_node* new_node;
    for (int i = 0; i < 256; i++) {
        int complete_byte = (int) floor(((double)(code[i].length)) / 8);
        int additional_bit = code[i].length % 8;
        code[i].additional_bits = additional_bit;
        code[i].complete_byte = complete_byte;
        int b = 0;
        struct tree_node* current = tree->root;
        for ( ; b < complete_byte; b++) {
            for (int c = 0; c < 8; c++) {
                if (CHECK_BIT(code[i].bits[b], c)) {
                    // is set -> right
                    //printf("1");
                    struct tree_node* temp = current;
                    current = current->right;
                    if (current == NULL) {
                        // create a new node
                        new_node = malloc(sizeof(struct tree_node));
                        new_node->is_external = 0;
                        new_node->right = NULL;
                        new_node->left = NULL;
                        temp->right = new_node;
                        current = new_node;
                    } else {
                        // else -> already has a node there
                        ;
                    }
                } else {
                    // not set -> left
                    struct tree_node* temp = current;
                    current = current->left;
                    //printf("0");
                    if (current == NULL) {
                        // create a new node
                        new_node = malloc(sizeof(struct tree_node));
                        new_node->is_external = 0;
                        new_node->right = NULL;
                        new_node->left = NULL;
                        temp->left = new_node;
                        current = new_node;
                    } else {
                        // else -> already has a node there
                        ;
                    }
                }
            }
        }

        for (int c = 0; c < additional_bit; c++) {
            if (CHECK_BIT(code[i].bits[b], c)) {
                // is set -> right
                //printf("1");
                struct tree_node* temp = current;
                current = current->right;
                if (current == NULL) {
                    // create a new node
                    new_node = malloc(sizeof(struct tree_node));
                    new_node->is_external = 0;
                    new_node->right = NULL;
                    new_node->left = NULL;
                    temp->right = new_node;
                    current = new_node;
                } else {
                    // else -> already has a node there
                    ;
                }
            } else {
                // not set -> left
                struct tree_node* temp = current;
                current = current->left;
                //printf("0");
                if (current == NULL) {
                    // create a new node
                    new_node = malloc(sizeof(struct tree_node));
                    new_node->is_external = 0;
                    new_node->right = NULL;
                    new_node->left = NULL;
                    temp->left = new_node;
                    current = new_node;
                } else {
                    // else -> already has a node there
                    ;
                }
            }
        }
        // set the external!
        current->is_external = 1;
        current->bytes = i;

    }

    return tree;
}


// decompression
uint8_t* decompress(const uint8_t* sequences, struct tree* tree,
        uint64_t length, uint64_t* decompressed_length) {

    uint64_t bit_length = (length - 1) * 8 - sequences[length - 1];

    uint8_t* bit_array = calloc(bit_length, 1);

    uint64_t i = 0;
    int stop = 0;


    for (uint64_t t = 0; t < length; t++) {

        for (int p = 0 ; p < 8; p++) {
            if (i >= bit_length) {
                stop = 1;
                break;
            }
            if (CHECK_BIT(sequences[t], p) ) {
                bit_array[i] = 1;
            } else {
                bit_array[i] = 0;
            }
            i++;
        }
        if (stop == 1)
            break;

    }

    struct tree_node* current_node = tree->root;

    *decompressed_length = 0;

    uint8_t *decompressed_buffer = NULL;


    for (i = 0; i < bit_length; i++) {

        if (bit_array[i] == 1) {
            // if set -> go right
            current_node = current_node->right;
        } else {
            // not set -> go left
            current_node = current_node->left;
        }

        if (current_node->is_external == 1) {
            // find the byte!

            (*decompressed_length)++;

            if (*decompressed_length == 1) {
                decompressed_buffer = malloc(*decompressed_length);
            } else {
                decompressed_buffer = realloc(decompressed_buffer,
                        *decompressed_length);
            }
            decompressed_buffer[*decompressed_length - 1] = current_node->bytes;
            current_node = tree->root;
        }

    }

    free(bit_array);

    return decompressed_buffer;
}

// compression

uint8_t *compress(const uint8_t* message, size_t len,
        struct compressed* code, uint64_t* payload_length) {
    uint64_t length = 0;
    uint8_t byte;
    uint8_t additional_byte;
    uint8_t nbits;
    byte = message[0];

    struct compressed bits = code[byte];
    uint8_t * buffer = malloc(bits.complete_byte);
    memcpy(buffer + length, bits.bits, bits.complete_byte);
    length += bits.complete_byte;
    additional_byte = bits.bits[bits.complete_byte] >> (8 - bits.additional_bits);
    additional_byte  = additional_byte << (8 - bits.additional_bits);
    nbits = (8 - bits.additional_bits); // need n bits more to complete


    // ------------------------------------------- loop below
    // read a new byte from client
    for (size_t t = 1; t < len; t++) {


        byte = message[t];
        // printf("0x%x ", byte);
        bits = code[byte];

        additional_byte = additional_byte | (bits.bits[0] >> (8 - nbits));
        // additional byte is complete -> memcpy to the buffer

        if (bits.length < nbits) {
            nbits = nbits - bits.length;
            continue;
        }
        length++;
        buffer = (uint8_t *)realloc(buffer, length);
        memcpy(buffer + length - 1, &additional_byte, 1);


        // left 8 - nbits long

        int complete_byte_num_beside_nbits =
                (int) floor(((double) (bits.length - nbits)) / 8);

        int num = 0;
        uint8_t incomplete_byte = bits.bits[0] << nbits;
        // need nbits more
        int index = 1;
        for (; index < 32; index++) {

            if (complete_byte_num_beside_nbits <= num)
                break;
            byte = bits.bits[index];

            incomplete_byte = incomplete_byte | (byte >> (8 - nbits));

            // now complete!
            length++;
            buffer = (uint8_t *)realloc(buffer, length);
            memcpy(buffer + length - 1, &incomplete_byte, 1);
            num++;
            incomplete_byte = byte << nbits;

        }

        additional_byte = incomplete_byte
                | (bits.bits[index] >> (8 - nbits));

        if (complete_byte_num_beside_nbits <= 0) {
            nbits = 8 - (bits.length - nbits);
        } else {
            nbits = 8 - (bits.length - nbits) % 8;
        }

    }

    if (nbits == 8)
        nbits = 0;
    else {
        length++;
        buffer = (uint8_t *)realloc(buffer, length);
        memcpy(buffer + length - 1, &additional_byte, 1);
    }
    length++;
    buffer = (uint8_t *)realloc(buffer, length);
    memcpy(buffer + length - 1, &nbits, 1);

    *payload_length = length;

    return buffer;
}


char *bin2hex(const unsigned char *input, size_t len) {
    char *result;
    char *hexits = "0123456789ABCDEF";

    if (input == NULL || len == 0)
        return NULL;

    // (2 hexits + space) / chr + NULL
    size_t result_length = (len * 3) + 1;

    result = malloc(result_length);
    bzero(result, result_length);

    for (size_t i = 0; i < len; i++) {
        result[i * 3] = hexits[input[i] >> 4u];
        result[i * 3 + 1] = hexits[input[i] & 0x0Fu];
        result[i * 3 + 2] = ' '; // for readability
    }

    return result;

}

uint64_t swap_uint64(uint64_t val) {
    val = ((val << 8u) & 0xFF00FF00FF00FF00ULL )
            | ((val >> 8u) & 0x00FF00FF00FF00FFULL );
    val = ((val << 16u) & 0xFFFF0000FFFF0000ULL )
            | ((val >> 16u) & 0x0000FFFF0000FFFFULL );
    return (val << 32u) | (val >> 32u);
}


