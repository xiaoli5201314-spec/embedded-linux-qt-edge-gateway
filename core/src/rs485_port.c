/*
 * rs485_port.c - poll() based half duplex RS485 port.
 *
 * Design notes
 * ------------
 *  - The DE (driver enable) line is held asserted from the first queued byte
 *    until rs485_tx_complete() reports that the UART transmit shift register is
 *    empty. On Linux that is the TIOCOUTQ count plus tcdrain(), the portable
 *    equivalent of polling the TC status bit.
 *  - A configurable guard time (de_guard_us) is added after the shift register
 *    empties, because the transceiver itself needs a few bit times to release
 *    the differential pair. Without it the slave sees a truncated stop bit.
 *  - Everything is poll() driven so the caller can interleave several ports in
 *    one acquisition loop.
 *
 * The file descriptor path is shared between the target and the host tests:
 * rs485_open_fd() lets the tests drive the port over a socketpair.
 */

/*
 * cfmakeraw(), usleep() and the TIOCOUTQ request macro are exposed by glibc
 * only when a feature test macro asks for them; -std=c99 alone hides them.
 * The macro must be defined before the first system header is pulled in.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "rs485_port.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>

/* usleep() needs _DEFAULT_SOURCE as well, which is set above. A short helper
 * keeps the call sites readable and the time unit explicit. */
static void sleep_us(unsigned long us)
{
    if (us > 0ul) {
        (void)usleep((useconds_t)us);
    }
}

struct rs485_port {
    int      fd;
    int      owns_fd;      /* 1 when the port opened the fd itself */
    uint8_t  de_asserted;
    uint32_t de_guard_us;
    int      de_gpio;      /* -1 when the kernel handles the direction */
};

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return ((uint64_t)tv.tv_sec * 1000000ull) + (uint64_t)tv.tv_usec;
}

/* The DE line is a GPIO on the target. The tiny hook is kept behind a function
 * so the host build links without libgpiod while the target build can provide
 * the real implementation. */
__attribute__((weak)) void rs485_de_set(int gpio, int level)
{
    (void)gpio;
    (void)level;
}

static speed_t to_speed(uint32_t baud)
{
    switch (baud) {
    case 1200:    return B1200;
    case 2400:    return B2400;
    case 4800:    return B4800;
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
    case 230400:  return B230400;
    default:      return B9600;
    }
}

static int configure_tty(int fd, const rs485_config_t *cfg)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) != 0) {
        return -1;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, to_speed(cfg->baudrate));
    cfsetospeed(&tio, to_speed(cfg->baudrate));
    tio.c_cflag |= (CLOCAL | CREAD);

    switch (cfg->data_bits) {
    case 7: tio.c_cflag |= CS7; break;
    case 8:
    default: tio.c_cflag |= CS8; break;
    }

    if (cfg->stop_bits == 2) {
        tio.c_cflag |= CSTOPB;
    } else {
        tio.c_cflag &= (tcflag_t)~CSTOPB;
    }

    tio.c_cflag &= (tcflag_t)~(PARENB | PARODD);
    if (cfg->parity == 1) {
        tio.c_cflag |= (PARENB | PARODD);
    } else if (cfg->parity == 2) {
        tio.c_cflag |= PARENB;
    }

    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return 0;
}

static rs485_port_t *port_new(int fd, int owns_fd, const rs485_config_t *cfg)
{
    rs485_port_t *p = (rs485_port_t *)calloc(1, sizeof(*p));
    if (p == 0) {
        if (owns_fd) {
            close(fd);
        }
        return 0;
    }
    p->fd          = fd;
    p->owns_fd     = owns_fd;
    p->de_guard_us = (cfg != 0) ? cfg->de_guard_us : 0u;
    p->de_gpio     = (cfg != 0) ? cfg->de_gpio : -1;
    return p;
}

