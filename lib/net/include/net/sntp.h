#ifndef NET_SNTP_H
#define NET_SNTP_H

/* SNTP: sets the system clock. 0 on success. */
int sntp_sync(const char *server);

#endif
