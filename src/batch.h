/* batch.h: static batching. The arena is some 660 meshes, as on the page, and
 * the cart pays for each draw through the GL import bridge; merging the
 * meshes that never move and share a material into one draw keeps the same
 * pixels for a fraction of the calls. Textures with identical pixels (every
 * Kenney prop of a kit carries its own copy of the kit's colour map) are
 * shared first, so whole kits merge. */
#ifndef DS_BATCH_H
#define DS_BATCH_H
#include "three.h"

/* merge the static opaque meshes under root; the subtrees in skip (animated
 * objects) are left alone */
void batch_static(t3_object *root, t3_object *const *skip, int nskip);

#endif
