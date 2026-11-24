/* Markdown card ("markdown"), shown as plain text (marks stripped, line breaks kept). */

#include "cards/card.h"
#include "ui/theme.h"

static const char *const TYPES[] = {"markdown", NULL};
extern const card_def card_markdown;

/* Markdown -> plain text: drops heading marks, emphasis and link targets. */
static void strip_markdown(const char *in, char *out, size_t len)
{
    size_t o = 0;
    int line_start = 1;
    for (const char *s = in; *s && o + 1 < len; s++) {
        if (line_start) {
            while (*s == '#' || *s == '>') s++;
            while (*s == ' ') s++;
            if (!*s) break;
        }
        line_start = *s == '\n';
        if (*s == '*' || *s == '_' || *s == '`') continue;
        if (*s == ']' && s[1] == '(') {
            /* [text](url): keep text */
            const char *e = strchr(s, ')');
            if (e) s = e;
            continue;
        }
        if (*s == '[') continue;
        out[o++] = *s;
    }
    out[o] = 0;
}

static void parse(card_parser *p, int card)
{
    char title[96], content[512];
    card_str(p, card, "title", title, sizeof title);
    card_add_heading(p, title, 1);
    ha_item *it = card_emit(p, &card_markdown, card);
    if (!it) return;
    card_str(p, card, "content", content, sizeof content);
    strip_markdown(content, it->text, sizeof it->text);
}

static int height(const ha_item *it, int w)
{
    return 18 * gfx_text_wrap(&font_sm, 0, 0, w - 24, 12, 18, it->text, 0, 0, 0) + 22;
}

static void draw(const card_ctx *c)
{
    gfx_rrect(c->x, c->y, c->w, c->h, 12, C_CARD);
    gfx_text_wrap(&font_sm, c->x + 12, c->y + 24, c->w - 24, 12, 18, c->it->text, C_TEXT, 0, 1);
}

const card_def card_markdown = {
    .name = "markdown",
    .types = TYPES,
    .span = 12,
    .rows = 1,
    .parse = parse,
    .height = height,
    .draw = draw,
};
