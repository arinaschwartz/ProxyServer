/*
 * proxy.c - CS 154 Project 5 Proxy Server
 *
 * TEAM MEMBERS:
 *     Rebecca Hoberg, rahoberg@uchicago.edu
 *     Arin Schwartz, arinschwartz@uchicago.edu
 * 
 * Description:
   Semaphores: logSem: used to handle logging.
               copySem: handles lock-and-copy.
   
   The main() routine initializes the semaphores and enters infinite loop that establishes connection with client and creates threads that call the thread routine.
    
   High-level functions:
   
   clientToServer(): takes a client file descriptor and a uri. Parses uri, initializes all relevant information, and establishes connection to the server, returning the server file descriptor.
   
   serverToClient(): Writes content from server to client.
   
   thread(): The main thread routine. Calls clientToServer() and serverToClient(), logs all activity appropriately.
   
   Open_clientfd_ts(): Thread-safe version of Open_clientfd(), utilizes semaphores.
   
   
 */ 

#include "csapp.h"

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, int  *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size);
void thread(void** threadargs);
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen);
void Rio_writen_w(int fd, void *usrbuf, size_t n);
ssize_t Rio_readn_w(int fd, void *ptr, size_t nbytes);
void read_requesthdrs(rio_t *rp);
int open_clientfd_ts(char *hostname, int port);

/*Declare semaphores*/
static sem_t logSem;
static sem_t copySem;


/* open_clientfd - open connection to server at <hostname, port>                 
 *   and return a socket descriptor ready for reading and writing.               
 *   Returns -1 and sets errno on Unix error.                                    
 *   Returns -2 and sets h_errno on DNS (gethostbyname) error.                   
 */
  
int open_clientfd_ts(char *hostname, int port){
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;

    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1; /* check errno for cause of error */

    /* Fill in the server's IP address and port */
    P(&copySem);
    if ((hp = gethostbyname(hostname)) == NULL)
        return -2; /* check h_errno for cause of error */
    struct hostent* localhp = Malloc(sizeof(struct hostent));
    localhp = hp;
    V(&copySem);
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)localhp->h_addr_list[0],
	    (char *)&serveraddr.sin_addr.s_addr, localhp->h_length);
    serveraddr.sin_port = htons(port);
    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
        return -1;
    return clientfd;
}

/*Wrapper for client routine*/
int Open_clientfd_ts(char *hostname, int port) 
{
    int rc;

    if ((rc = open_clientfd_ts(hostname, port)) < 0) {
	if (rc == -1)
	    unix_error("Open_clientfd Unix error");
	else        
	    dns_error("Open_clientfd DNS error");
    }
    return rc;
}

/* 
 * main - Main routine for the proxy program 
 */
int main(int argc, char **argv)
{
  unsigned int listenfd, port, clientlen;
  struct sockaddr_in clientaddr;
  pthread_t tid;

  /* Check arguments */
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
    exit(0);
  }
  port=atoi(argv[1]);
  listenfd=Open_listenfd(port);
  FILE* logfile=fopen("proxy.log","w");
  fclose(logfile);
  int lfd=open("proxy.log", O_RDWR|O_APPEND);
  clientlen=sizeof(clientaddr);
  
  /*Initialize semaphores for logging, and lock-and-copy*/

  sem_init(&logSem, 0, 1);
  sem_init(&copySem, 0, 1);
  
  //infinite loop where the work of accepting connections as they come
  while(1){
    int *clientfd = Malloc(sizeof(int));
    *clientfd = Accept(listenfd, (SA*)&clientaddr, &clientlen);
    void** threadargs = Malloc(sizeof(void*)*3);
    threadargs[0] = clientfd;
    threadargs[1] = &clientaddr;
    threadargs[2] = &lfd;
    Pthread_create(&tid, NULL, (void*)thread, threadargs);
  }
  close(lfd);
  exit(0);
}


/*
 * parse_uri - URI parser
 * 
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, int *port)
{
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
	hostname[0] = '\0';
	return -1;
    }
       
    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';
    
    /* Extract the port number */
    *port = 80; /* default */
    if (*hostend == ':')   
	*port = atoi(hostend + 1);
    
    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
	pathname[0] = '\0';
    }
    else {
	pathbegin++;	
	strcpy(pathname, pathbegin);
    }

    return 0;
}

