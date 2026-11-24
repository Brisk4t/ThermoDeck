/* One-shot SNTP client: asks an NTP server for the time and sets the system clock. */

#include "net/sntp.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "core/common.h"

/*
 * Asks an NTP server for the time (up to 3 tries) and sets the system clock.
 * Returns 0 on success. Answers older than the build date are ignored.
 */
int sntp_sync(const char *server)
{
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server, "123", &hints, &ai) != 0 || !ai) {
        tdlog("ntp: cannot resolve %s", server);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int rc = -1;
    if (fd >= 0) {
        unsigned char pkt[48] = {0};
        pkt[0] = 0x1b; /* LI=0, VN=3, Mode=3 (client) */
        for (int attempt = 0; attempt < 3 && rc; attempt++) {
            if (sendto(fd, pkt, sizeof pkt, 0, ai->ai_addr, ai->ai_addrlen) != sizeof pkt) break;
            struct pollfd pfd = {fd, POLLIN, 0};
            if (poll(&pfd, 1, 2500) <= 0) continue;
            unsigned char resp[48];
            if (recv(fd, resp, sizeof resp, 0) < 48) continue;
            /* transmit timestamp: seconds + fraction since 1900 */
            uint32_t secs =
                ((uint32_t)resp[40] << 24) | ((uint32_t)resp[41] << 16) | ((uint32_t)resp[42] << 8) | resp[43];
            uint32_t frac =
                ((uint32_t)resp[44] << 24) | ((uint32_t)resp[45] << 16) | ((uint32_t)resp[46] << 8) | resp[47];
            if (secs == 0) continue;
            struct timeval tv;
            tv.tv_sec = (time_t)(secs - 2208988800u);
            tv.tv_usec = (suseconds_t)(((uint64_t)frac * 1000000u) >> 32);
            if (tv.tv_sec < g_build_epoch) continue;
            if (settimeofday(&tv, NULL) == 0) {
                rc = 0;
                tdlog("ntp: clock set from %s", server);
            }
            else {
                tdlog("ntp: settimeofday failed: %s", strerror(errno));
                rc = -2;
            }
        }
        close(fd);
    }
    freeaddrinfo(ai);
    return rc;
}
