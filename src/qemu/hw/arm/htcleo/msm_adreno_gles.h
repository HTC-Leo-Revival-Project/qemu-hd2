/*
 * Adreno 200 host GPU backend (OpenGL ES 3): the 3D pipe
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_MSM_ADRENO_GLES_H
#define HW_ARM_HTCLEO_MSM_ADRENO_GLES_H

#include "msm_adreno_gl.h"

/*
 * The same nine calls as the desktop GL backend (msm_adreno_gl.h), with the
 * guest's draws run on the host's OpenGL ES 3 context instead.
 */
/* report what the host's OpenGL ES can do, without keeping a context */
void adreno_gles_probe(MsmAdrenoState *s);

bool adreno_gles_available(MsmAdrenoState *s);
void adreno_gles_draw(MsmAdrenoState *s, const AdrenoGlDraw *g);
void adreno_gles_gmem_to_cpu(MsmAdrenoState *s);
void adreno_gles_gmem_from_cpu(MsmAdrenoState *s);
bool adreno_gles_gmem_snapshot(MsmAdrenoState *s, int *slot);
void adreno_gles_snapshots_wait(MsmAdrenoState *s);
uint8_t *adreno_gles_snapshot_ptr(MsmAdrenoState *s, int slot);
void adreno_gles_snapshots_release(MsmAdrenoState *s);

#endif