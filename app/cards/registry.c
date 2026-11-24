/* The list of card types. To add one, define it in its own file and list it here. */

#include <string.h>

#include "cards/cards.h"

extern const card_def card_heading, card_tile, card_button, card_value, card_entities, card_glance, card_entity_row,
    card_markdown, card_weather, card_unsupported;

/* id = index; card_unsupported must stay first (id 0 is the fallback) */
static const card_def *const CARDS[] = {
    &card_unsupported, &card_heading, &card_tile,       &card_button,   &card_value,
    &card_entities,    &card_glance,  &card_entity_row, &card_markdown, &card_weather,
};

const card_def *card_find(const char *type)
{
    for (size_t i = 0; i < sizeof CARDS / sizeof CARDS[0]; i++)
        for (const char *const *t = CARDS[i]->types; t && *t; t++)
            if (!strcmp(*t, type)) return CARDS[i];
    return NULL;
}

const card_def *card_get(int id)
{
    return id >= 0 && id < (int)(sizeof CARDS / sizeof CARDS[0]) ? CARDS[id] : &card_unsupported;
}

int card_id(const card_def *def)
{
    for (size_t i = 0; i < sizeof CARDS / sizeof CARDS[0]; i++)
        if (CARDS[i] == def) return (int)i;
    return 0;
}
