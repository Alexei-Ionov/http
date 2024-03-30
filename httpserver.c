#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>

#include "libhttp.h"
#include "wq.h"

#include <dirent.h>

#include <semaphore.h>

#define SEEK_END 2
#define SEEK_SET 0


/*
 * Global configuration variables.
 * You need to use these in your implementation of handle_files_request and
 * handle_proxy_request. Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
wq_t work_queue; // Only used by poolserver
int num_threads; // Only used by poolserver
int server_port; // Default value: 8000
char* server_files_directory;
char* server_proxy_hostname;
int server_proxy_port;

struct threadArgs { 
  int sender;
  int reciever;
};

struct threadServerArgs { 
  int fd;
  void (*request_handler)(int);
};
/*
converts an integer to its string version.
*/
char* length_to_str(int content_length) { 
  int buff_length = snprintf(NULL, 0, "%d", content_length);
  if (buff_length < 0) {
    perror("failed to get length");
    exit(-1);
  }
  char *buffer = malloc(buff_length+1);
  if (buffer == NULL) { 
    perror("malloc failed");
    exit(-1);
  }
  buffer[buff_length] = '\0';

  int ret = snprintf(buffer, buff_length + 1, "%d", content_length);
  if (ret < 0) {
    perror("snprintf failed");
    free(buffer);
    exit(-1);
  }
  return buffer;
}


/*
 * Serves the contents the file stored at `path` to the client socket `fd`.
 * It is the caller's reponsibility to ensure that the file stored at `path` exists.
 */
void serve_file(int fd, char* path) {

  FILE* file = fopen(path, "r");
  int ret = fseek(file, 0, SEEK_END);
  if (ret == -1) { 
    perror("Failed to fseek");
    exit(-1);
  }
  int length = ftell(file);
  if (length == -1) { 
    perror("Failed to ftell");
    exit(-1);
  }
  fclose(file);
  char *content_length = length_to_str(length);
  http_start_response(fd, 200);
  http_send_header(fd, "Content-Type", http_get_mime_type(path));
  http_send_header(fd, "Content-Length", content_length); 
  http_end_headers(fd);
  int server_fd = open(path, O_RDONLY);
  writeData(server_fd, fd);

  free(content_length);
  close(server_fd);
}
char* getHTTPLink(char* path, char* filename) { 
  int length = strlen("<a href=\"//\"></a><br/>") + strlen(path) + strlen(filename) * 2 + 2;
  char *buf = malloc(length);
  buf[length - 2] = '\n';
  buf[length - 1] = '\0';
  http_format_href(buf, path, filename);
  return buf;
}
void writeLink(int fd, char* path, char* filename) { 
  /*
  `<a href="/path/filename">filename</a><br/>`

  */
  char* buf = getHTTPLink(path, filename);
  int length = strlen(buf);
  int bytes_written;
  int total_bytes_written = 0;
  while ((bytes_written = write(fd, &buf[total_bytes_written], length - total_bytes_written)) > 0) { 
    total_bytes_written += bytes_written;
  }
}
char* getParentPath(char* path) { 
  if (strcmp(path, "./") == 0) { 
    return "./";
  }
  int i = strlen(path) - 1;

  /*
  .//my_documents
  */
  if (path[i] == '/') { 
    i -= 1;
  }
  while (i >= 0 && path[i] != '/') { 
    i -= 1;
  }
  i -= 1;
  char *buf = malloc(i + 2);
  buf[i+1] = '\0';
  memcpy(buf, path, i+1);
  return buf;
}
int getContentLength(int fd, char* path) { 
  DIR *directory = opendir(path);
  if (directory == NULL) { 
    send404(fd);
    exit(-1);
  }
  struct dirent *dir_entry = readdir(directory);
  int content_length = 0;
  while (dir_entry != NULL) {
    char *link = getHTTPLink(path, dir_entry->d_name);
    content_length += strlen(link);
    dir_entry = readdir(directory);
  }
  char* parent_path = getParentPath(path);
  content_length += strlen(parent_path);
  return content_length;
}
void serve_directory(int fd, char* path) {
  DIR *directory = opendir(path);
  if (directory == NULL) { 
    send404(fd);
    return;
  }
  struct dirent *dir_entry = readdir(directory);
  while (dir_entry != NULL) {
    if (strcmp(dir_entry->d_name, "index.html") == 0) { 
      int length = strlen(path) + strlen("/index.html") + 1;
      char new_path[length];
      http_format_index(&new_path, path);
      serve_file(fd, new_path);
      return;
    }
    dir_entry = readdir(directory);
  }
  /*
  
  if we didn't find the index.html file then we fill fall through to here where we 
  will just list all of the links to the children in the current directory as well as
  a link to the parent directory
  
  */
  int length = getContentLength(fd, path);
  char *content_length = length_to_str(length);

  http_start_response(fd, 200);
  http_send_header(fd, "Content-Type", http_get_mime_type(".html"));
  http_send_header(fd, "Content-Length", content_length); 
  http_end_headers(fd);

  /*
  now to write the actual content of the response
  */
  directory = opendir(path);
  if (directory == NULL) { 
    perror("failed to open directory on the second run");
    return;
  }
  dir_entry = readdir(directory);
  while (dir_entry != NULL) {
    writeLink(fd, path, dir_entry->d_name);
    dir_entry = readdir(directory);
  }
  /* now write the parent directory!*/

  char* parent_path = getParentPath(path);

  if (strcmp(parent_path, "./") == 0) { 
    writeLink(fd, parent_path, parent_path);
    free(parent_path);
    return;
  }

  int i = strlen(parent_path) - 1;
  while (i >= 0 && parent_path[i] != '/') { 
    i -= 1;
  }
  char *directory_name = malloc(sizeof(strlen(parent_path) - i));
  directory_name[strlen(parent_path) - i - 1] = '\0';
  int directory_index = 0;
  while (i < strlen(parent_path)) { 
    directory_name[directory_index] = parent_path[i];
    i += 1;
    directory_index += 1;
  }
  writeLink(fd, parent_path, directory_name);
  free(parent_path);
  free(directory_name);
  free(content_length);
  return;
}

