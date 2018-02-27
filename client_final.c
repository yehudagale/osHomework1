/* Generic */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

/* Network */
#include <netdb.h>
#include <sys/socket.h>

#define BUF_SIZE 100

char *host;
char *portnum;
char *schedalg;

char *currentfile;
char *file1;
char *file2;

pthread_barrier_t barrier;
pthread_mutex_t fifo_send_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t concur_file_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t recieve_mutex = PTHREAD_MUTEX_INITIALIZER;

// Get host information (used to establishConnection)
struct addrinfo *getHostInfo(char* host, char* port) {
  int r;
  struct addrinfo hints, *getaddrinfo_res;
  // Setup hints
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if ((r = getaddrinfo(host, port, &hints, &getaddrinfo_res))) {
    fprintf(stderr, "[getHostInfo:21:getaddrinfo] %s\n", gai_strerror(r));
    return NULL;
  }

  return getaddrinfo_res;
}

// Establish connection with host
int establishConnection(struct addrinfo *info) {
  if (info == NULL) return -1;

  int clientfd;
  for (;info != NULL; info = info->ai_next) {
    if ((clientfd = socket(info->ai_family,
                           info->ai_socktype,
                           info->ai_protocol)) < 0) {
      perror("[establishConnection:35:socket]");
      continue;
    }

    if (connect(clientfd, info->ai_addr, info->ai_addrlen) < 0) {
      close(clientfd);
      perror("[establishConnection:42:connect]");
      continue;
    }

    freeaddrinfo(info);
    return clientfd;
  }

  freeaddrinfo(info);
  return -1;
}

void run_with_error_checking(char *method_name, int error_code) {
  if (error_code == 0) return;
  if (strcmp(method_name, "pthread_barrier_wait") == 0) if (error_code == PTHREAD_BARRIER_SERIAL_THREAD) return;
  fprintf(stderr, "ERROR method: %s, error_code: %d check man pthread_create for description\n", method_name, error_code);
}

// Send GET request
void GET( char *null) {
  /**** Establish connection with <hostname>:<port> ***/
  int clientfd = establishConnection(getHostInfo(host, portnum));
  if (clientfd == -1) {
    fprintf(stderr,
            "[main:73] Failed to connect to: %s:%s \n",
            host, portnum);
    //return 3;
  }

  /*** SEND REQUEST ***/

  /* CONCUR
   * don't send the request until everyone has made their connections 
   *  so that everyone can do it simultaneusly
   */
  if(strcmp(schedalg, "CONCUR") == 0) {

    //the following block of code is to ensure that the threads alternate
    // between the two files, if two files are provided
    char *file;
    run_with_error_checking("pthread_mutex_lock", pthread_mutex_lock(&concur_file_mutex));
    file = file1;
    if(file2 != NULL) {
      file1 = file2;
      file2 = currentfile;
      currentfile = file1;
    }
    run_with_error_checking("pthread_mutex_unlock", pthread_mutex_unlock(&concur_file_mutex));

    //wait at barrier so that everyone sends requests at the same time
    run_with_error_checking("pthread_barrier_wait", pthread_barrier_wait(&barrier));

    //send
    char req[1000] = {0};
    sprintf(req, "GET %s HTTP/1.0\r\n\r\n", file);
    send(clientfd, req, strlen(req), 0);
  }

  /*
   * FIFO
   *use a mutex to make sure that only one thread can send a request at a time
    */
  if(strcmp(schedalg, "FIFO") == 0) {
    //set mutex
    run_with_error_checking("pthread_mutex_lock", pthread_mutex_lock(&fifo_send_mutex));

    //send
    char req[1000] = {0};
    sprintf(req, "GET %s HTTP/1.0\r\n\r\n", currentfile);
    send(clientfd, req, strlen(req), 0);
    if(file2 != NULL) {
      file1 = file2;
      file2 = currentfile;
      currentfile = file1;
    }

    //unlock the nutex so that the next thread can run.
    run_with_error_checking("pthread_mutex_unlock", pthread_mutex_unlock(&fifo_send_mutex));
  }

  /*
   * BOTH FIFO AND CONCUR 
   */
  /**** AWAIT RESULT ***/

  //don't accept responses until everyone has sent the get request
  run_with_error_checking("pthread_barrier_wait", pthread_barrier_wait(&barrier));

  //wait to recieve
  //The receipt is in a mutex to ensure that there is no interleaving between the printing of output
  // between multiple threads.
  char buf[BUF_SIZE];
  run_with_error_checking("pthread_mutex_lock", pthread_mutex_lock(&recieve_mutex));
  printf("\n%s%ld\n", "THREAD: ", pthread_self());
  while (recv(clientfd, buf, BUF_SIZE, 0) > 0) {
    fputs(buf, stdout);
    memset(buf, 0, BUF_SIZE);
  }
  close(clientfd);
  run_with_error_checking("pthread_mutex_unlock", pthread_mutex_unlock(&recieve_mutex));

  //don't move on until everyone has recieved their responses
  run_with_error_checking("pthread_barrier_wait", pthread_barrier_wait(&barrier));
}

void * loop(void *null) {
  while(1) GET(currentfile);
  return NULL;
}

int main(int argc, char **argv) {
  int numthreads;

  /***** TESTING FOR VALID INPUT *****/

  //test for correct number of arguments
  if (argc != 6 && argc != 7) {
    fprintf(stderr, "USAGE: ./httpclient <hostname> <port> <number threads> <schedalg> <request path> <op: reqpath2>\n");
    return 1;
  }
  //test for valid number of threads (input)
  if (atoi(argv[3]) < 1) {
    fprintf(stderr, "ERROR: Must specify a positive number of threads\n");
    return 1;
  }
  //test input for valid scheduling algorithm
  if (strcmp(argv[4], "CONCUR") != 0 && strcmp(argv[4], "FIFO") != 0) {
    fprintf(stderr, "%s\n", "ERROR: Must specify a valid sceduling algorithms, either \"CONCUR\" or \"FIFO\"");
    return 1;
  }

  /***** STORING ARGUMENTS IN VARIABLES ****/
  host = argv[1];
  portnum = argv[2];
  numthreads = atoi(argv[3]);
  schedalg = argv[4];
  file1 = argv[5];
  file2 = NULL;
  if(argc == 7) file2 = argv[6];
  currentfile = file1;

  //store the name of the second file in variable, NULL if no second file
  switch(argc) {
    case 6:
      file2 = NULL;
      break;
    case 7:
      file2 = argv[6];
      break;
    default:
      file2 = NULL;
  }

  /***** EXECUTE WITH SCHEDULING ALGORITHM ****/

  //get the barrier ready
  run_with_error_checking("pthread_barrier_init", pthread_barrier_init(&barrier, NULL, numthreads));
  
  //create (and run) the threads. The code they will be executing is in the loop() method
  pthread_t thread_id[numthreads];
  for (int i = 0; i < numthreads; i++) run_with_error_checking("pthread_create", pthread_create(&thread_id[i], NULL, loop, NULL));

  //don't exit until all of the threads finish (not going to happen...)
  for (int i = 0; i < numthreads; i++) run_with_error_checking("pthread_join" ,pthread_join(thread_id[i], NULL));
  return 0;
}