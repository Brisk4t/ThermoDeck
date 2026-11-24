#ifndef WEB_WEB_H
#define WEB_WEB_H

void web_start(int port);
/* Current 6-digit PIN for the LAN auth page (shown on screen). */
void web_get_pin(char out[7]);

#endif