void send404(int fd) { 
  http_start_response(fd, 404);
  http_send_header(fd, "Content-Type", "text/html");
  http_end_headers(fd);
  close(fd);
}
/*
 * Reads an HTTP request from client socket (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 *
 *   Closes the client socket (fd) when finished.
 */
void handle_files_request(int fd) {

  struct http_request* request = http_request_parse(fd);

  if (request == NULL || request->path[0] != '/') {
    http_start_response(fd, 400);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    close(fd);
    return;
  }

  if (strstr(request->path, "..") != NULL) {
    http_start_response(fd, 403);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    close(fd);
    return;
  }

  /* Add `./` to the beginning of the requested path */
  char* path = malloc(2 + strlen(request->path) + 1);
  path[0] = '.';
  path[1] = '/';
  memcpy(path + 2, request->path, strlen(request->path) + 1);
  struct stat info;
  if (stat(path, &info) != 0) { 
    send404(fd);
    return;
  }
  if (S_ISDIR(info.st_mode)) { 
    serve_directory(fd, path);
  } else { 
    /*
    else path leads to file. 

    first we check if path to file is valid (i.e. file exists)
    */
    int server_fd = open(path, O_RDONLY);
    if (server_fd == -1) { 
      send404(fd);
    } else { 
      serve_file(fd, path);
    }
    close(server_fd);
  }

  close(fd);
  return;
}

/*
writes data from one fd to the other
*/
int writeData(int fd_sender, int fd_reciever) { 
  int bytes_written;
  int buf_size = 4096;
  char buffer[buf_size];
  int write_index;
  int bytes_read;
  while ((bytes_read = read(fd_sender, &buffer, buf_size)) > 0) { 
    write_index = 0;
    while (write_index != bytes_read) { 
      write_index += write(fd_reciever, &buffer[write_index], buf_size - write_index);
    }
  }
  return write_index;
}

void *manageProxyRequest(void *arg) {
  struct threadArgs *args = (struct threadArgs*) arg;
  int fd_sender = args->sender;
  int fd_reciever = args->reciever;
  int bytes_read;
  int buf_size = 4096;
  char buf[buf_size];
  int bytes_written;
  while ((bytes_read = read(fd_sender, &buf, buf_size)) > 0) { 
    bytes_written = write(fd_reciever, &buf, bytes_read);
    if (bytes_written != bytes_read) { 
      close(fd_reciever);
      close(fd_sender);
      return;
    }
  }
}
/*
 * Opens a connection to the proxy target (hostname=server_proxy_hostname and
 * port=server_proxy_port) and relays traffic to/from the stream fd and the
 * proxy target_fd. HTTP requests from the client (fd) should be sent to the
 * proxy target (target_fd), and HTTP responses from the proxy target (target_fd)
 * should be sent to the client (fd).
 *
 *   +--------+     +------------+     +--------------+
 *   | client | <-> | httpserver | <-> | proxy target |
 *   +--------+     +------------+     +--------------+
 *
 *   Closes client socket (fd) and proxy target fd (target_fd) when finished.
 */

