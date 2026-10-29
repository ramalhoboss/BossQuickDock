#include <stdint.h>
struct in_addr{uint32_t s_addr;};struct sockaddr_in{unsigned short sin_family,sin_port;struct in_addr sin_addr;unsigned char pad[8];};static unsigned short htons(unsigned short v){return (unsigned short)((v<<8)|(v>>8));}
