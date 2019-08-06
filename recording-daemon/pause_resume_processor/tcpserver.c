#include "tcpserver.h"
#include "log.h"
#include "epoll.h"

typedef struct tcpclient_s {
    int fd;
    handler_t handler;
} tcpclient_t;

#define INVALID_FD  (-1)

typedef struct tcpserver_s {
  in_addr_t     addr;    /* local IP or INADDR_ANY   */
  int           port;          /* local port to listen on  */
  int           fd;            /* listener descriptor      */
  int           clients_count;
  tcpclient_t * clients;     /* array of client descriptors */
  int           ticks;         /* uptime in seconds        */
  handler_t     handler;
} tcpserver_t; 


tcpserver_t tcpserver = {
  .addr = INADDR_ANY, /* by default, listen on all local IP's   */
  .fd = INVALID_FD
};

void close_client(tcpclient_t* pClient){
    if (pClient->fd != INVALID_FD) {
        epoll_del(pClient->fd);
        close(pClient->fd);
        pClient->fd = INVALID_FD;
    }
}


tcp_command_id_t check_command(char * buf, char ** arg) {

    int i = 0;
    // trim leading space
    while ( *buf == ' ' ) {
        buf ++;
    }

    for (; i < UNDEFINED_COMMAND ; ++i) {
        if (strlen(buf) >= strlen(ps_tcp_commands[i].command_str) && 
            strncmp(buf, ps_tcp_commands[i].command_str, strlen(ps_tcp_commands[i].command_str)) == 0) {
            break; 
        }
    }

    if (i == UNDEFINED_COMMAND) { 
        if (arg) *arg = NULL;
        return UNDEFINED_COMMAND;
    } 
    
    if (arg) {
        // find the command argument
        buf += strlen(ps_tcp_commands[i].command_str);
        // trim off leading space
        while ( *buf == ' ' ) {
            buf++;
        }
        if ( *buf == '\0') {
            // without argument
            *arg = NULL;
        } else {
            *arg = buf;
        }
    }
    
    return ps_tcp_commands[i].command_id;
}


void process_client(handler_t *handler){

    tcpclient_t* pClient = handler->ptr;
    if (pClient == NULL)
        return;
    char buf[2048];
    int rc = read(pClient->fd, buf, sizeof(buf));

    if (rc == 0){
        close_client(pClient);
        return;
    }
    else if (rc < 0){
        ilog(LOG_ERR,  "recv: %s\n", strerror(errno)); 
        return;
    }

    char * arg = NULL;

    int command = check_command(buf, &arg);
    ilog(LOG_INFO, "tcpserver got command %s : args : %s ", command == UNDEFINED_COMMAND ? "UNDEFINED_COMMAND" :  ps_tcp_commands[command].command_str ,
                                                            arg == NULL ? "NULL" : arg); 

    switch (command)
    {
        case STOP_RECORDING:
            dbg("====> stopRecording command: %s", buf);
            pause_stream( arg, ALL_CHANNELS);
            break;
        case START_RECORDING:
            dbg("====> startRecording command: %s", buf);
            resume_stream( arg, ALL_CHANNELS);
            break;
        case HEALTH_CHECK:
            dbg("====> healthCheck command: %s", buf);
            dbg("====> healthCheck TODO");      // TODO item
            break;
        default:
            dbg("====> not a valid command: %s", buf);    
            break;
    }
    
    return;
}

tcpclient_t * assign_client(int client_fd) {
    tcpclient_t * pclient = tcpserver.clients;
    for (int i=0; i< tcpserver.clients_count ; i++, pclient++ ) {
        if ( pclient->fd == INVALID_FD) {   // find an available client
            pclient->fd = client_fd;
            return pclient;
        } 
    }
    return NULL;
}
/* accept a new client connection to the listening socket */
void accept_client(handler_t *handler){

    int fd;
    struct sockaddr_in in;
    socklen_t sz = sizeof(in);
    tcpserver_t *pServer = handler->ptr;

    fd = accept(pServer->fd,(struct sockaddr*)&in, &sz);
    if (fd == INVALID_FD) {
        ilog(LOG_ERR, "tcpserver failed to accept connection: %s\n", strerror(errno)); 
        return;
    }

    dbg( "connection fd %d from %s:%d\n", fd,
        inet_ntoa(in.sin_addr), (int)ntohs(in.sin_port));

    tcpclient_t *pClient = assign_client (fd);
    if (pClient == NULL) {
        ilog(LOG_ERR, "Can't find a available client, consider increase the ps_max_clients in your config.");	
        close(fd);
        return;
    }
 
	if (epoll_add(fd, EPOLLIN, &pClient->handler)) {
        ilog(LOG_ERR, "[%d] tcpclient epoll_add error, Error:[%d:%s]",fd,  errno, strerror(errno));		
		close(fd);
		return ;
	}

    return ;
}

BOOL tcpserver_setup(int listen_port, int max_clients) {

    ilog(LOG_INFO, "Set up  tcpserver listening on %d with max clients : %d", listen_port, max_clients);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    tcpserver.fd = fd;

    if (fd == INVALID_FD) {
        ilog(LOG_ERR, "tcpserver create tcp server socket failed : %s\n", strerror(errno));
        return FALSE;
    }

    // initialize tcpserver
    tcpserver.port = listen_port;
    tcpserver.handler.ptr = &tcpserver;
	tcpserver.handler.func = accept_client;

    tcpserver.clients_count = max_clients;

    // initialize clients
    tcpserver.clients = malloc( max_clients * sizeof(tcpclient_t));
    tcpclient_t * pclient = tcpserver.clients;
    for (int i=0; i< max_clients; i++, pclient++ ) {
        pclient->fd = INVALID_FD; 
        pclient->handler.ptr = pclient;
        pclient->handler.func = process_client;
    }

    // Initialize connection
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = tcpserver.addr;
    sin.sin_port = htons(tcpserver.port);

    int one=1;
    if ( setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ) {
        ilog(LOG_ERR, "tcpserver setsockopt failed : %s\n", strerror(errno));
        goto failed;
    }

    if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) == -1) {
        ilog(LOG_ERR, "tcpserver bind failed : %s\n", strerror(errno));
        goto failed;
    }

/*
    int scocket_flag = fcntl(fd, F_GETFL, 0) | O_NONBLOCK;
    if ( fcntl(fd, F_SETFL, scocket_flag) ) {
        ilog(LOG_ERR, "set scoket to non-blocking mode failed: %s\n", strerror(errno));
        goto failed;
    }
    */

    if (listen(fd,1) == -1) {
       ilog(LOG_ERR, "listen: %s\n", strerror(errno));
        goto failed;
    }

	if (epoll_add(fd, EPOLLIN, &tcpserver.handler)) {
        ilog(LOG_ERR, "[%d] tcpserver epoll_add error, Error:[%d:%s]",fd,  errno, strerror(errno));	
        goto failed; 
	}

    return TRUE;

 failed:
         
    free(tcpserver.clients);
    tcpserver.clients = NULL;
    tcpserver.fd = INVALID_FD;
    close(fd);
    return FALSE;
}

void tcpserver_close(void){
    if (tcpserver.fd != INVALID_FD ) { 
        for (int i=0; i<tcpserver.clients_count; i++){
            close_client(&(tcpserver.clients[i]));
        }
        free(tcpserver.clients);
        close(tcpserver.fd); 
    }   
}


