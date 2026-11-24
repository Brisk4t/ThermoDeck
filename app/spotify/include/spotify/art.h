#ifndef SPOTIFY_ART_H
#define SPOTIFY_ART_H

/* Album art: fetched + decoded on its own thread, scaled to ART_PX square. */
#define ART_PX 144

void art_start(void);
/* Ask for the art at url ("" = none). Cheap; call on every poll. */
void art_request(const char *url);
/* Draws the current art at (x,y) with rounded corners. Returns 0 if none is ready. */
int art_draw(int x, int y, int radius);

#endif
