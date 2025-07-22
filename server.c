#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 104857600

const char DEFAULT_WEB_ROOT[] = "./web_root";

typedef struct  {
    int port;
    char root_dir[1024];
} ServerOptions;

char *concat_string(const char* str1, const char* str2) {
    size_t length = strlen(str1) + strlen(str2) + 1; // +1 for the null terminator
    char *buffer = (char *)malloc(length);
    strcpy(buffer, str1);
    strcat(buffer, str2);
    return buffer;
}

bool file_exists(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file != NULL) {
        fclose(file);
        return true;
    }
    return false;
}

int is_directory(const char *path) {
    struct stat statbuf;
    if (stat(path, &statbuf) != 0)
        return 0;
    return S_ISDIR(statbuf.st_mode);
}

const char *get_file_extension(const char *file_name) {
    const char *dot = strrchr(file_name, '.');
    if (!dot || dot == file_name) {
        char *suffixed_file_name = concat_string(file_name, ".json");
        if (file_exists(suffixed_file_name)) {
            free(suffixed_file_name);
            return "json";
        }
        free(suffixed_file_name);
        return "";
    }
    return dot + 1;
}

const char *get_mime_type(const char *file_ext) {
    if (strcasecmp(file_ext, "html") == 0 || strcasecmp(file_ext, "htm") == 0) {
        return "text/html";
    } else if (strcasecmp(file_ext, "txt") == 0) {
        return "text/plain";
    } else if (strcasecmp(file_ext, "jpg") == 0 || strcasecmp(file_ext, "jpeg") == 0) {
        return "image/jpeg";
    } else if (strcasecmp(file_ext, "png") == 0) {
        return "image/png";
    } else if (strcasecmp(file_ext, "json") == 0) {
        return "application/json";
    } else {
        return "application/octet-stream";
    }
}

bool case_insensitive_compare(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        if (tolower((unsigned char)*s1) != tolower((unsigned char)*s2)) {
            return false;
        }
        s1++;
        s2++;
    }
    return *s1 == *s2;
}

char *get_file_case_insensitive(const char *file_name) {
    DIR *dir = opendir(".");
    if (dir == NULL) {
        perror("opendir");
        return NULL;
    }

    struct dirent *entry;
    char *found_file_name = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (case_insensitive_compare(entry->d_name, file_name)) {
            found_file_name = entry->d_name;
            break;
        }
    }

    closedir(dir);
    return found_file_name;
}

char *url_decode(const char *src) {
    size_t src_len = strlen(src);
    char *decoded = malloc(src_len + 1);
    size_t decoded_len = 0;

    // decode %2x to hex
    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == '%' && i + 2 < src_len) {
            int hex_val;
            sscanf(src + i + 1, "%2x", &hex_val);
            decoded[decoded_len++] = hex_val;
            i += 2;
        } else {
            decoded[decoded_len++] = src[i];
        }
    }

    // add null terminator
    decoded[decoded_len] = '\0';
    return decoded;
}

char *read_file(const char *filename, size_t *fileSize) {
    FILE *file = fopen(filename, "rb"); // Open in binary mode to handle all file types correctly

    if (file == NULL) {
        perror("Error opening file"); // Print an error message to the console
        return NULL; // Indicate an error
    }

    // Determine the file size
    fseek(file, 0, SEEK_END); // Move the file pointer to the end of the file
    long file_size_long = ftell(file); // Get the current position (which is the file size)
    rewind(file); // Reset the file pointer to the beginning of the file

    // Check for potential errors with file size (e.g., if it's too large)
    if (file_size_long < 0) {
        perror("Error getting file size");
        fclose(file);
        return NULL;
    }

    // Ensure file size doesn't exceed the maximum size of size_t
    if (file_size_long > SIZE_MAX) {
        fprintf(stderr, "File size is too large to handle.\n");
        fclose(file);
        return NULL;
    }
    *fileSize = (size_t)file_size_long; // Cast to size_t (safer)

    // Allocate memory for the character array
    char *buffer = (char *)malloc(*fileSize + 1); // +1 for the null terminator
    if (buffer == NULL) {
        perror("Error allocating memory");
        fclose(file);
        return NULL;
    }

    // Read the file into the buffer
    size_t bytesRead = fread(buffer, 1, *fileSize, file);
    if (bytesRead != *fileSize) {
        if (ferror(file)) {
            perror("Error reading file");
            free(buffer);
            fclose(file);
            return NULL;
        }
    }

    // 4. Null-terminate the buffer (important for strings!)
    buffer[*fileSize] = '\0';

    // 5. Close the file
    fclose(file);

    return buffer; // Return the pointer to the character array
}