void handle_proxy_request(int fd) {


  /*
  * The code below does a DNS lookup of server_proxy_hostname and
  * opens a connection to it. Please do not modify.
  */
  struct sockaddr_in target_address;
  memset(&target_address, 0, sizeof(target_address));
  target_address.sin_family = AF_INET;
  target_address.sin_port = htons(server_proxy_port);

  // Use DNS to resolve the proxy target's IP address
  struct hostent* target_dns_entry = gethostbyname2(server_proxy_hostname, AF_INET);

  // Create an IPv4 TCP socket to communicate with the proxy target.
  int target_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (target_fd == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
    close(fd);
    exit(errno);
  }

  if (target_dns_entry == NULL) {
    fprintf(stderr, "Cannot find host: %s\n", server_proxy_hostname);
    close(target_fd);
    close(fd);
    exit(ENXIO);
  }

  char* dns_address = target_dns_entry->h_addr_list[0];

  // Connect to the proxy target.
  memcpy(&target_address.sin_addr, dns_address, sizeof(target_address.sin_addr));
  int connection_status =
      connect(target_fd, (struct sockaddr*)&target_address, sizeof(target_address));

  if (connection_status < 0) {
    /* Dummy request parsing, just to be compliant. */
    http_request_parse(fd);

    http_start_response(fd, 502);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    close(target_fd);
    close(fd);
    return;
  }
  

  struct threadArgs *arg1 = malloc(sizeof(struct threadArgs));
  arg1->sender = fd;
  arg1->reciever = target_fd;


  struct threadArgs *arg2 = malloc(sizeof(struct threadArgs));
  arg2->sender = target_fd;
  arg2->reciever = fd;

  struct threadArgs* args[2];
  args[0] = arg1;
  args[1] = arg2;

  pthread_t threads[2];

  for (int i = 0; i < 2; i++) { 
    int res = pthread_create(&threads[i], NULL, manageProxyRequest, (void*)args[i]);
    if (res == -1) { 
      perror("thread creation failed");
      return;
    }
  }
  pthread_join(threads[0], NULL);
  pthread_join(threads[1], NULL);
  
 
  free(arg1);
  free(arg2); 
}

#ifdef POOLSERVER
/*
 * All worker threads will run this function until the server shutsdown.
 * Each thread should block until a new request has been received.
 * When the server accepts a new connection, a thread should be dispatched
 * to send a response to the client.
 */
void* handle_clients(void* void_request_handler) {
  void (*request_handler)(int) = (void (*)(int))void_request_handler;
  /* (Valgrind) Detach so thread frees its memory on completion, since we won't
   * be joining on it. */
  pthread_detach(pthread_self());
  while (1) { 
    int client_fd = wq_pop(&work_queue);
    request_handler(client_fd);
  }
}

/*
 * Creates `num_threads` amount of threads. Initializes the work queue.
 */
void init_thread_pool(int num_threads, void (*request_handler)(int)) {

  wq_init(&work_queue);
  pthread_t threads[num_threads];
  for (int i = 0; i < num_threads; i++) { 
    int res = pthread_create(&threads[i], NULL, handle_clients, (void*)request_handler);
    if (res == -1) { 
      perror("thread creation failed");
      return;
    }
  }
}
#endif
void *handle_request(void *arg) { 
  struct threadServerArgs *args = (struct threadServerArgs*) arg;
  args->request_handler(args->fd);
}
/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int* socket_number, void (*request_handler)(int)) {
  (void)request_handler;
  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;

  // Creates a socket for IPv4 and TCP.
  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    perror("Failed to create a new socket");
    exit(errno);
  }

  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option, sizeof(socket_option)) ==
      -1) {
    perror("Failed to set socket options");
    exit(errno);
  }

  // Setup arguments for bind()
  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  /*
   * TODO: PART 1
   *
   * Given the socket created above, call bind() to give it
   * an address and a port. Then, call listen() with the socket.
   * An appropriate size of the backlog is 1024, though you may
   * play around with this value during performance testing.sm
   */
  int ret = bind(*socket_number, (const struct sockaddr *) &server_address, sizeof(server_address));
  if (ret) { 
    perror("Failed to bind");
    exit(-1);
  }
  ret = listen(*socket_number, 1024);
  if (ret) { 
    perror("Failed to listen");
    exit(-1);
  }

  printf("Listening on port %d...\n", server_port);

#ifdef POOLSERVER
  /*
   * The thread pool is initialized *before* the server
   * begins accepting client connections.
   */
  init_thread_pool(num_threads, request_handler);
