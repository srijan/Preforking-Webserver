#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

#define LISTENQ 0
#define UNIXSOCKPATH "/tmp/webserver_unix.tmp"
#define MAXLINE 1024

pid_t *cpids;    // PIDs of children
int *cclients;   // Number of clients handled by each child
int *cactive;    // Busy / Free status for each child
int nchildren;   // Number of children
int maxn;        // Max n
int activecount; // Number of children active

ssize_t read_line(int,void*,size_t);
ssize_t write_n(int,const void*,size_t);
void print_server_status();
void sig_int(int);
void show_child_status(int);

int main(int argc, const char *argv[])
{
  int listenfd, i, j, k, status;
  const int on = 1;
  socklen_t addrlen;
  pid_t child_make(int, int, int, int);
  pid_t pid;
  int servPort, startServers, minSpareServers, maxSpareServers, maxClients, maxRequestsPerChild;
  struct sockaddr_in servAddr;

  int un_listenfd, un_connfd;
  pid_t un_childpid;
  socklen_t un_clilen;
  struct sockaddr_un un_cliaddr;
  struct sockaddr_un un_servaddr;

  int maxi, maxfd, nready, clients[FD_SETSIZE];
  ssize_t n;
  fd_set rset, allset;
  char buf[MAXLINE];

  if(argc < 7) {
    fprintf(stderr, "Usage:  %s <ServerPort> <StartServers> <MinSpareServers> <MaxSpareServers> <MaxClients> <MaxRequestsPerChild>\n", argv[0]);
    exit(1);
  }

  servPort = atoi(argv[1]);
  startServers = atoi(argv[2]);
  minSpareServers = atoi(argv[3]);
  maxSpareServers = atoi(argv[4]);
  maxClients = atoi(argv[5]);
  maxRequestsPerChild = atoi(argv[6]);

  // TCP Server Setup.
  memset(&servAddr, 0, sizeof(servAddr));
  servAddr.sin_family = AF_INET;
  servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servAddr.sin_port = htons(servPort);
  if ((listenfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    perror("socket");
    exit(1);
  }
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (bind(listenfd, (struct sockaddr *) &servAddr, sizeof(servAddr)) < 0) {
    perror("bind");
    exit(1);
  }
  if (listen(listenfd, LISTENQ) < 0) {
    perror("listen");
    exit(1);
  }

  // Unix Domain Socket Server Setup.
  if((un_listenfd = socket(AF_LOCAL, SOCK_STREAM, 0)) < 0) {
    perror("unix socket");
    exit(1);
  }
  unlink(UNIXSOCKPATH);
  bzero(&un_servaddr, sizeof(un_servaddr));
  un_servaddr.sun_family = AF_LOCAL;
  strcpy(un_servaddr.sun_path, UNIXSOCKPATH);
  if(bind(un_listenfd, (struct sockaddr *)&un_servaddr, sizeof(un_servaddr)) < 0) {
    perror("unix bind");
    exit(1);
  }
  if(listen(un_listenfd, LISTENQ) < 0) {
    perror("unix listen");
    exit(1);
  }

  // select() variables setup.
  maxfd = un_listenfd;
  maxi = -1;
  for(i=0;i<FD_SETSIZE;i++) {
    clients[i] = -1;
  }
  FD_ZERO(&allset);
  FD_SET(un_listenfd, &allset);

  // variable initialization for children
  nchildren = startServers;
  cpids = calloc(maxClients, sizeof(pid_t));
  cclients = calloc(maxClients, sizeof(int));
  cactive = calloc(maxClients, sizeof(int));
  activecount = 0;
  maxn = nchildren;

  // starting "startServers" number of servers in the beginning.
  for(i = 0; i < nchildren; i++) {
    cpids[i] = child_make(i, listenfd, addrlen, maxRequestsPerChild);
  }

  // main loop
  while(1) {
    rset = allset;
    while((nready = select(maxfd + 1, &rset, NULL, NULL, NULL)) == -1 && errno == EINTR) {
      continue;
    }

    if(FD_ISSET(un_listenfd, &rset)) {
      // new child connection to unix socket
      un_clilen = sizeof(un_cliaddr);
      if((un_connfd = accept(un_listenfd, (struct sockaddr *)&un_cliaddr, &un_clilen)) < 0) {
        perror("unix accept");
        exit(1);
      }
      for(i=0;i<FD_SETSIZE;i++) {
        if(clients[i] < 0) {
          clients[i] = un_connfd;
          break;
        }
      }
      FD_SET(un_connfd, &allset);
      if(un_connfd > maxfd) {
        maxfd = un_connfd;
      }
      if(i > maxi) {
        maxi = i;
      }
      if(--nready <= 0) {
        continue;
      }
    }
    for(i=0;i<=maxi;i++) {
      if((un_connfd = clients[i]) < 0) {
        continue;
      }
      if(FD_ISSET(un_connfd, &rset)) {
        // read data from child
        bzero(&buf, sizeof(buf));
        if((n = read_line(un_connfd, buf, MAXLINE)) == 0) {
          close(un_connfd);
          FD_CLR(un_connfd, &allset);
          clients[i] = -1;
        }
        j = atoi(buf);
        bzero(&buf, sizeof(buf));
        if((n = read_line(un_connfd, buf, MAXLINE)) == 0) {
          close(un_connfd);
          FD_CLR(un_connfd, &allset);
          clients[i] = -1;
        }
        // Recycles the child
        if(strcmp(buf,"ExitRecycle") == 0) {
          waitpid(cpids[j], &status, 0);
          print_server_status();
          printf("Recycling Child.\n\n");
          cpids[j] = child_make(j, listenfd, addrlen, maxRequestsPerChild);
          cclients[j] = 0;
          cactive[j] = 0;
        }
        // Removes the child
        else if(strcmp(buf,"ExitClean") == 0) {
          waitpid(cpids[j], &status, 0);
          nchildren--;
          cpids[j] = 0;
          cclients[j] = 0;
          cactive[j] = 0;
        }
        // Sets child status as busy
        else if(strcmp(buf,"SetBusy") == 0) {
          cclients[j]++;
          cactive[j] = 1;
          activecount++;
          while(((nchildren - activecount) < minSpareServers) && (nchildren < maxClients)) {
            print_server_status();
            printf("Starting new child as spare server.\n\n");
            // find next free index in cpids.
            if(maxn == nchildren) {
              k = nchildren;
              maxn++;
            }
            else {
              for(k=0;k<maxn;k++) {
                if(cpids[k] == 0) {
                  break;
                }
              }
            }
            cpids[k] = child_make(k, listenfd, addrlen, maxRequestsPerChild);
            nchildren++;
          }
        }
        // sets child status as free
        else if(strcmp(buf,"SetFree") == 0) {
          cactive[j] = 0;
          activecount--;
          if((nchildren - activecount) > maxSpareServers) {
            print_server_status();
            printf("Stopping a child because of extra spares.\n\n");
            write_n(un_connfd, "Exit\n", 5);
          }
          else {
            write_n(un_connfd, "Ok\n", 3);
          }
        }
        if (--nready <= 0) {
          break;
        }
      }
    }
  }
  return 0;
}

void show_child_status(int signo) {
  // shows child status
  int i;
  printf("===>\n");
  printf("Number of Children: %d\n", nchildren);
  printf("Number of active Children: %d\n", activecount);
  printf("Children Status:\n");
  printf("No.\tPID\tClients\tActive\n");
  for(i=0;i<maxn;i++) {
    if(cpids[i] != 0) {
      printf("%d.\t%d\t%d\t",i,cpids[i],cclients[i]);
      if(cactive[i] == 1) {
        printf("Yes");
      }
      else {
        printf("No");
      }
      printf("\n");
    }
  }
  printf("\n");
}

void print_server_status() {
  printf("=====>\nChanging process pool.\n");
  printf("Number of Children: %d\n", nchildren);
  printf("Number of clients being handled: %d\n", activecount);
}

pid_t child_make(int i, int listenfd, int addrlen, int maxRequestsPerChild) {
  // forks a new child
  signal(SIGINT, SIG_IGN);
  pid_t pid;
  void child_main(int, int, int, int);
  if( (pid = fork()) > 0) {
    signal(SIGINT, show_child_status);
    return pid;               /* parent */
  }
  child_main(i, listenfd, addrlen, maxRequestsPerChild);
  exit(0);
}

void child_main(int i, int listenfd, int addrlen, int maxRequestsPerChild) {
  // main child process logic
  int connfd;
  void send_msg_to_parent(int, int, const char*);
  void web_child(int);
  socklen_t clilen;
  struct sockaddr *cliaddr;
  int reqs;
  cliaddr = malloc(addrlen);
  pid_t pid;

  int un_sockfd;
  struct sockaddr_un un_servaddr;
  char buf[MAXLINE];

  // connect to parent via unix domain socket
  pid = getpid();
  un_sockfd = socket(AF_LOCAL, SOCK_STREAM, 0);
  bzero(&un_servaddr, sizeof(un_servaddr));
  un_servaddr.sun_family = AF_LOCAL;
  strcpy(un_servaddr.sun_path, UNIXSOCKPATH);
  connect(un_sockfd, (struct sockaddr *)&un_servaddr, sizeof(un_servaddr));

  printf("Child %ld started.\n", (long)pid);
  for(reqs = 0; reqs < maxRequestsPerChild; reqs++) {
    clilen = addrlen;
    if((connfd = accept(listenfd, cliaddr, &clilen)) < 0) {
      perror("accept");
      exit(1);
    }
    send_msg_to_parent(un_sockfd, i, "SetBusy");
    web_child(connfd);
    close(connfd);
    send_msg_to_parent(un_sockfd, i, "SetFree");
    bzero(&buf, sizeof(buf));
    read_line(un_sockfd, buf, MAXLINE);
    if(strcmp(buf,"Exit") == 0) {
      send_msg_to_parent(un_sockfd, i, "ExitClean");
      close(un_sockfd);
      printf("Child %ld exiting due to MaxSpareServers.\n", (long)pid);
      exit(0);
    }
  }
  send_msg_to_parent(un_sockfd, i, "ExitRecycle");
  close(un_sockfd);
  printf("Child %ld exiting due to MaxRequestsPerChild.\n", (long)pid);
}

void send_msg_to_parent(int fd, int i, const char *msg) {
  // sends a message to parent
  char buf[MAXLINE];
  bzero(&buf, sizeof(buf));
  snprintf(buf, MAXLINE, "%d\n%s\n", i, msg);
  int n = write_n(fd, buf, strlen(buf));
}

void web_child(int sockfd) {
  // main logic for sending data to client
  // right now, it sends dummy data
  char recvBuffer[10240];
  socklen_t clilen;
  struct sockaddr_in cliAddr;
  int cliPort;
  char cliIpStr[INET_ADDRSTRLEN];

  char *buf = "HTTP/1.1 200 OK\nServer: NetProg-Assignment-P2-Server-v0.1\nContent-Length: 38\nAccept-Ranges: bytes\nContent-Type: text/html\n\n<html>NetProg Assignment Server</html>";

  if(recv(sockfd, recvBuffer, sizeof(recvBuffer), 0) == 0) {
    return;        /* connection closed by other end */
  }
  clilen = sizeof(cliAddr);
  getpeername(sockfd, (struct sockaddr*)&cliAddr, &clilen);
  cliPort = ntohs(cliAddr.sin_port);
  inet_ntop(AF_INET, &cliAddr.sin_addr, cliIpStr, sizeof(cliIpStr));
  printf("Child %ld got request from %s:%d.\n", (long)getpid(), cliIpStr, cliPort);
  send(sockfd, buf, strlen(buf), 0);
  return;
}
ssize_t read_line(int fd, void *vptr, size_t maxlen) {
  ssize_t n, rc;
  char c, *ptr;
  ptr = vptr;
  for(n = 1; n < maxlen; n++) {
    if((rc = read(fd, &c, 1)) == 1) {
      if (c == '\n')
        break;        /* '\n' is not part of output */
      *ptr++ = c;
    }
    else if (rc == 0) {
      if (n == 1)
        return(0);
      else
        break;
    }
    else {
      if(errno == EINTR) {
        n--;
      }
      else {
        return(-1);
      }
    }
  }
  *ptr = 0;
  return(n);
}
ssize_t write_n(int fd, const void *vptr, size_t n) {
  size_t nleft;
  ssize_t nwritten;
  const char *ptr;
  ptr = vptr;
  nleft = n;
  while(nleft > 0) {
    if((nwritten = write(fd, ptr, nleft)) <= 0) {
      if (errno == EINTR) {
        nwritten = 0;
      }
      else {
        return(-1);
      }
    }
    nleft -= nwritten;
    ptr   += nwritten;
  }
  return(n);
}