rs485_port_t *rs485_open_fd(int fd, const rs485_config_t *cfg)
{
    if (fd < 0) {
        return 0;
    }
    return port_new(fd, 0, cfg);
}

rs485_port_t *rs485_open(const rs485_config_t *cfg)
{
    int fd;
    int flags;

    if (cfg == 0 || cfg->device == 0) {
        return 0;
    }
    fd = open(cfg->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return 0;
    }
    if (configure_tty(fd, cfg) != 0) {
        close(fd);
        return 0;
    }
    flags = fcntl(fd, F_GETFL, 0);
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return port_new(fd, 1, cfg);
}

void rs485_close(rs485_port_t *p)
{
    if (p == 0) {
        return;
    }
    if (p->de_asserted) {
        rs485_de_set(p->de_gpio, 0);
    }
    if (p->owns_fd) {
        close(p->fd);
    }
    free(p);
}

int rs485_write(rs485_port_t *p, const uint8_t *data, size_t len)
{
    size_t written = 0;

    if (p == 0 || data == 0 || len == 0) {
        return -1;
    }

    /* Assert DE first: the first start bit must not leave the UART before the
     * transceiver is driving the pair. */
    if (!p->de_asserted) {
        rs485_de_set(p->de_gpio, 1);
        p->de_asserted = 1;
    }

    while (written < len) {
        ssize_t n = write(p->fd, &data[written], len - written);
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd pfd;
            pfd.fd = p->fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 100) <= 0) {
                continue;       /* keep trying; the caller controls the timeout */
            }
            continue;
        }
        return -1;
    }
    return (int)written;
}

int rs485_tx_complete(rs485_port_t *p, uint32_t timeout_ms)
{
    uint64_t deadline;

    if (p == 0) {
        return 0;
    }

    /*
     * A non tty descriptor has no UART shift register, so there is nothing to
     * drain: the bytes are already in the peer's buffer. This path is what the
     * unit tests exercise through a socketpair, and it also covers USB serial
     * bridges that reject TIOCOUTQ / tcdrain.
     */
    if (!isatty(p->fd)) {
        if (p->de_guard_us > 0u) {
            sleep_us((unsigned long)p->de_guard_us);
        }
        if (p->de_asserted) {
            rs485_de_set(p->de_gpio, 0);
            p->de_asserted = 0;
        }
        return 1;
    }

    deadline = now_us() + ((uint64_t)timeout_ms * 1000ull);

    for (;;) {
        int pending = 0;
        int drained;

#ifdef TIOCOUTQ
        if (ioctl(p->fd, TIOCOUTQ, &pending) != 0) {
            pending = 0;
        }
#endif
        if (pending == 0) {
            drained = tcdrain(p->fd);
            if (drained == 0 || errno == ENOTTY || errno == EINVAL) {
                break;
            }
        }
        if (now_us() >= deadline) {
            return 0;
        }
        sleep_us(200ul);
    }

    /* Guard time: let the transceiver release the differential pair. */
    if (p->de_guard_us > 0u) {
        sleep_us((unsigned long)p->de_guard_us);
    }
    if (p->de_asserted) {
        rs485_de_set(p->de_gpio, 0);
        p->de_asserted = 0;
    }
    return 1;
}

int rs485_read(rs485_port_t *p, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    struct pollfd pfd;
    int           rc;

    if (p == 0 || buf == 0 || cap == 0) {
        return -1;
    }
    pfd.fd = p->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc == 0) {
        return 0;                       /* nothing yet, not an error */
    }
    if (rc < 0) {
        return (errno == EINTR) ? 0 : -1;
    }

    {
        ssize_t n = read(p->fd, buf, cap);
        if (n < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }
        return (int)n;
    }
}

void rs485_reset_queues(rs485_port_t *p)
{
    if (p == 0) {
        return;
    }
    tcflush(p->fd, TCIOFLUSH);
    if (p->de_asserted) {
        rs485_de_set(p->de_gpio, 0);
        p->de_asserted = 0;
    }
}
