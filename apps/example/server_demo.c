#include  <unistd.h>
#include  <sys/types.h>       /* basic system data types */
#include  <sys/socket.h>      /* basic socket definitions */
#include  <netinet/in.h>      /* sockaddr_in{} and other Internet defns */
#include  <arpa/inet.h>       /* inet(3) functions */
#include <sys/epoll.h> /* epoll function */
#include <fcntl.h>     /* nonblocking */
#include <sys/resource.h> /*setrlimit */

#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>


#define MAXEPOLLSIZE 30000
#define MAXLINE 1024
static char *conf_file = "epserver.conf";
static int core_limit;

struct thread_context
{
    mctx_t mctx;
    int ep;
  struct server_vars *svars;
};


struct thread_context *ctx;
mctx_t mctx;

int handle(int connfd);


int main(int argc, char **argv)
{
    int  servPort = 80;
    int listenq = 4096;
    
    int listenfd, connfd, kdpfd, nfds, n, curfds,acceptCount;
    struct sockaddr_in servaddr, cliaddr;
    socklen_t socklen = sizeof(struct sockaddr_in);
    struct mtcp_epoll_event ev;
    struct mtcp_epoll_event *events;
    //struct mtcp_epoll_event *mevents;
    struct rlimit rt;
    char buf[MAXLINE];
    acceptCount = 0;
    
    struct mtcp_conf mcfg;
    int ret;
    //struct mtcp_epoll_event ev;
    //struct epoll_event evv;
    core_limit=1;
    //conf_file = "epserver.conf";
    mtcp_getconf(&mcfg);
    mcfg.num_cores = core_limit;
    mtcp_setconf(&mcfg);
    printf("%s*******",conf_file);
    ret = mtcp_init(conf_file);
    if(ret){
        printf("init fail!");
        return 0;
    }
    mtcp_getconf(&mcfg);
    
    mtcp_core_affinitize(0);
    ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
    ctx->mctx = mtcp_create_context(0);
    
    /*--------------------------------*/

    /* 设置每个进程允许打开的最大文件数 */
    rt.rlim_max = rt.rlim_cur = MAXEPOLLSIZE;
    if (setrlimit(RLIMIT_NOFILE, &rt) == -1)
    {
        printf("setrlimit error");
        return -1;
    }
    
    kdpfd = mtcp_epoll_create(ctx->mctx,MAXEPOLLSIZE);
    printf("%d\n",kdpfd);
    
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl (INADDR_ANY);
    servaddr.sin_port = htons (servPort);
    
    listenfd = mtcp_socket(ctx->mctx,AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        perror("can't create socket file");
        return -1;
    }
    printf("listenfd=%d\n",listenfd);
    
    int opt = 1;
    
    mtcp_setsockopt(ctx->mctx,listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (mtcp_setsock_nonblock(ctx->mctx,listenfd) < 0) {
        perror("setnonblock error");
    }
    
    if (mtcp_bind(ctx->mctx,listenfd, (struct sockaddr *) &servaddr, sizeof(struct sockaddr)) == -1)
    {
        perror("bind error");
        return -1;
    }
    
    if (mtcp_listen(ctx->mctx,listenfd, listenq) == -1)
    {
        perror("listen error");
        return -1;
    }
    /* 创建 epoll 句柄，把监听 socket 加入到 epoll 集合里 */
    
    ev.events = EPOLLIN;
    ev.data.sockid = listenfd;
    if (mtcp_epoll_ctl(ctx->mctx,kdpfd, EPOLL_CTL_ADD, listenfd, &ev) < 0)
    {
        fprintf(stderr, "epoll set insertion error: fd=%d\n", listenfd);
        return -1;
    }
    curfds = 1;
    
    printf("epollserver startup,port %d, max connection is %d, backlog is %d\n", servPort, MAXEPOLLSIZE, listenq);
    
    events = (struct mtcp_epoll_event *) calloc(MAXEPOLLSIZE, sizeof(struct mtcp_epoll_event));
    //mevents = (struct mtcp_epoll_event *) calloc(MAXEPOLLSIZE, sizeof(struct mtcp_epoll_event));
    
    while(1) {
        /* 等待有事件发生 */
        nfds = mtcp_epoll_wait(ctx->mctx,kdpfd, events, MAXEPOLLSIZE, -1);
        if (nfds < 0) {
            if (errno != EINTR)
                perror("mtcp_epoll_wait");
            break;
        }
        /* 处理所有事件 */
        for (n = 0; n < nfds; ++n)
        {
            //events[n].events = mevents[n].events;
            //events[n].data.fd = mevents[n].data.sockid;
            if (events[n].data.sockid == listenfd)
            {
                connfd = mtcp_accept(ctx->mctx,listenfd, (struct sockaddr *)&cliaddr,&socklen);
                if (connfd < 0)
                {
                    perror("accept error");
                    continue;
                }
                
                sprintf(buf, "accept form %s:%d\n", inet_ntoa(cliaddr.sin_addr), cliaddr.sin_port);
                printf("%d:%s", ++acceptCount, buf);
                
                if (curfds >= MAXEPOLLSIZE) {
                    fprintf(stderr, "too many connection, more than %d\n", MAXEPOLLSIZE);
                    mtcp_close(ctx->mctx,connfd);
                    continue;
                }
                if (mtcp_setsock_nonblock(ctx->mctx,connfd) < 0) {
                    perror("setnonblocking error");
                }
                ev.events = EPOLLIN;
                ev.data.sockid = connfd;
                
                if (mtcp_epoll_ctl(ctx->mctx,kdpfd, EPOLL_CTL_ADD, connfd, &ev) < 0)
                {
                    fprintf(stderr, "add socket '%d' to epoll failed: %s\n", connfd, strerror(errno));
                    return -1;
                }
                curfds++;
                continue;
            }
            // 处理客户端请求
            if (handle(events[n].data.sockid) < 0) {
                mtcp_epoll_ctl(ctx->mctx,kdpfd, EPOLL_CTL_DEL, events[n].data.sockid,&ev);
                curfds--;   
            }
        }
    }
    mtcp_close(ctx->mctx,listenfd);
    mtcp_destroy_context(mctx);
    return 0;
}
int handle(int connfd) {
    int nread,nwrite;
    char buf[MAXLINE];
    nread = mtcp_read(ctx->mctx,connfd, buf, MAXLINE);//读取客户端socket流
    
    if (nread == 0) {
        printf("client close the connection\n");
        mtcp_close(ctx->mctx,connfd);
        return -1;
    }
    if (nread < 0) {
        perror("read error");
        mtcp_close(ctx->mctx,connfd);
        return -1;
    }
    nwrite = mtcp_write(ctx->mctx,connfd, buf, nread);//响应客户端
    if(nwrite<0)
    {
        perror("write error");
    }
    return 0;
}
