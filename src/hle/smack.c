/*
 * joyride - Smacker (smackw32.dll), imported by ordinal.
 *
 * The game plays one movie, GTADATA\MOVIE.SMK (the intro). For now SmackOpen
 * reports it unavailable so the game skips it; real playback (ffmpeg's
 * Smacker decoder into the game's buffer) comes later.
 *
 * Ordinals from the shipped SMACKW32.DLL's export table.
 */
#include <stdio.h>
#include "win32hle.h"
#include "joyride_hle.h"

static void s_SmackSoundUseMSS(void) { RET(1, 1); }          /* #33 (digdriver) -> ok */
static void s_SmackOpen(void) {                               /* #14 (name, flags, extrabuf) */
    fprintf(stderr, "[smack] SmackOpen(%s): movie playback not implemented yet, skipped\n",
            A32(0) ? ASTR(0) : "(handle)");
    RET(0, 3);
}
static void s_SmackClose(void)        { RET(0, 1); }          /* #18 */
static void s_SmackDoFrame(void)      { RET(0, 1); }          /* #19 */
static void s_SmackNextFrame(void)    { RET(0, 1); }          /* #21 */
static void s_SmackToBuffer(void)     { RET(0, 7); }          /* #23 */
static void s_SmackToBufferRect(void) { RET(0, 2); }          /* #28 */
static void s_SmackWait(void)         { RET(0, 1); }          /* #32: 0 = next frame is due */

const win32hle_shim joyride_smack[] = {
    { "smackw32.dll#14", s_SmackOpen },       { "smackw32.dll#18", s_SmackClose },
    { "smackw32.dll#19", s_SmackDoFrame },    { "smackw32.dll#21", s_SmackNextFrame },
    { "smackw32.dll#23", s_SmackToBuffer },   { "smackw32.dll#28", s_SmackToBufferRect },
    { "smackw32.dll#32", s_SmackWait },       { "smackw32.dll#33", s_SmackSoundUseMSS },
    { NULL, NULL },
};
