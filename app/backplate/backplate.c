/*
 * Backplate safety: commands every HVAC relay off when ThermoDeck starts.
 *
 * The stock software (libcore DevMgrRootDeviceTstat::DirectRelayShutoff) does the same
 * thing in an emergency: "Set Grouped Output" with an empty mask, written twice 75 ms
 * apart, no ACK requested. After that ThermoDeck sends nothing more. The backplate firmware
 * switches every relay off by itself whenever the host has been silent for a few seconds,
 * and it rejects relay commands until it gets a relay configuration, which ThermoDeck
 * never sends. See BACKPLATE.md.
 */

#include "backplate/backplate.h"

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "core/common.h"

uint16_t backplate_crc(const uint8_t *data, size_t len)
{
    /* reflected 0x1021, init 0xFFFF, final xor 0xFFFF */
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return (uint16_t)(c ^ 0xFFFF);
}

int backplate_frame(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out, size_t outlen)
{
    size_t total = len + 6;
    if (total > 128 || total > outlen) return -1;
    out[0] = 0xF2;
    out[1] = 0x86;
    out[2] = (uint8_t)(len + 3); /* type + payload + crc */
    out[3] = type;
    if (len) memcpy(out + 4, payload, len);
    uint16_t crc = backplate_crc(out + 3, len + 1);
    out[4 + len] = (uint8_t)(crc >> 8);
    out[5 + len] = (uint8_t)crc;
    return (int)total;
}

/* 19200 8N1, raw, no flow control: what the stock HAL_UART sets for this port. */
int backplate_open(const char *dev)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        t.c_cflag = (t.c_cflag & ~(CSIZE | PARENB | CSTOPB | CRTSCTS)) | CS8 | CREAD | CLOCAL;
        cfsetispeed(&t, B19200);
        cfsetospeed(&t, B19200);
        tcsetattr(fd, TCSANOW, &t);
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

int backplate_relays_off(const char *dev)
{
    if (!dev) dev = BACKPLATE_DEV;
    int fd = backplate_open(dev);
    if (fd < 0) {
        tdlog("backplate: can't open %s (%s), relays not commanded", dev, strerror(errno));
        return -1;
    }
    const uint8_t mask[2] = {0, 0};
    uint8_t fr[16];
    int n = backplate_frame(BP_TX_GROUPED_OUTPUT, mask, sizeof mask, fr, sizeof fr);

    /* twice, like the stock emergency shutoff */
    int sent = 0;
    for (int i = 0; i < 2; i++) {
        if (write(fd, fr, (size_t)n) == n) sent++;
        tcdrain(fd);
        usleep(75000);
    }
    close(fd);
    tdlog("backplate: all relays off sent (%d/2) on %s", sent, dev);
    return sent ? 0 : -1;
}
