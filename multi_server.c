#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#define VERSION 23
#define BUFSIZE 8096
#define ERROR      42
#define LOG        44
#define FORBIDDEN 403
#define NOTFOUND  404
#define NUM_THREADS 1
#define TBUFSIZE 15

#define FIFO 0
#define HPIC 1
#define HPHC 2

struct {
	char *ext;
	char *filetype;
} extensions [] = {
	{"gif", "image/gif" },  
	{"jpg", "image/jpg" }, 
	{"jpeg","image/jpeg"},
	{"png", "image/png" },  
	{"ico", "image/ico" },  
	{"zip", "image/zip" },  
	{"gz",  "image/gz"  },  
	{"tar", "image/tar" },  
	{"htm", "text/html" },  
	{"html","text/html" },  
	{0,0} };

static int dummy; //keep compiler happy
typedef struct connec
{
	int hit;
	int fd;
	char* request;
}connec;
typedef struct connection_buffer{
	pthread_mutex_t buf_mutex;
	pthread_cond_t cond_w;
	pthread_cond_t cond_m;
	int how_full;
	int front_q;
    int rear_q;
	int schedule_type;
	connec connections[TBUFSIZE];
} connection_buffer;
connection_buffer cbuff;
/* Second queue 
typedef struct circularQueue_s{
    int front;
    int rear;
    int validItems;
    int data[MAX_ITEMS];
} circular_q;

void initializeQueue(circularQueue_t *theQueue)
{
    int i;
    theQueue->validItems  =  0;
    theQueue->front =  0;
    theQueue->rear  =  0;
    for(i=0; i < MAX_ITEMS; i++){
        theQueue->data[i] = 0;
    }        
    return;
}
*/

typedef struct master_stats{
    int xStatReqArrivalTime;
}m_stats;

typedef struct global_stats{
    int xStatReqDispatchCount;
    int xStatReqDispatchTime;
    int xStatReqCompleteCount;
}g_stats;


typedef struct thread_stats{
    int xStatThreadId;//The id of the responding thread (numbered 0 to number of threads-1)
    int xStatThreadCount;//The total number of http requests this thread has handled
    int xStatThreadHtml;//The total number of HTML requests this thread has handled
    int xStatThreadImage;//The total number of image requests this thread has handled
}t_stats;

m_stats master_info;
g_stats global_info;
t_stats thread_data_array[NUM_THREADS];
time_t start_time;

/**
 * Gets the type of scheduling process it will use
 * @param type
 * @return
 */
int get_schedule_type(char *type){
    int schedule_type;
    fprintf(stderr, "Type in schedule_type is: %s\n",type);
    if(strncmp(type,"FIFO",5) == 0){
        schedule_type = FIFO;
        fprintf(stderr, "FIFO is set\n");
    }else if(strncmp(type,"HPIC",5) == 0){
        schedule_type = HPIC;
    }else if(strcmp(type,"HPHC") == 0){
        schedule_type = HPHC;
    }else{
        schedule_type = -1;
        fprintf(stderr, "default is set\n");
    }
    return schedule_type;
}


void init_buff(char * schedule_type)
{
	cbuff.how_full = 0;
	cbuff.front_q = 0;
    cbuff.rear_q = 0;
	pthread_mutex_init(&cbuff.buf_mutex, 0);
	pthread_cond_init(&cbuff.cond_m, 0);
	pthread_cond_init(&cbuff.cond_w, 0);
	cbuff.schedule_type = get_schedule_type(schedule_type);
	fprintf(stderr, "S_type = %d\n",cbuff.schedule_type);

}

