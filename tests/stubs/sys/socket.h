#ifndef TEST_SOCKET_H
#define TEST_SOCKET_H
#include <sys/types.h>
typedef unsigned socklen_t;struct sockaddr{unsigned short family;char data[14];};
#define AF_INET 2
#define SOCK_DGRAM 2
#define MSG_DONTWAIT 0x80
int socket(int,int,int);
#endif
