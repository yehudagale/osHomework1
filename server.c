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
int NUM_THREADS;
int TBUFSIZE;
#define FIFO 0
#define HPIC 1
#define HPHC 2
#define TXT 0
#define IMG 1
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
	char type;
	int hit;
	int fd;
	char* request;
	time_t xStatReqArrivalTime;
	int dispatched_before;
	int arrived_after_picked_before;
}connec;
typedef struct connec_queue
{
	int front;
	int rear;
	connec * connections;
} connec_queue;
typedef struct connection_buffer{
	int num_dispatched;
	pthread_mutex_t buf_mutex;
	pthread_cond_t cond_w;
	pthread_cond_t cond_m;
	int how_full;
	int schedule_type;
	connec_queue high_priority_queue;
	connec_queue default_queue;
} connection_buffer;
connection_buffer cbuff;



typedef struct num_completed{
	int num_completed;
	pthread_mutex_t com_mutex;
}c_stat;

typedef struct thread_stats{
    int xStatThreadId;//The id of the responding thread (numbered 0 to number of threads-1)
    int xStatThreadCount;//The total number of http requests this thread has handled
    int xStatThreadHtml;//The total number of HTML requests this thread has handled
    int xStatThreadImage;//The total number of image requests this thread has handled
}t_stats;
c_stat completed;
t_stats * thread_data_array;
time_t start_time;
time_t get_time_milli()
{
	struct timeval tv;
	//used http://souptonuts.sourceforge.net/code/gettimeofday.c.html

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000; 
}
/**
 * Gets the type of scheduling process it will use
 * @param type
 * @return
 */