int add_to(connection_buffer * buff, connec con_to_add)
{
	fprintf(stderr, "%s\n", "started add_to buffer");
	pthread_mutex_lock(&buff->buf_mutex);
	fprintf(stderr, "%s%d%s%d\n", "locked mutex, buffer is: ", buff->how_full, " buffer size is: ", TBUFSIZE);
	while (buff->how_full >= TBUFSIZE) {
		pthread_cond_wait(&buff->cond_m, &buff->buf_mutex);
	}
	
	switch(buff->schedule_type){
		//http://www.xappsoftware.com/wordpress/2012/09/27/a-simple-implementation-of-a-circular-queue-in-c-language/
        case FIFO ://FIFO enque
            
            buff->rear_q = (buff->rear_q + 1)%(TBUFSIZE - 1);
            fprintf(stderr, "buff->front_q is %d\n",buff->front_q );
            fprintf(stderr, "buff->rear_q is %d\n",buff->rear_q );
            buff->connections[buff->rear_q] = con_to_add;

            fprintf(stderr, "%s\n", "Insertion success!!!");
            break;
        case HPIC ://HPIC still needs to be done, pics/JPG content goes first
            break;
        case HPHC ://HPHC still needs to be done, html pages goes first
            break;
        default ://stack based -LIFO
            buff->connections[buff->how_full] = con_to_add;
            fprintf(stderr, "LIFO is added\n");
            /*above 2 lines*/
    }

	fprintf(stderr, "%s\n", "passed full check");
	buff->how_full++;
	pthread_cond_signal(&buff->cond_w);
	fprintf(stderr, "%s%d\n", "buffer after signal from master is: ", cbuff.how_full);
	pthread_mutex_unlock(&buff->buf_mutex);
	fprintf(stderr, "%s\n", "added to buffer");
	return 1;
}


connec get_con(connection_buffer * buff)
{
	connec to_ret;	
	fprintf(stderr, "%s\n", "called get_con");
	pthread_mutex_lock(&buff->buf_mutex);
	fprintf(stderr, "%s\n", "locked mutex");
	while (buff->how_full <= 0){
		pthread_cond_wait(&buff->cond_w, &buff->buf_mutex);
		fprintf(stderr, "%s\n", "thread woken");
	} 
	buff->how_full--;
	fprintf(stderr, "buff->front_q is %d\n",buff->front_q );
	fprintf(stderr, "buff->rear_q is %d\n",buff->rear_q );
	//http://www.xappsoftware.com/wordpress/2012/09/27/a-simple-implementation-of-a-circular-queue-in-c-language/
    /*Which scheduler will be done*/
    switch(buff->schedule_type){
        case FIFO ://FIFO deque
            //remove value from queue and save it
            to_ret = buff->connections[buff->front_q];
            fprintf(stderr, "Removed connection from q\n");
            buff->front_q = (buff->front_q + 1)%(TBUFSIZE - 1);
            break;
        case HPIC ://HPIC
            break;
        case HPHC ://HPHC
            break;
        default ://stack based -LIFO
            to_ret = buff->connections[buff->how_full];
    }

	fprintf(stderr, "%s\n", "passed empty check");
	pthread_cond_signal(&buff->cond_m);
	pthread_mutex_unlock(&buff->buf_mutex);
	return to_ret;
}
void logger(int type, char *s1, char *s2, int socket_fd)
{
	int fd ;
	char logbuffer[BUFSIZE*2];

	switch (type) {
	case ERROR: (void)sprintf(logbuffer,"ERROR: %s:%s Errno=%d exiting pid=%d",s1, s2, errno,getpid()); 
		break;
	case FORBIDDEN: 
		dummy = write(socket_fd, "HTTP/1.1 403 Forbidden\nContent-Length: 185\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>403 Forbidden</title>\n</head><body>\n<h1>Forbidden</h1>\nThe requested URL, file type or operation is not allowed on this simple static file webserver.\n</body></html>\n",271);
		(void)sprintf(logbuffer,"FORBIDDEN: %s:%s",s1, s2); 
		break;
	case NOTFOUND: 
		dummy = write(socket_fd, "HTTP/1.1 404 Not Found\nContent-Length: 136\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>404 Not Found</title>\n</head><body>\n<h1>Not Found</h1>\nThe requested URL was not found on this server.\n</body></html>\n",224);
		(void)sprintf(logbuffer,"NOT FOUND: %s:%s",s1, s2); 
		break;
	case LOG: (void)sprintf(logbuffer," INFO: %s:%s:%d",s1, s2,socket_fd); break;
	}	
	/* No checks here, nothing can be done with a failure anyway */
	if((fd = open("nweb.log", O_CREAT| O_WRONLY | O_APPEND,0644)) >= 0) {
		dummy = write(fd,logbuffer,strlen(logbuffer)); 
		dummy = write(fd,"\n",1);      
		(void)close(fd);
	}
	if(type == ERROR || type == NOTFOUND || type == FORBIDDEN) exit(3);
}