void build_http_response(const char *file_name, 
                        const char *file_ext,
                        char *response, 
                        size_t *response_len) {
    // build HTTP header
    const char *mime_type = get_mime_type(file_ext);

    int file_fd = open(file_name, O_RDONLY);
    if (file_fd == -1) {
        // If file doesn't exist, check if file with ".json" suffix exists
        char *suffixed_file_name = concat_string(file_name, ".json");
        file_fd = open(suffixed_file_name, O_RDONLY);
        free(suffixed_file_name);
    }
    if (file_fd == -1) {
        // If file still doesn't exist, return default_response
        size_t default_response_file_size;
        char *default_response = read_file("default_response", &default_response_file_size);
        memcpy(response, default_response, default_response_file_size);
        *response_len = default_response_file_size;
        free(default_response);
        return;
    }

    // get file size for Content-Length
    struct stat file_stat;
    fstat(file_fd, &file_stat);
    off_t file_size = file_stat.st_size;

    char *header = (char *)malloc(BUFFER_SIZE * sizeof(char));
    snprintf(header, BUFFER_SIZE,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "\r\n",
             mime_type);

    // copy header to response buffer
    *response_len = 0;
    memcpy(response, header, strlen(header));
    *response_len += strlen(header);
    free(header);

    // copy file to response buffer
    ssize_t bytes_read;
    while ((bytes_read = read(file_fd, 
                            response + *response_len, 
                            BUFFER_SIZE - *response_len)) > 0) {
        *response_len += bytes_read;
    }

    close(file_fd);
}

void *handle_client(void *arg) {
    int client_fd = *((int *)arg);
    char *buffer = (char *)malloc(BUFFER_SIZE * sizeof(char));

    // receive request data from client and store into buffer
    ssize_t bytes_received = recv(client_fd, buffer, BUFFER_SIZE, 0);
    if (bytes_received > 0) {
        // parse the request
        regex_t regex;
        regcomp(&regex, "^([A-Z]+) /([^ ?]*)\\?{0,1}(.*) HTTP/1", REG_EXTENDED);
        regmatch_t matches[3];

        if (regexec(&regex, buffer, 3, matches, 0) == 0) {
            // extract filename from request and decode URL
            buffer[matches[2].rm_eo] = '\0';
            const char *url_encoded_file_name = buffer + matches[2].rm_so;
            char *file_name = url_decode(url_encoded_file_name);

            // get file extension
            char file_ext[32];
            strcpy(file_ext, get_file_extension(file_name));

            // build HTTP response
            char *response = (char *)malloc(BUFFER_SIZE * 2 * sizeof(char));
            size_t response_len;
            build_http_response(file_name, file_ext, response, &response_len);

            // send HTTP response to client
            send(client_fd, response, response_len, 0);

            free(response);
            free(file_name);
        }
        regfree(&regex);
    }
    close(client_fd);
    free(arg);
    free(buffer);
    return NULL;
}

int create_server(int port) {
    int server_fd;
    struct sockaddr_in server_addr;

    printf("Starting server at port %i\n", port);
    // create server socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // config socket
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // bind socket to port
    if (bind(server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // listen for connections
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

void server_listen(int server_fd) {
    while (1) {
        // client info
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int *client_fd = malloc(sizeof(int));

        // accept client connection
        if ((*client_fd = accept(server_fd,
                                (struct sockaddr *)&client_addr,
                                &client_addr_len)) < 0) {
            perror("accept failed");
            continue;
        }

        // create a new thread to handle client request
        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_client, (void *)client_fd);
        pthread_detach(thread_id);
    }
}

int parse_arguments(int argc, char*argv[], ServerOptions *options) {
    int opt;
    while((opt = getopt(argc, argv, ":p:d:h")) != -1)
    {
        switch(opt)
        {
            case 'p':
                int port_int = atoi(optarg);
                if (port_int > 0) {
                    printf("Using port: %s\n", optarg);
                    options->port = atoi(optarg);
                } else {
                    printf("Invalid port: %s\n", optarg);
                    return 1;
                }
                break;
            case 'd':
                if (is_directory(optarg)) {
                    printf("Using web root directory: %s\n", optarg);
                    strcpy(options->root_dir, optarg);
                } else {
                    printf("Invalid directory: %s\n", optarg);
                    return 1;
                }
                break;
            case 'h':
                printf("Usage:\n");
                printf("-p <PORT>         Web server's port number\n");
                printf("-d <DIRECTORY>    Root directory of web content\n");
                return 1;
            case ':':
                printf("option needs a value\n");
                break;
            case '?':
                printf("unknown option: %c\n", optopt);
                break;
        }
    }

    // optind is for the extra arguments
    // which are not parsed
    for(; optind < argc; optind++){
        printf("extra arguments: %s\n", argv[optind]);
    }

    if (options->port == 0) {
        options->port = DEFAULT_PORT;
        printf("Using default port: %d\n", DEFAULT_PORT);
    }
    if (strlen(options->root_dir) == 0) {
        strcpy(options->root_dir, DEFAULT_WEB_ROOT);
        printf("Using default web root directory: %s\n", DEFAULT_WEB_ROOT);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    ServerOptions options = { 0, ""};
    if(parse_arguments(argc, argv, &options) != 0 ) {
        return EXIT_FAILURE;
    }
    if (strlen(options.root_dir) != 0) {
        chdir(options.root_dir);
    }
    int server_fd = create_server(options.port);
    server_listen(server_fd);
    close(server_fd);
    return 0;
}