/*Parses URI for host and relevant info, then handles rest of client-to-server transfers*/
int clientToServer(int clientfd, char* uri){
  rio_t* rio1 = Malloc(sizeof(rio_t));
  Rio_readinitb(rio1, clientfd);
  char* hostname=Malloc(MAXLINE);
  char*pathname=Malloc(MAXLINE);
  int port;
  char* buf=Malloc(MAXLINE);
  char* version = Malloc(MAXLINE);
  
  int buflen=Rio_readlineb_w(rio1, buf, MAXLINE);
  
  sscanf(buf, "GET %s %s",uri, version);
  int len=strlen(uri);
  uri[len]='/';
  if(parse_uri(uri, hostname, pathname, &port)<0){
    Rio_writen_w(clientfd,"Not a valid URL\n",16);
    return -1;
  }
  
  int serverfd=Open_clientfd_ts(hostname,port);
  if(serverfd<0){
    Rio_writen_w(clientfd,"Open_clientfd error.\n", 21);
    return -1;
  }
  Free(hostname);
  Free(pathname);
  Free(version);
  Rio_writen_w(serverfd,buf,buflen);
  /*Writes all subsequent lines to server*/
  while(strcmp(buf, "\r\n") != 0){
    buflen = Rio_readlineb_w(rio1, buf, MAXLINE);
    Rio_writen_w(serverfd, buf, buflen);
  }
  
  Free(rio1);
  Free(buf);
  return serverfd;
}

/*Handles transfers from server back to client*/
int serverToClient(int serverfd, int clientfd){

  void* buf2=Malloc(MAXLINE);
  int size=0;
  int n = 1;
  while(n > 0){
    n=Rio_readn_w(serverfd,buf2,MAXLINE);
    
    Rio_writen_w(clientfd,buf2,n);
    size+=n;
  }
  Free(buf2);
  return size;
}

/*Main thread routine*/
void thread(void** threadargs){
  int clientfd = *(int*)threadargs[0];
  Pthread_detach(pthread_self());
  struct sockaddr_in* clientaddr = threadargs[1];
  int lfd = *(int*)threadargs[2];
  char* uri=Malloc(MAXLINE);

  int serverfd = clientToServer(clientfd, uri);
  if(serverfd < 0){
    Close(clientfd);
    return;
  }
  
  int size = serverToClient(serverfd, clientfd);
  
  /*Logging handled here*/
  P(&logSem);
  char* logstring=Malloc(MAXLINE);
  format_log_entry(logstring, clientaddr, uri, size);
  Rio_writen_w(lfd,logstring,strlen(logstring));
  V(&logSem);
  
  Free(threadargs);
  Free(uri);
  Free(logstring);
  Close(clientfd);
}

/*
 * format_log_entry - Create a formatted log entry in logstring. 
 * 
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size in bytes
 * of the response from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, 
		      char *uri, int size)
{
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /* 
     * Convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 13, CS:APP).
     */
    host = ntohl(sockaddr->sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;


    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %d.%d.%d.%d %s %d\n", time_str, a, b, c, d, uri, size);
}

/*Wrapper for rio function, warnings included*/
void Rio_writen_w(int fd, void *usrbuf, size_t n)
{
  if (rio_writen(fd, usrbuf, n) != n)
    printf("Warning: Rio_writen error");
}

ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen)
{
  ssize_t rc;

  if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0)
    printf("Warning: Rio_readlineb error");
  return rc;
}

ssize_t Rio_readn_w(int fd, void *ptr, size_t nbytes)
{
  ssize_t n;

  if ((n = rio_readn(fd, ptr, nbytes)) < 0)
    printf("Warning: Rio_readn error");
  return n;
}

void read_requesthdrs(rio_t *rp){
  char buf[MAXLINE];
  Rio_readlineb_w(rp, buf, MAXLINE);
  while(strcmp(buf, "\r\n")){
    Rio_readlineb_w(rp,buf,MAXLINE);
  }
  return;
}