/* this is a child web server process, so we can exit on errors */
void web(connec connection, t_stats t_data)
{
	fprintf(stderr, "%s\n", "made it to web");
	int fd = connection.fd;
	int hit = connection.hit; 
	char* buffer = connection.request;
	char * fstr;
	int j, file_fd, buflen;
	long i, ret, len;
	fprintf(stderr, "connection is: %s\n", connection.request);
	if( strncmp(buffer,"GET ",4) && strncmp(buffer,"get ",4) ) {
		logger(FORBIDDEN,"Only simple GET operation supported",buffer,fd);
	}
	for(i=4;i<BUFSIZE;i++) { /* null terminate after the second space to ignore extra stuff */
		if(buffer[i] == ' ') { /* string is "GET URL " +lots of other stuff */
			buffer[i] = 0;
			break;
		}
	}
	for(j=0;j<i-1;j++) 	/* check for illegal parent directory use .. */
		if(buffer[j] == '.' && buffer[j+1] == '.') {
			logger(FORBIDDEN,"Parent directory (..) path names not supported",buffer,fd);
		}
	if( !strncmp(&buffer[0],"GET /\0",6) || !strncmp(&buffer[0],"get /\0",6) ) /* convert no filename to index file */
		(void)strcpy(buffer,"GET /index.html");

	/* work out the file type and check we support it */
	buflen=strlen(buffer);
	fstr = (char *)0;
	for(i=0;extensions[i].ext != 0;i++) {
		len = strlen(extensions[i].ext);
		if( !strncmp(&buffer[buflen-len], extensions[i].ext, len)) {
			fstr =extensions[i].filetype;
			break;
		}
	}
	if(fstr == 0) logger(FORBIDDEN,"file extension type not supported",buffer,fd);

	if(( file_fd = open(&buffer[5],O_RDONLY)) == -1) {  /* open the file for reading */
		logger(NOTFOUND, "failed to open file",&buffer[5],fd);
	}
	logger(LOG,"SEND",&buffer[5],hit);
	len = (long)lseek(file_fd, (off_t)0, SEEK_END); /* lseek to the file end to find the length */
	      (void)lseek(file_fd, (off_t)0, SEEK_SET); /* lseek back to the file start ready for reading */
          (void)sprintf(buffer,"HTTP/1.1 200 OK\nServer: nweb/%d.0\nContent-Length: %ld\nConnection: close\nContent-Type: %s\n\n", VERSION, len, fstr); /* Header + a blank line */
	logger(LOG,"Header",buffer,hit);
	dummy = write(fd,buffer,strlen(buffer));
	//free(connection.request);
    /* Send the statistical headers described in the paper,below

    *The number of requests that arrived before this request arrived.
     *Note that this is a shared value across all of the threads.*
    (void)sprintf(buffer,"X-stat-req-arrival-count: %d\r\n", xStatReqArrivalCount);
	dummy = write(fd,buffer,strlen(buffer));

    *The arrival time of this request, as first seen by the master thread.
     * This time should be relative to the start time of the web server. *
    (void)sprintf(buffer,"X-stat-req-arrival-time: %d\r\n", xStatReqArrivalTime);
    dummy = write(fd,buffer,strlen(buffer));

    *The number of requests that were dispatched before this request was dispatched
     * (i.e., when the request was picked by a worker thread).
     * Note that this is a shared value across all of the threads. *
    (void)sprintf(buffer,"X-stat-req-dispatch-count: %d\r\n", xStatReqDispatchCount);
    dummy = write(fd,buffer,strlen(buffer));

    *The time this request was dispatched (i.e., when the request was picked by a worker thread).
     * This time should be relative to the start time of the web server.*
    (void)sprintf(buffer,"X-stat-req-dispatch-time: %d\r\n", xStatReqDispatchTime);
    dummy = write(fd,buffer,strlen(buffer));

    *The number of requests that completed before this request completed;
     * we define completed as the point after the file has been read
     * and just before the worker thread starts writing the response on the socket.
     * Note that this is a shared value across all of the threads. *
    (void)sprintf(buffer,"X-stat-req-complete-count: %d\r\n", xStatReqCompleteCount);
    dummy = write(fd,buffer,strlen(buffer));

    *The time at which the read of the file is complete and the worker thread begins writing the response on the socket.
     * This time should be relative to the start time of the web server. *
    (void)sprintf(buffer,"X-stat-req-complete-time: %d\r\n", xStatReqCompleteTime);
    dummy = write(fd,buffer,strlen(buffer));

    *The number of requests that were given priority over this request
     * (that is, the number of requests that arrived after this request arrived,
     * but were dispatched before this request was dispatched).*
    (void)sprintf(buffer,"X-stat-req-age: %d\r\n", xStatReqAge);
    dummy = write(fd,buffer,strlen(buffer));

    */
    
    (void)sprintf(buffer,"X-stat-req-arrival-count: %d\r\n", connection.hit - 1);
    dummy = write(fd,buffer,strlen(buffer));
    (void)sprintf(buffer,"X-stat-thread-id: %d\r\n", t_data.xStatThreadId);
    dummy = write(fd,buffer,strlen(buffer));
    (void)sprintf(buffer,"X-stat-thread-count: %d\r\n", t_data.xStatThreadCount);
    dummy = write(fd,buffer,strlen(buffer));
    (void)sprintf(buffer,"X-stat-thread-html: %d\r\n", t_data.xStatThreadHtml);
    dummy = write(fd,buffer,strlen(buffer));
    (void)sprintf(buffer,"X-stat-thread-image: %d\r\n", t_data.xStatThreadImage);
    dummy = write(fd,buffer,strlen(buffer));

    
    
    /* send file in 8KB block - last block may be smaller */
	while (	(ret = read(file_fd, buffer, BUFSIZE)) > 0 ) {
		dummy = write(fd,buffer,ret);
	}
	sleep(1);	/* allow socket to drain before signalling the socket is closed */
	close(fd);
	// exit(1);
}
char* get_file_type(char* buffer, char * fstr) {
    long i,len;
    for(i=4;i<BUFSIZE;i++) { /* null terminate after the second space to ignore extra stuff */
    	if(buffer[i] == ' ') { /* string is "GET URL " +lots of other stuff */
    		buffer[i] = 0;
    		break;
    	}
    }
    fprintf(stderr, "buffer after fix: %s\n", buffer);
    unsigned int buflen = strlen(buffer);
    for(i=0;extensions[i].ext != 0;i++) {
        len = strlen(extensions[i].ext);
        fprintf(stderr, "buffer: %s %s\n", &buffer[buflen-len], extensions[i].ext);
        if(!strncmp(&buffer[buflen-len], extensions[i].ext, len)) {
            fstr =extensions[i].filetype;
            break;
        }
    }
    fprintf(stderr, "in get file type %s\n", fstr);
    return fstr;
}

