#ifndef CARDS_CARDS_H
#define CARDS_CARDS_H

/* Card registry and Lovelace dashboard parsing (used by the HA client and the UI). */

#include <stddef.h>

#include "cards/card.h"

/* Card for a Lovelace type, or NULL if ThermoDeck doesn't support it. */
const card_def *card_find(const char *type);
/* Card type id (stored in ha_item.card) <-> definition. */
const card_def *card_get(int id);
int card_id(const card_def *def);

/* Parses a lovelace/config result into items + the entities and forecasts to track
 * (states empty). Returns 0, or -1 with a reason in err. */
int ha_dashboard_parse(const char *js, size_t len, ha_dash *out, char *err, size_t errlen);
/* Dashboard of tiles for a comma-separated entity list (fallback when HA has none). */
void ha_dashboard_from_list(const char *list, ha_dash *out);

#endif
