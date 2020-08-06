#include "server.h"



int main(int argc, char** argv) {
    // usage check
    if (argc != 2)
        exit(1);

    // read the configuration file
    char* config_path = argv[1];
    FILE* ptr = fopen(config_path, "rb");
    if (ptr == NULL) {
        exit(1);
    }
    struct config config_bytes;
    // check first 6 bytes
    check( fread(&config_bytes, sizeof(config_bytes), 1, ptr),
           "read IP/port error");
    fseek(ptr, 0L, SEEK_END);
    size_t sz = ftell(ptr) - 6;
    fseek(ptr, 6L, SEEK_SET);
    char* directory = malloc(sz + 1);
    // check last bytes
    check( fread(directory, sz, 1, ptr), "read directory error");
    directory[sz] = 0;

    // create a new socket
    struct sockaddr_in  server_addr, client_addr;
    int server_socket, client_socket, addr_size;
    check( (server_socket = socket(AF_INET, SOCK_STREAM, 0)),
           "Failed to create socekt.");

    // initialise the address struct
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = config_bytes.IP_addr;
    server_addr.sin_port = config_bytes.server_port;

    int option = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
               &option, sizeof(int));

    // bind
    check( bind(server_socket, (SA*)&server_addr, sizeof(server_addr)),
           "Bind Faied!");

    // listen
    check( listen(server_socket, SOMAXCONN),
           "Listen Failed!");

    // create some shared data between threads
    struct global* global = global_create(server_socket);
    global->directory = directory;

    while (1) {
        if (global->end == 1) {
            pthread_cond_broadcast(&global->cond);
            break;
        }

        // a new connection!
        addr_size = sizeof(struct sockaddr_in);
        if ((client_socket =
                     accept(server_socket, (SA*)&client_addr,
                            (socklen_t*)&addr_size)) < 0) {
            pthread_cond_broadcast(&global->cond);
            break;
        }
        pthread_t thread;
        struct argument* arg = malloc(sizeof(struct argument));
        arg->global = global;
        arg->client_socket = client_socket;
        pthread_create(&thread, NULL, handle_connection, arg);

    } // while

    // clean up
    global_destroy(global);
    return 0;
}