void * be_a_worker(void * worker_cond)
{
	printf("%s\n", "created");
	t_stats *t_data = (t_stats*) worker_cond;
	int t_id = t_data->xStatThreadId;
	fprintf(stderr, "Thread_id is:%d \n", t_id);
	int imgReq =0, htmlReq = 0;
	for(int req = 1 ; ; req++){
		connec connection = get_con(&cbuff);
		fprintf(stderr, "we seg fault on connection.request\n" );
		char* file_type = malloc(12* sizeof(char));
		file_type = get_file_type(connection.request, file_type);

		fprintf(stderr, "this is file type: %s\n", file_type);
		if(strncmp(file_type,"text",4) == 0){
		    t_data->xStatThreadHtml = ++htmlReq;
		    fprintf(stderr, "%d\n", htmlReq);
		    
		}else if (strncmp(file_type,"image",5) == 0){
		    t_data->xStatThreadImage = ++imgReq;
		}
		// free(file_type);
		t_data->xStatThreadCount = req;
		fprintf(stderr, "we never get here\n" );
		web(connection, *t_data);
	}
	return 0;
}

char* read_req(int fd){
	long ret;
	char* buffer = malloc(sizeof(char) * (BUFSIZE + 1)); 
	/* fill buffer with zeros */
    for(int i = 0; i< BUFSIZE;i++){
    	buffer[i] = 0;
    }
	
	ret =read(fd,buffer,BUFSIZE); 	/* read Web request in one go */
	if(ret == 0 || ret == -1) {	/* read failure stop now */
		logger(FORBIDDEN,"failed to read browser request","",fd);
	}
	if(ret > 0 && ret < BUFSIZE){	/* return code is valid chars */
		buffer[ret]=0;		/* terminate the buffer */
	}
	else{
		buffer[0]=0;
	}
	for(int i=0;i<ret;i++){	/* remove CF and LF characters */
		if(buffer[i] == '\r' || buffer[i] == '\n'){
			buffer[i]='*';
		}
	}
	return buffer;
	
}

