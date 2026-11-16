// The hardware movie decoders (H.264 and AV1) on Windows: not yet ported (Media Foundation's H.264 / AV1 decoder MFTs would take the Android MediaCodec port's
// place). Until then a decoder opens and decodes nothing, so a game's H.264 or AV1 movie shows no picture and the game goes on (its movie player sees the end).
// VP8 / VP9 movies (WebM: Pokémon Pokopia's) are decoded by libvpx on the CPU, as on every platform, and are not affected.
#include "dm.h"
#include "hle_movie_avc.h"
#include "hle_movie_av1.h"

struct mavc { int told; };
mavc *mavc_new(void) {
    static int told; if (!told++) LOG("[RED] movie: no H.264 decoder on Windows yet (Media Foundation is not ported): H.264 movies show no picture");
    return calloc(1, sizeof(mavc));
}
void mavc_free(mavc *a) { free(a); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) { (void)a; (void)p; (void)n; (void)pts_us; return -1; }
int mavc_take(mavc *a, int flush, uint8_t *nv12, int w, int h, int64_t *pts_us) { (void)a; (void)flush; (void)nv12; (void)w; (void)h; (void)pts_us; return 0; }
int mavc_queued(const mavc *a) { (void)a; return 0; }
int mavc_bt709(const mavc *a, int h) { (void)a; return h >= 720; }
void mavc_set_rgba(mavc *a) { (void)a; }
void mavc_drop(mavc *a) { (void)a; }

struct mav1 { int unused; };
mav1 *mav1_new(void) {
    static int told; if (!told++) LOG("[RED] movie: no AV1 decoder on Windows yet (Media Foundation is not ported): AV1 movies show no picture");
    return calloc(1, sizeof(mav1));
}
void mav1_free(mav1 *a) { free(a); }
void mav1_reset(mav1 *a) { (void)a; }
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out) { (void)a; (void)data; (void)n; (void)out; return -1; }
const char *mav1_backend(const mav1 *a) { (void)a; return "none (Windows)"; }
int mav1_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h) { (void)obu; (void)n; (void)c; (void)w; (void)h; return 0; }

// AAC decoding for movies (AudioConverter): not on Windows yet (Media Foundation's AAC decoder would do it); the movie's sound is silent
#include <AudioToolbox/AudioToolbox.h>
OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) {
    (void)in; (void)out; *c = NULL;
    static int told; if (!told++) LOG("[RED] movie: no AAC decoder on Windows yet: AAC movie sound is silent");
    return -50;
}
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID id, UInt32 size, const void *data) { (void)c; (void)id; (void)size; (void)data; return -50; }
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc fn, void *ctx, UInt32 *packets, AudioBufferList *out, AudioStreamPacketDescription *desc) {
    (void)c; (void)fn; (void)ctx; (void)out; (void)desc; if (packets) *packets = 0; return -50;
}
OSStatus AudioConverterDispose(AudioConverterRef c) { (void)c; return noErr; }