#endif

  while (1) {
    client_socket_number = accept(*socket_number, (struct sockaddr*)&client_address,
                                  (socklen_t*)&client_address_length);
    if (client_socket_number < 0) {
      perror("Error accepting socket");
      continue;
    }

    printf("Accepted connection from %s on port %d\n", inet_ntoa(client_address.sin_addr),
           client_address.sin_port);

#ifdef BASICSERVER
    /*
     * This is a single-process, single-threaded HTTP server.
     * When a client connection has been accepted, the main
     * process sends a response to the client. During this
     * time, the server does not listen and accept connections.
     * Only after a response has been sent to the client can
     * the server accept a new connection.
     */
    request_handler(client_socket_number);

#elif FORKSERVER
    /*
     * TODO: PART 5
     *
     * When a client connection has been accepted, a new
     * process is spawned. This child process will send
     * a response to the client. Afterwards, the child
     * process should exit. During this time, the parent
     * process should continue listening and accepting
     * connections.
     */

    /* PART 5 BEGIN */
    int pid = fork(); 
    if (pid == 0) { 
      close(*socket_number);
      request_handler(client_socket_number);
      close(client_socket_number);
    } else if (pid > 0) { 
      close(client_socket_number);
    } else { 
      perror("fork failed");
      exit(-1);
    }

    /* PART 5 END */

#elif THREADSERVER
    /*
     * TODO: PART 6
     *
     * When a client connection has been accepted, a new
     * thread is created. This thread will send a response
     * to the client. The main thread should continue
     * listening and accepting connections. The main
     * thread will NOT be joining with the new thread.
     */

    /* PART 6 BEGIN */
    
    struct threadServerArgs *args = malloc(sizeof(struct threadServerArgs));
    args->fd = client_socket_number;
    args->request_handler = request_handler;
  
    pthread_t thread;
    int res = pthread_create(&thread, NULL, handle_request, (void*)args);
    if (res == -1) { 
      perror("thread creation failed");
      return;
    }
    /* PART 6 END */
#elif POOLSERVER
    /*
     * TODO: PART 7
     *
     * When a client connection has been accepted, add the
     * client's socket number to the work queue. A thread
     * in the thread pool will send a response to the client.
     */
    wq_push(&work_queue, client_socket_number);
    
#endif
  }

  shutdown(*socket_number, SHUT_RDWR);
  close(*socket_number);
}

int server_fd;
void signal_callback_handler(int signum) {
  printf("Caught signal %d: %s\n", signum, strsignal(signum));
  printf("Closing socket %d\n", server_fd);
  if (close(server_fd) < 0)
    perror("Failed to close server_fd (ignoring)\n");
  exit(0);
}

char* USAGE =
    "Usage: ./httpserver --files some_directory/ [--port 8000 --num-threads 5]\n"
    "       ./httpserver --proxy inst.eecs.berkeley.edu:80 [--port 8000 --num-threads 5]\n";

void exit_with_usage() {
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char** argv) {
  signal(SIGINT, signal_callback_handler);
  signal(SIGPIPE, SIG_IGN);

  /* Default settings */
  server_port = 8000;
  void (*request_handler)(int) = NULL;

  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;
      server_files_directory = argv[++i];
      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    } else if (strcmp("--proxy", argv[i]) == 0) {
      request_handler = handle_proxy_request;

      char* proxy_target = argv[++i];
      if (!proxy_target) {
        fprintf(stderr, "Expected argument after --proxy\n");
        exit_with_usage();
      }

      char* colon_pointer = strchr(proxy_target, ':');
      if (colon_pointer != NULL) {
        *colon_pointer = '\0';
        server_proxy_hostname = proxy_target;
        server_proxy_port = atoi(colon_pointer + 1);
      } else {
        server_proxy_hostname = proxy_target;
        server_proxy_port = 80;
      }
    } else if (strcmp("--port", argv[i]) == 0) {
      char* server_port_string = argv[++i];
      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    } else if (strcmp("--num-threads", argv[i]) == 0) {
      char* num_threads_str = argv[++i];
      if (!num_threads_str || (num_threads = atoi(num_threads_str)) < 1) {
        fprintf(stderr, "Expected positive integer after --num-threads\n");
        exit_with_usage();
      }
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  if (server_files_directory == NULL && server_proxy_hostname == NULL) {
    fprintf(stderr, "Please specify either \"--files [DIRECTORY]\" or \n"
                    "                      \"--proxy [HOSTNAME:PORT]\"\n");
    exit_with_usage();
  }

#ifdef POOLSERVER
  if (num_threads < 1) {
    fprintf(stderr, "Please specify \"--num-threads [N]\"\n");
    exit_with_usage();
  }
#endif

  chdir(server_files_directory);
  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;
}