void * be_a_master(void * master_cond)
{
	char ** argv = (char ** ) master_cond;
	static struct sockaddr_in cli_addr; /* static = initialised to zeros */
	static struct sockaddr_in serv_addr; /* static = initialised to zeros */
	int port, listenfd, socketfd, hit;
	socklen_t length;
	if((listenfd = socket(AF_INET, SOCK_STREAM,0)) <0)
		logger(ERROR, "system call","socket",0);
	port = atoi(argv[1]);
	if(port < 0 || port >60000)
		logger(ERROR,"Invalid port number (try 1->60000)",argv[1],0);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);
	if(bind(listenfd, (struct sockaddr *)&serv_addr,sizeof(serv_addr)) <0)
		logger(ERROR,"system call","bind",0);
	if( listen(listenfd,64) <0)
		logger(ERROR,"system call","listen",0);
	for(hit=1;;hit++) {
		length = sizeof(cli_addr);
		if((socketfd = accept(listenfd, (struct sockaddr *)&cli_addr, &length)) < 0)
			logger(ERROR,"system call","accept",0);
		// if((pid = fork()) < 0) {
		// 	logger(ERROR,"system call","fork",0);
		// }
		// else {
			// if(pid == 0) { 	/* child */
			// 	(void)close(listenfd);
			// 	web(socketfd,hit);  never returns 
			// } else { 	/* parent */
			// 	(void)close(socketfd);
			// }
		connec con_to_add;
		con_to_add.hit = hit;
		con_to_add.fd = socketfd; 
		//put request string
		fprintf(stderr, "%s\n", "about to read");
		con_to_add.request = read_req(socketfd);
		fprintf(stderr, "request is: %s\n", con_to_add.request);
		add_to(&cbuff, con_to_add);
		fprintf(stderr, "%d\n", cbuff.how_full);
	}
	return 0;
}


int main(int argc, char **argv)
{

	int i;
	if( argc < 5  || argc > 5 || !strcmp(argv[1], "-?") ) {
		(void)printf("hint: nweb Port-Number Top-Directory\t\tversion %d\n\n"
	"\tnweb is a small and very safe mini web server\n"
	"\tnweb only servers out file/web pages with extensions named below\n"
	"\t and only from the named directory or its sub-directories.\n"
	"\tThere is no fancy features = safe and secure.\n\n"
	"\tExample: nweb 8181 /home/nwebdir &\n\n"
	"\tOnly Supports:", VERSION);
		for(i=0;extensions[i].ext != 0;i++)
			(void)printf(" %s",extensions[i].ext);

		(void)printf("\n\tNot Supported: URLs including \"..\", Java, Javascript, CGI\n"
	"\tNot Supported: directories / /etc /bin /lib /tmp /usr /dev /sbin \n"
	"\tNo warranty given or implied\n\tNigel Griffiths nag@uk.ibm.com\n"  );
		exit(0);
	}
	if( !strncmp(argv[2],"/"   ,2 ) || !strncmp(argv[2],"/etc", 5 ) ||
	    !strncmp(argv[2],"/bin",5 ) || !strncmp(argv[2],"/lib", 5 ) ||
	    !strncmp(argv[2],"/tmp",5 ) || !strncmp(argv[2],"/usr", 5 ) ||
	    !strncmp(argv[2],"/dev",5 ) || !strncmp(argv[2],"/sbin",6) ){
		(void)printf("ERROR: Bad top directory %s, see nweb -?\n",argv[2]);
		exit(3);
	}
	if(chdir(argv[2]) == -1){ 
		(void)printf("ERROR: Can't Change to directory %s\n",argv[2]);
		exit(4);
	}
	/* Become deamon + unstopable and no zombies children (= no wait()) */
	// if(fork() != 0)
	// 	return 0;  parent returns OK to shell 
	(void)signal(SIGCHLD, SIG_IGN); /* ignore child death */
	(void)signal(SIGHUP, SIG_IGN); /* ignore terminal hangups */
	// for(i=0;i<32;i++)
	// 	(void)close(i);		/* close open files */
	fprintf(stderr, "%s\n", "made it to 265");
	(void)setpgrp();		/* break away from process group */
	logger(LOG,"nweb starting",argv[1],getpid());
	/* setup the network socket */
	pthread_t threads[NUM_THREADS];
	init_buff(argv[4]);
	struct timeval tv;
	//used http://souptonuts.sourceforge.net/code/gettimeofday.c.html

	gettimeofday(&tv, NULL);
	start_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	fprintf(stderr, "start time: %ld\n", start_time);
	
	for (int i = 0; i < NUM_THREADS; i++)
	{
		thread_data_array[i].xStatThreadId = i;
		pthread_create(&threads[i], NULL, be_a_worker, &thread_data_array[i]);
	}
	// sleep(1);
	be_a_master(argv);
	fprintf(stderr, "%s\n", "made it to end");
}
 