void* handle_connection(void* arg) {

    struct argument* argument = (struct argument*)arg;
    int client_socket = argument->client_socket;
    struct global* shared_data = argument->global;
    while (1) {

        if (shared_data->end == 1) {
            close(client_socket);
            break;
        }
        // read first byte of message from client
        struct message message_byte;
        ssize_t nread;
        nread = recv(client_socket, &message_byte.first, 1, 0);
        // check implicit shutdown
        if (nread <= 0) {
            close(client_socket);
            break;
        }

        // read payload length from client
        nread = read(client_socket, &message_byte.p_len, 8);
        // check implicit shutdown
        if (nread <= 0) {
            close(client_socket);
            break;
        }

        // actual number (little)
        message_byte.p_len = swap_uint64(message_byte.p_len);

        // return header
        struct message_header header;
        header.type = message_byte.first >> 4u;


        if (header.type == 0x08) {
            // shutdown
            close(client_socket);
            shared_data->end = 1;
            shutdown(shared_data->server_socket, SHUT_RDWR);
            close(shared_data->server_socket);
            break;
        }

        if (header.type == 0x00) {
            // Echo function
            struct message m_out;
            m_out.first = 0x01u << 4u;
            m_out.first = m_out.first | (message_byte.first & 0x08u);
            if ((CHECK_BIT(message_byte.first, 4) == 0)
                && CHECK_BIT(message_byte.first, 5)) {
                // require compression
                // The is a separate compression process,
                // because the payload of echo may be a large number
                // read byte one by one -> no need to malloc a large chunck
                m_out.first = m_out.first | 0x08u;
                uint64_t length = 0;
                uint8_t byte;
                uint8_t additional_byte;
                uint8_t nbits;
                check(read(client_socket, &byte, 1), "read byte error");
                struct compressed bits = shared_data->code[byte];
                uint8_t * buffer = malloc(bits.complete_byte);
                memcpy(buffer + length, bits.bits, bits.complete_byte);
                length += bits.complete_byte;
                additional_byte = bits.bits[bits.complete_byte] >> (8 - bits.additional_bits);
                additional_byte  = additional_byte << (8 - bits.additional_bits);
                nbits = (8 - bits.additional_bits); // need n bits more to complete

                // read a new byte from client
                for (int t = 1; t < message_byte.p_len; t++) {
                    check(read(client_socket, &byte, 1), "read byte error");
                    bits = shared_data->code[byte];
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
                        buffer = (uint8_t *) realloc(buffer, length);
                        memcpy(buffer + length - 1, &incomplete_byte, 1);
                        num++;
                        incomplete_byte = byte << nbits;
                    }

                    // update nbits for next loop
                    additional_byte =
                            incomplete_byte | (bits.bits[index] >> (8 - nbits));
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
                m_out.p_len = swap_uint64(length);
                check(write(client_socket, &m_out, sizeof(struct message)),
                      "write error1");
                check(write(client_socket, buffer, length), "write error2");
                free(buffer);

            } else {
                // do not need compress
                char buffer[MAXLINE];
                m_out.p_len = swap_uint64(message_byte.p_len); // to big endian
                check(write(client_socket, &m_out, sizeof(struct message)),
                      "write error1");
                ssize_t already_read = 0;
                while (already_read < message_byte.p_len) {
                    check( nread = read(client_socket, buffer, MIN(MAXLINE,
                                                                   message_byte.p_len - already_read)),
                           "recv2 error");
                    already_read += nread;
                    check(write(client_socket, buffer, nread), "write error2");
                }
            }


        } else if (header.type == 0x02) {
            // directory listing
            struct message m_out;
            m_out.first = 0x03u << 4u;
            m_out.first = m_out.first | ((message_byte.first & 0x04u) << 1u);
            uint64_t temp_len;
            uint8_t * filenames = directory_listing(&temp_len, shared_data->directory);

            if (filenames == NULL) {
                // error
                m_out.first = 0xf0u;
                m_out.p_len = 0;
                check(write(client_socket, &m_out, sizeof(struct message)),
                      "write error1");
                close(client_socket);
                continue;
            }

            if (CHECK_BIT(m_out.first, 4u)) {
                // compress
                uint64_t compressed_payload_len;
                uint8_t* compressed = compress(filenames, temp_len, shared_data->code,
                                               &compressed_payload_len);
                m_out.p_len = swap_uint64(compressed_payload_len);
                check(write(client_socket, &m_out, sizeof(struct message)),
                      "write error1");
                check(write(client_socket, compressed, compressed_payload_len),
                      "write error2");
                free(compressed);
            } else {
                m_out.p_len = swap_uint64(temp_len);
                check(write(client_socket, &m_out, sizeof(struct message)),
                      "write error1");
                check(write(client_socket, filenames, temp_len), "write error2");

            }
            free(filenames);

        } else if (header.type == 0x04) {
            // file size!
            char* filename = malloc(message_byte.p_len + 1);
            check(read(client_socket, filename, message_byte.p_len),
                  "read file name 0x04 error");
            filename[message_byte.p_len] = 0;
            struct message m_out;

            if (CHECK_BIT(message_byte.first, 4)) {
                // decompress
                uint64_t decompressed_length;
                uint8_t* decompressed_buffer = decompress((uint8_t *)filename,
                                                          shared_data->tree, message_byte.p_len, &decompressed_length);
                free(filename);
                filename = (char*)decompressed_buffer;
                filename = realloc(filename, decompressed_length + 1);
                filename[decompressed_length] = 0;
            }

            uint8_t* file_size = file_size_query(shared_data->directory, filename);
            m_out.first = 0x05u << 4u;
            m_out.first = m_out.first | ((message_byte.first & 0x04u) << 1u);
            if (file_size == NULL) {
                // error
                m_out.first = 0xf0u;
                m_out.p_len = 0;
                check(write(client_socket, &m_out, sizeof(struct message)),
                      "write error1");
                close(client_socket);
                free(filename);
                continue;

            } else {
                if (CHECK_BIT(m_out.first, 4u)) {
                    // compress
                    uint64_t compressed_payload_len;
                    uint8_t* compressed = compress(file_size, 8, shared_data->code,
                                                   &compressed_payload_len);
                    m_out.p_len = swap_uint64(compressed_payload_len);
                    check(write(client_socket, &m_out, sizeof(struct message)),
                          "write error1");
                    check(write(client_socket, compressed, compressed_payload_len),
                          "write error2");
                    free(compressed);

                } else {
                    m_out.p_len = swap_uint64(8);
                    check(write(client_socket, &m_out, sizeof(struct message)),
                          "write error1");
                    check(write(client_socket, file_size, 8),
                          "write error2");
                }
                free(file_size);
            }
            free(filename);

        } else if (header.type == 0x06 ) {
            // retrieve file
            struct message m_out;
            m_out.first = 0x07u << 4u;
            m_out.first = m_out.first | ((message_byte.first & 0x04u) << 1u);
            struct retrieve_info* info = malloc(sizeof(struct retrieve_info));

            if (CHECK_BIT(message_byte.first, 4)) {
                // need to decompress
                uint8_t* compressed_buffer = malloc(message_byte.p_len);
                check( read(client_socket, compressed_buffer, message_byte.p_len),
                       "read compressed buffer error");
                uint64_t decompressed_length;
                uint8_t* decompressed_buffer = decompress(compressed_buffer,
                                                          shared_data->tree, message_byte.p_len, &decompressed_length);
                free(compressed_buffer);
                memcpy(&info->session_id, decompressed_buffer, 4);
                memcpy(&info->offset, decompressed_buffer + 4, 8);
                memcpy(&info->retrieve_length, decompressed_buffer + 12, 8);
                info->offset = swap_uint64(info->offset);
                info->retrieve_length = swap_uint64(info->retrieve_length);
                info->file_name = malloc(decompressed_length - 20 + 1);
                memcpy(info->file_name, decompressed_buffer + 20,
                       decompressed_length - 20);
                info->file_name[decompressed_length - 20] = 0;
                free(decompressed_buffer);

            } else {
                uint8_t info_buffer[20];
                check( read(client_socket, &info_buffer, 20),
                       "read info buffer error");
                memcpy(&info->session_id, info_buffer, 4);
                memcpy(&info->offset, info_buffer + 4, 8);
                memcpy(&info->retrieve_length, info_buffer + 12, 8);
                info->offset = swap_uint64(info->offset);
                info->retrieve_length = swap_uint64(info->retrieve_length);
                info->file_name= malloc(message_byte.p_len - 20 + 1);
                check( read(client_socket,
                            info->file_name, message_byte.p_len - 20),
                       "read file name error");
                info->file_name[message_byte.p_len - 20] = 0;
            }
            info->closed = 0;
            info->retrieving_finished = 0;
            info->request_num = 0;
            info->sent = 0;
            // store a struct associated with session ID and file query information
            // to a dynamic array called thread_pool->requests
            int find = -1;
            pthread_mutex_lock(&shared_data->mutex_map);
            for (int d = 0; d < shared_data->num_of_ID; d++) {
                if (shared_data->requests[d]->session_id == info->session_id) {
                    find = d;
                    break;
                }
            }
            if (find == -1) {
                // This session ID has not been recorded yet
                pthread_mutex_init(&info->lock, NULL);
                pthread_cond_init(&info->cond, NULL);
                shared_data->num_of_ID++;
                if (shared_data->num_of_ID == 1) {
                    shared_data->requests =
                            (struct retrieve_info**)malloc(sizeof(struct retrieve_info*));
                } else {
                    shared_data->requests =
                            (struct retrieve_info**)realloc(shared_data->requests,
                                                            sizeof(struct retrieve_info*) * shared_data->num_of_ID);
                }
                shared_data->requests[shared_data->num_of_ID - 1] = info;
                pthread_mutex_unlock(&shared_data->mutex_map);
            } else {
                // already have this session ID operated
                struct retrieve_info* temp_info = shared_data->requests[find];
                if (temp_info->offset != info->offset
                    || temp_info->retrieve_length != info->retrieve_length
                    || strcmp(temp_info->file_name, info->file_name) != 0) {
                    // same session ID with different file range, etc
                    if (temp_info->closed == 0) {
                        // there is an ongoing request -> error!
                        m_out.first = 0xf0u;
                        m_out.p_len = 0;
                        check(write(client_socket, &m_out, sizeof(struct message)),
                              "write error1");
                        close(client_socket);
                        free(info->file_name);
                        free(info);
                        continue;
                    } else {
                        // same session ID with new file range
                        free(temp_info->file_name);
                        pthread_cond_destroy(&temp_info->cond);
                        pthread_mutex_destroy(&temp_info->lock);
                        free(temp_info);
                        shared_data->requests[find] = info;
                    }
                    pthread_mutex_unlock(&shared_data->mutex_map);
                } else if (temp_info->closed == 2) {
                    // previous file retrieving failed
                    pthread_mutex_unlock(&shared_data->mutex_map);
                    m_out.first = 0xf0u;
                    m_out.p_len = 0;
                    check(write(client_socket, &m_out, sizeof(struct message)),
                          "write error1");
                    close(client_socket);
                    pthread_mutex_unlock(&temp_info->lock);
                    continue;

                } else if (temp_info->closed == 1) {
                    pthread_mutex_unlock(&shared_data->mutex_map);
                    // this file retrieving previously closed
                    // send back empty payload with type 0x07
                    free(info->file_name);
                    free(info);
                    m_out.first = 0x70u;
                    m_out.p_len = 0;
                    check(write(client_socket, &m_out, sizeof(struct message)),
                          "write error1");
                    close(client_socket);
                    continue;

                } else {
                    pthread_mutex_unlock(&shared_data->mutex_map);
                    free(info->file_name);
                    free(info);
                    // this file retrieving is ongoing -> multiplexing!
                    if (temp_info->retrieving_finished == 1) {
                        m_out.first = 0x70u;
                        m_out.p_len = 0;
                        check(write(client_socket, &m_out, sizeof(struct message)),
                              "write error1");
                        close(client_socket);
                        continue;
                    }
                    pthread_mutex_lock(&temp_info->lock);
                    temp_info->request_num++;
                    /* MULTIPLEXING starts!!*/
                    // 1. wait for the first client retireve the file content.
                    // 2. signal the cond (broadcast)
                    // 3. check if error
                    pthread_cond_wait(&temp_info->cond, &temp_info->lock);
                    if (temp_info->retrieving_finished == 2) {
                        // current file retrieving failed
                        m_out.first = 0xf0u;
                        m_out.p_len = 0;
                        check(write(client_socket, &m_out, sizeof(struct message)),
                              "write error1");
                        close(client_socket);
                        pthread_mutex_unlock(&temp_info->lock);
                        continue;
                    }
                    uint64_t portion = (uint64_t)(temp_info->file_content_len /
                                                  temp_info->request_num);
                    uint64_t p_len;
                    int nth = temp_info->finished_num;
                    temp_info->finished_num++;
                    pthread_mutex_unlock(&temp_info->lock);
                    if (nth < temp_info->request_num - 1) {
                        p_len = 20 + portion;
                    } else {
                        p_len = 20 + temp_info->file_content_len - (nth * portion);
                    }

                    uint8_t* payload = malloc(p_len);
                    memcpy(payload, &temp_info->session_id, 4);
                    uint64_t file_length = swap_uint64(p_len - 20);
                    memcpy(payload + 12, &file_length, 8);

                    uint64_t offset = nth * portion;
                    memcpy(payload + 20, temp_info->file_content + offset,
                           p_len - 20);

                    offset += temp_info->offset;
                    offset = swap_uint64(offset);

                    memcpy(payload + 4, &offset, 8);

                    if (temp_info->compressed == 1) {
                        // need to compress
                        m_out.first = m_out.first | 0x08u;
                        uint64_t compressed_payload_len;
                        uint8_t* compressed = compress(payload, p_len,
                                                       shared_data->code, &compressed_payload_len);
                        free(payload);
                        payload = compressed;
                        p_len = compressed_payload_len;
                    }

                    m_out.p_len = swap_uint64(p_len);
                    check(write(client_socket, &m_out, sizeof(struct message)),
                          "write error1");

                    check(write(client_socket, payload, p_len), "write error2");
                    free(payload);
                    pthread_mutex_lock(&temp_info->lock);
                    nth = temp_info->sent;
                    temp_info->sent++;
                    pthread_mutex_unlock(&temp_info->lock);

                    // the last finished transfer cleans up
                    if (nth == temp_info->request_num - 1) {
                        free(temp_info->file_content);
                        temp_info->closed = 1;
                        pthread_cond_destroy(&temp_info->cond);
                        pthread_mutex_destroy(&temp_info->lock);
                    }
                    continue;

                }
            }


            uint8_t* file_content = retrieve_file(*info, shared_data->directory);
            info->retrieving_finished = 1;
            pthread_mutex_lock(&info->lock);
            info->request_num++;
            pthread_mutex_unlock(&info->lock);
            if (file_content == NULL) {
                // signal all waiting threads
                info->retrieving_finished = 2;
                info->closed = 2;
                pthread_cond_broadcast(&info->cond);

                m_out.first = 0xf0u;
                m_out.p_len = 0;
                check(write(client_socket, &m_out, sizeof(struct message)),
                      "write error1");
                close(client_socket);
                continue;
            }

            if (CHECK_BIT(m_out.first, 4u)) {
                info->compressed = 1;
            } else {
                info->compressed = 0;
            }
            info->file_content = file_content;
            info->file_content_len = info->retrieve_length;

            /* MULTIPLEXING starts!!*/

            info->finished_num = 0;
            if (info->request_num != 1)
                pthread_cond_broadcast(&info->cond);
            uint64_t portion = (uint64_t) (info->file_content_len/info->request_num);
            uint64_t p_len;
            pthread_mutex_lock(&info->lock);
            int nth = info->finished_num;
            info->finished_num++;
            pthread_mutex_unlock(&info->lock);

            // total length = info->file_content_len
            // total requests = info->request_num
            // each length = floor(info->file_content_len/info->request_num)

            if (nth < info->request_num - 1) {
                p_len = 20 + portion;
            } else {
                p_len = 20 + info->file_content_len - (nth * portion);
            }
            uint8_t* payload = malloc(p_len);
            memcpy(payload, &info->session_id, 4);
            uint64_t file_length = swap_uint64(p_len - 20);
            memcpy(payload + 12, &file_length, 8);

            uint64_t offset = nth * portion;
            memcpy(payload + 20, info->file_content + offset,  p_len - 20);

            offset += info->offset;
            offset = swap_uint64(offset);

            memcpy(payload + 4, &offset, 8);

            if (info->compressed == 1) {
                // need to compress
                m_out.first = m_out.first | 0x08u;
                uint64_t compressed_payload_len;
                uint8_t* compressed = compress(payload, p_len, shared_data->code,
                                               &compressed_payload_len);
                free(payload);
                payload = compressed;
                p_len = compressed_payload_len;
            }

            m_out.p_len = swap_uint64(p_len);
            check(write(client_socket, &m_out, sizeof(struct message)),
                  "write error1");
            check(write(client_socket, payload, p_len), "write error2");
            free(payload);

            pthread_mutex_lock(&info->lock);
            nth = info->sent;
            info->sent++;
            pthread_mutex_unlock(&info->lock);
            // the last finished transfer cleans up
            if (nth == info->request_num - 1) {
                free(info->file_content);
                info->closed = 1;
                pthread_cond_destroy(&info->cond);
                pthread_mutex_destroy(&info->lock);
            }

        } else {
            // error functionality
            struct message m_out;
            m_out.first = 0xf0u;
            m_out.p_len = 0;
            check(write(client_socket, &m_out, sizeof(struct message)),
                  "write error1");
            close(client_socket);
            break;
        }

    }

    free(arg);
    return NULL;
}


