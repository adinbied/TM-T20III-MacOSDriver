/* simserver - TM-T20III simulator on TCP, for trying tmutil without a printer.
 *   build/simserver 19100 &   then   build/tmutil --tcp 127.0.0.1:19100 get
 * Serves connections one after another; state persists between them. */
#include "tmsim.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 19100;
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) {
        perror("simserver");
        return 1;
    }
    static tm_sim sim;
    long custom[256];
    memset(custom, 0, sizeof custom);
    custom[3] = 6; custom[5] = 3; custom[6] = 13; custom[101] = 1; custom[102] = 1;
    sim.msw[1] = 0x10;                                  /* auto LF on */
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0)
            continue;
        tm_sim_start(&sim, c);                         /* zeroes custom[] */
        memcpy(sim.custom, custom, sizeof custom);
        /* wait for the client to finish, then keep state */
        char b;
        while (recv(c, &b, 1, MSG_PEEK) > 0)
            usleep(20000);
        usleep(50000);
        memcpy(custom, sim.custom, sizeof custom);
        close(c);
    }
}
