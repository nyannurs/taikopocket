/* Ids of the library's sprites, sounds and skin settings (see assets.def). */
#ifndef TP_ASSETS_H
#define TP_ASSETS_H

#include <stdint.h>

enum {
#define SPRITE(name) SPR_##name,
#define SOUND(name)
#define CFG(name)
#include "assets.def"
#undef SPRITE
#undef SOUND
#undef CFG
    SPR_COUNT
};

enum {
#define SPRITE(name)
#define SOUND(name) SND_##name,
#define CFG(name)
#include "assets.def"
#undef SPRITE
#undef SOUND
#undef CFG
    SND_COUNT
};

enum {
#define SPRITE(name)
#define SOUND(name)
#define CFG(name) CFG_##name,
#include "assets.def"
#undef SPRITE
#undef SOUND
#undef CFG
    CFG_COUNT
};

int assets_cfg(int id, int fallback);
/* mono 16-bit samples of a sound, or NULL */
const int16_t *assets_sound(int id, uint32_t *samples, uint32_t *rate);

#endif