uint8_t *directory_listing(uint64_t* payload_length, char* directory) {
    uint64_t length = 0;
    uint8_t * filenames = NULL;
    int first = 1;
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir (directory)) != NULL) {
        // print all the regular files and directories within directory
        while ((ent = readdir (dir)) != NULL) {
            if (ent->d_type != DT_REG) {
                continue;
            }
            length += strlen(ent->d_name);
            if (first == 1) {
                filenames = (uint8_t*)malloc(length);
                first = 0;
            } else {
                filenames = (uint8_t*)realloc(filenames, length);
            }
            memcpy(filenames + length - strlen(ent->d_name), ent->d_name,
                   strlen(ent->d_name));
            length++;
            filenames = (uint8_t*)realloc(filenames, length);
            filenames[length - 1] = 0x00;
        }
        closedir (dir);
    } else {
        //could not open directory
        return NULL;
    }
    *payload_length = length;
    return filenames;
}




uint8_t *file_size_query(char* directory, char* file_name) {
    char* full_name = malloc(strlen(directory) + strlen(file_name) + 2);
    strcpy(full_name, directory);
    strcat(full_name, "/");
    strcat(full_name, file_name);
    full_name[strlen(directory) + strlen(file_name) + 1] = 0;
    FILE* fpt = fopen(full_name, "rb");
    free(full_name);
    if (fpt == NULL) {
        // cannot open
        return NULL;
    }
    uint64_t sz;
    fseek(fpt, 0L, SEEK_END);
    sz = ftell(fpt);
    uint8_t* file_size = malloc(sizeof(uint8_t) * 8);
    sz = swap_uint64(sz);
    memcpy(file_size, &sz, 8);
    fclose(fpt);
    return file_size;
}




uint8_t* retrieve_file(struct retrieve_info info, char* directory) {
    char* full_name = malloc(strlen(directory) + strlen(info.file_name) + 2);
    strcpy(full_name, directory);
    strcat(full_name, "/");
    strcat(full_name, info.file_name);
    full_name[strlen(directory) + strlen(info.file_name) + 1] = 0;
    FILE* fpt = fopen(full_name, "rb");
    free(full_name);
    if (fpt == NULL) {
        // cannot open
        return NULL;
    }
    uint64_t sz;
    fseek(fpt, 0L, SEEK_END);
    sz = ftell(fpt);
    if (info.offset + info.retrieve_length > sz) {
        // acquired length is too large
        fclose(fpt);
        return NULL;
    }
    fseek(fpt, info.offset, SEEK_SET);
    uint8_t* file_content = malloc(info.retrieve_length * 100);
    file_content[info.retrieve_length] = 0;
    if (fread(file_content, info.retrieve_length, 1, fpt) == 0) {
        fclose(fpt);
        free(file_content);
        return NULL;
    }
    return file_content;
}