int get_schedule_type(char *type){
    int schedule_type;
    //?*?fprintf(stderr, "Type in schedule_type is: %s\n",type);
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
void init_buff(int schedule_type)
{
	cbuff.default_queue.front = 0;
	cbuff.default_queue.rear = 0;
	cbuff.default_queue.connections = malloc(sizeof(connec) * TBUFSIZE);
	cbuff.high_priority_queue.rear = 0;
	cbuff.high_priority_queue.front = 0;
	cbuff.high_priority_queue.connections = malloc(sizeof(connec) * TBUFSIZE);
	cbuff.num_dispatched = 0;
	cbuff.how_full = 0;
	completed.num_completed = 0;
	cbuff.schedule_type = schedule_type;
	pthread_mutex_init(&cbuff.buf_mutex, 0);
	pthread_cond_init(&cbuff.cond_m, 0);
	pthread_cond_init(&cbuff.cond_w, 0);
	pthread_mutex_init(&completed.com_mutex, 0);
}
//does not check for full or empty since that is already taken care of
connec deque(connec_queue * queue)
{
	connec to_ret = queue->connections[queue->front++];
	queue->front = queue->front % TBUFSIZE;
	return to_ret;
}
int is_empty(connec_queue * queue)
{
	return queue->front == queue->rear;
}
void enque(connec connection, connec_queue * queue)
{
	queue->connections[queue->rear++] = connection;
	queue->rear = queue->rear % TBUFSIZE;

}
int add_to(connection_buffer * buff, connec con_to_add)
{
	//?*?fprintf(stderr, "%s\n", "started add");
	pthread_mutex_lock(&buff->buf_mutex);
	//?*?fprintf(stderr, "%s%d%s%d\n", "locked mutex, buffer is: ", buff->how_full, " buffer size is: ", TBUFSIZE);
	while (buff->how_full >= TBUFSIZE) {
		pthread_cond_wait(&buff->cond_m, &buff->buf_mutex);
	}
	switch(buff->schedule_type){
		case FIFO :
			enque(con_to_add, &buff->default_queue);
		break;
		case HPIC:
			//?*?fprintf(stderr, "**************************************type is %d\n", con_to_add.type);
			if (con_to_add.type == IMG)
			{
				enque(con_to_add, &buff->high_priority_queue);
			}
			else{
				enque(con_to_add, &buff->default_queue);
			}
			break;
		case HPHC:
			if (con_to_add.type == TXT)
			{
				enque(con_to_add, &buff->high_priority_queue);
			}
			else{
				enque(con_to_add, &buff->default_queue);
			}
			break;
		default: //stack-based LIFO
			buff->default_queue.connections[buff->how_full] = con_to_add;
	}
	//?*?fprintf(stderr, "%s\n", "passed full check");
	buff->how_full++;
	pthread_cond_signal(&buff->cond_w);
	////?*?fprintf(stderr, "%s%d\n", "buffer after signal from master is: ", cbuff.how_full);
	pthread_mutex_unlock(&buff->buf_mutex);
	//?*?fprintf(stderr, "%s\n", "added to buffer");
	return 1;
}
connec get_con(connection_buffer * buff)
{
	pthread_mutex_lock(&buff->buf_mutex);
	while (buff->how_full <= 0){
		pthread_cond_wait(&buff->cond_w, &buff->buf_mutex);
	} 
	buff->how_full--;
	connec to_ret;
	//?*?fprintf(stderr, "schedule type is: %d\n", buff->schedule_type);
	switch(buff->schedule_type){
		case FIFO :
			to_ret = deque(&buff->default_queue);
			//no need to check for age since a queue always has age 0
		break;
		case HPIC:
		case HPHC:
			//?*?fprintf(stderr, "ran the HPIC/HPHC code and the queue is %d\n",is_empty(&buff->high_priority_queue));
			if (is_empty(&buff->high_priority_queue))
			{
				to_ret = deque(&buff->default_queue);
			}
			else{
				to_ret = deque(&buff->high_priority_queue);
				//?*?fprintf(stderr, "**ran the ageing code for %d \n", buff->schedule_type);
				for (int i = buff->default_queue.rear; i != buff->default_queue.front ; i = (i - 1) % TBUFSIZE)
				{
					if (buff->default_queue.connections[i].hit > to_ret.hit)
					{
						buff->default_queue.connections[i].arrived_after_picked_before++;
						//?*?fprintf(stderr, "**aged item %d by one\n", i);
					}
					else{
						break;
					}
				}
			}
			break;
		default: //stack-based LIFO
			to_ret = buff->default_queue.connections[buff->how_full];
			if (buff->how_full >= 0)
			{
				for (int i = buff->how_full - 1; i >= 0; i--)
				{
					buff->default_queue.connections[i].arrived_after_picked_before += 1;
				}
			}
	}
	to_ret.dispatched_before = buff->num_dispatched++;
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
void web(connec connection, t_stats t_data, time_t xStatReqDispatchTime)
{
	//?*?fprintf(stderr, "%s\n", "made it to web");
	int fd = connection.fd;
	int hit = connection.hit; 
	char* buffer = connection.request;
	char * fstr;
	int j, file_fd, buflen;
	long i, ret, len;
	//?*?fprintf(stderr, "connection is: %s\n", connection.request);
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


    *The arrival time of this request, as first seen by the master thread.
     * This time should be relative to the start time of the web server. */
    (void)sprintf(buffer,"X-stat-req-arrival-time: %ld\r\n", connection.xStatReqArrivalTime);
    dummy = write(fd,buffer,strlen(buffer));


    /*The time this request was dispatched (i.e., when the request was picked by a worker thread).
     * This time should be relative to the start time of the web server.*/
    (void)sprintf(buffer,"X-stat-req-dispatch-time: %ld\r\n", xStatReqDispatchTime);
    dummy = write(fd,buffer,strlen(buffer));



    /*The number of requests that were given priority over this request
     * (that is, the number of requests that arrived after this request arrived,
     * but were dispatched before this request was dispatched).*/
    (void)sprintf(buffer,"X-stat-req-age: %d\r\n", connection.arrived_after_picked_before);
    dummy = write(fd,buffer,strlen(buffer));

    /*The number of requests that arrived before this request arrived.
     *Note that this is a shared value across all of the threads.*/
    (void)sprintf(buffer,"X-stat-req-arrival-count: %d\r\n", connection.hit - 1);
    dummy = write(fd,buffer,strlen(buffer));
    /*The number of requests that were dispatched before this request was dispatched
     * (i.e., when the request was picked by a worker thread).
     * Note that this is a shared value across all of the threads. */
    (void)sprintf(buffer,"X-stat-req-dispatch-count: %d\r\n", connection.dispatched_before);
    dummy = write(fd,buffer,strlen(buffer));

    (void)sprintf(buffer,"X-stat-thread-id: %d\r\n", t_data.xStatThreadId);
    dummy = write(fd,buffer,strlen(buffer));
    (void)sprintf(buffer,"X-stat-thread-count: %d\r\n", t_data.xStatThreadCount);
    dummy = write(fd,buffer,strlen(buffer));
    (void)sprintf(buffer,"X-stat-thread-html: %d\r\n", t_data.xStatThreadHtml);
    dummy = write(fd,buffer,strlen(buffer));
    (void)sprintf(buffer,"X-stat-thread-image: %d\r\n", t_data.xStatThreadImage);
    dummy = write(fd,buffer,strlen(buffer));

    /*The number of requests that completed before this request completed;
     * we define completed as the point after the file has been read
     * and just before the worker thread starts writing the response on the socket.
     * Note that this is a shared value across all of the threads. */
    pthread_mutex_lock(&completed.com_mutex);
    (void)sprintf(buffer,"X-stat-req-complete-count: %d\r\n", completed.num_completed++);
    pthread_mutex_unlock(&completed.com_mutex);
    dummy = write(fd,buffer,strlen(buffer));
    /*The time at which the read of the file is complete and the worker thread begins writing the response on the socket.
     * This time should be relative to the start time of the web server. */
    (void)sprintf(buffer,"X-stat-req-complete-time: %ld\r\n", get_time_milli() - start_time);
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
    //?*?fprintf(stderr, "buffer after fix: %s\n", buffer);
    unsigned int buflen = strlen(buffer);
    for(i=0;extensions[i].ext != 0;i++) {
        len = strlen(extensions[i].ext);
        // //?*?fprintf(stderr, "buffer: %s %s\n", &buffer[buflen-len], extensions[i].ext);
        if(!strncmp(&buffer[buflen-len], extensions[i].ext, len)) {
            fstr =extensions[i].filetype;
            break;
        }
    }
    //?*?fprintf(stderr, "in get file type %s\n", fstr);
    return fstr;
}

void * be_a_worker(void * worker_cond)
{
	printf("%s\n", "created");
	t_stats *t_data = (t_stats*) worker_cond;
	//int t_id = t_data->xStatThreadId;
	//?*?fprintf(stderr, "Thread_id is:%d \n", t_id);
	int imgReq =0, htmlReq = 0;
	for(int req = 1 ; ; req++){
		connec connection = get_con(&cbuff);
		//?*?fprintf(stderr, "here is the request: %s\n", connection.request);
		time_t dispatch_time = get_time_milli() - start_time;
		if(connection.type == TXT){
		    t_data->xStatThreadHtml = ++htmlReq;
		    //?*?fprintf(stderr, "%d\n", htmlReq);
		    
		}else if (connection.type == IMG){
		    t_data->xStatThreadImage = ++imgReq;
		}
		// free(file_type);
		t_data->xStatThreadCount = req;
		web(connection, *t_data, dispatch_time);
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
		con_to_add.xStatReqArrivalTime = get_time_milli() - start_time;
		con_to_add.hit = hit;
		con_to_add.fd = socketfd; 
		con_to_add.arrived_after_picked_before = 0;
		//?*?fprintf(stderr, "%s\n", "about to read");
		con_to_add.request = read_req(socketfd);
		char file_type2[20];
		// con_to_add.request = malloc(sizeof(char) * (strlen(tmp) + 1));
		//strcpy(con_to_add.request, tmp);
		//?*?fprintf(stderr, "request before is: %s\n", con_to_add.request);
		char * file_type = get_file_type(con_to_add.request, file_type2);
		//?*?fprintf(stderr, "request after is: %s\n", con_to_add.request);
		if(strncmp(file_type,"text",4) == 0){
			con_to_add.type = TXT;
		    
		}else if (strncmp(file_type,"image",5) == 0){
			con_to_add.type = IMG;
		}
		else{
			con_to_add.type = 3;
		}
		//put request string
		add_to(&cbuff, con_to_add);
		//?*?fprintf(stderr, "%d\n", cbuff.how_full);
	}
	return 0;
}

void test(char ** argv)
{

	static connec_queue test_queue;
	connec test0;
	test0.hit = 0;
	test0.fd = 0;
	test0.request= "test0";
	test0.xStatReqArrivalTime = get_time_milli();
	test0.dispatched_before = 0;
	test0.arrived_after_picked_before = 0;
	connec test1;
	test1.hit = 0;
	test1.fd = 0;
	test1.request= "test1";
	test1.xStatReqArrivalTime = get_time_milli();
	test1.dispatched_before = 0;
	test1.arrived_after_picked_before = 0;
	connec test3;
	test3.hit = 0;
	test3.fd = 0;
	test3.request= "test3";
	test3.xStatReqArrivalTime = get_time_milli();
	test3.dispatched_before = 0;
	test3.arrived_after_picked_before = 0;

	enque(test0, &test_queue);
	enque(test1, &test_queue);
	fprintf(stderr, "%s\n", deque(&test_queue).request);
	enque(test3, &test_queue);
	fprintf(stderr, "%s\n", deque(&test_queue).request);
	fprintf(stderr, "%s\n", deque(&test_queue).request);

	exit(0);
}
int main(int argc, char **argv)
{
	//test(argv);
	int i;
	if( argc < 6  || argc > 6 || !strcmp(argv[1], "-?") ) {
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
	// 	return 0; // parent returns OK to shell 
	(void)signal(SIGCHLD, SIG_IGN); /* ignore child death */
	(void)signal(SIGHUP, SIG_IGN); /* ignore terminal hangups */
	// for(i=0;i<32;i++)
	// 	(void)close(i);		/* close open files */
	fprintf(stderr, "%s\n", "made it to 265");
	(void)setpgrp();		/* break away from process group */
	logger(LOG,"nweb starting",argv[1],getpid());
	/* setup the network socket */
	NUM_THREADS = atoi(argv[3]);
	TBUFSIZE = atoi(argv[4]);
	thread_data_array = calloc(NUM_THREADS, sizeof(t_stats));
	pthread_t * threads = calloc(NUM_THREADS, sizeof(pthread_t));
	init_buff(get_schedule_type(argv[5]));
	struct timeval tv;
	//used http://souptonuts.sourceforge.net/code/gettimeofday.c.html

	gettimeofday(&tv, NULL);
	start_time = get_time_milli();
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
 
