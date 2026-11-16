// AudioToolbox's output AudioUnit on WASAPI (compat/AudioToolbox/AudioToolbox.h): the unit's render callback fills the shared-mode endpoint buffer of the default
// output device. A render thread of its own (MMCSS "Pro Audio") waits on the stream's event and renders what the buffer has room for; the format set on the input
// scope (PCM16, the host's rate and channel count) is given to WASAPI as it is, with AUTOCONVERTPCM: the audio engine resamples and mixes to the device's format.
// Stop returns once the thread has rendered its last (hle_audio.c relies on Core Audio's AudioOutputUnitStop doing so). A device that goes away (headphones
// unplugged, the default device changed: AUDCLNT_E_DEVICE_INVALIDATED) is reopened on the default device by the thread itself.
#define COBJMACROS 1
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include "dm.h"

static const CLSID k_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID k_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID k_IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID k_IID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

struct dm_au_component { int unused; };
struct dm_au {
    AudioStreamBasicDescription fmt;
    AURenderCallbackStruct cb;
    pthread_mutex_t mu;
    HANDLE th, ev, ready;
    _Atomic int run;
    OSStatus open_status;
    _Atomic uint64_t callbacks, frames, glitches;
    int buffer_ms;
};
static struct dm_au_component g_output;

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d->componentType == kAudioUnitType_Output ? &g_output : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    if (c != &g_output) return -1;
    struct dm_au *au = calloc(1, sizeof *au); if (!au) return -1;
    pthread_mutex_init(&au->mu, NULL);
    const char *e = getenv("DM_WASAPI_MS"); au->buffer_ms = e && atoi(e) > 0 ? atoi(e) : 20;   // the endpoint buffer the engine is asked for (it may give more)
    *out = au; return noErr;
}
static void render(struct dm_au *au, void *audio, UInt32 frames) {
    UInt32 bytes = frames * au->fmt.mBytesPerFrame;
    AudioBufferList bl = {1, {{au->fmt.mChannelsPerFrame, bytes, audio}}};
    AudioUnitRenderActionFlags fl = 0; AudioTimeStamp ts = {0};
    ts.mHostTime = dm_win_mono_ns();
    if (!au->cb.inputProc || au->cb.inputProc(au->cb.inputProcRefCon, &fl, &ts, 0, frames, &bl)) memset(audio, 0, bytes);
}
typedef struct { IMMDeviceEnumerator *en; IMMDevice *dev; IAudioClient *ac; IAudioRenderClient *rc; UINT32 cap; } wstream;
static void ws_close(wstream *s) {
    if (s->ac) IAudioClient_Stop(s->ac);
    if (s->rc) IAudioRenderClient_Release(s->rc);
    if (s->ac) IAudioClient_Release(s->ac);
    if (s->dev) IMMDevice_Release(s->dev);
    if (s->en) IMMDeviceEnumerator_Release(s->en);
    memset(s, 0, sizeof *s);
}
static HRESULT ws_open(struct dm_au *au, wstream *s) {
    memset(s, 0, sizeof *s);
    HRESULT hr = CoCreateInstance(&k_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &k_IID_IMMDeviceEnumerator, (void **)&s->en);
    if (FAILED(hr)) goto fail;
    if (FAILED(hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(s->en, eRender, eConsole, &s->dev))) goto fail;
    if (FAILED(hr = IMMDevice_Activate(s->dev, &k_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&s->ac))) goto fail;
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = (WORD)au->fmt.mChannelsPerFrame; wf.nSamplesPerSec = (DWORD)au->fmt.mSampleRate;
    wf.wBitsPerSample = 16; wf.nBlockAlign = (WORD)(wf.nChannels * 2); wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    DWORD fl = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(hr = IAudioClient_Initialize(s->ac, AUDCLNT_SHAREMODE_SHARED, fl, (REFERENCE_TIME)au->buffer_ms * 10000, 0, &wf, NULL))) goto fail;
    if (FAILED(hr = IAudioClient_SetEventHandle(s->ac, au->ev))) goto fail;
    if (FAILED(hr = IAudioClient_GetBufferSize(s->ac, &s->cap))) goto fail;
    if (FAILED(hr = IAudioClient_GetService(s->ac, &k_IID_IAudioRenderClient, (void **)&s->rc))) goto fail;
    BYTE *b; if (SUCCEEDED(IAudioRenderClient_GetBuffer(s->rc, s->cap, &b))) IAudioRenderClient_ReleaseBuffer(s->rc, s->cap, AUDCLNT_BUFFERFLAGS_SILENT);   // start on silence
    REFERENCE_TIME per = 0, lat = 0; IAudioClient_GetDevicePeriod(s->ac, &per, NULL); IAudioClient_GetStreamLatency(s->ac, &lat);
    LOG("audio: WASAPI shared stream %u Hz %u channels (PCM16, converted by the audio engine), buffer %u frames (%.1f ms), device period %.1f ms, latency %.1f ms",
        (unsigned)wf.nSamplesPerSec, (unsigned)wf.nChannels, (unsigned)s->cap, s->cap * 1000.0 / wf.nSamplesPerSec, per / 1e4, lat / 1e4);
    if (FAILED(hr = IAudioClient_Start(s->ac))) goto fail;
    return S_OK;
fail:
    LOG("[RED] audio: WASAPI stream open failed: HRESULT %#lx", (unsigned long)hr);
    ws_close(s); return hr;
}
static DWORD WINAPI render_main(void *p) {
    struct dm_au *au = p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD task = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (!mm) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"dm audio (WASAPI)");
    wstream s; HRESULT hr = ws_open(au, &s);
    au->open_status = FAILED(hr) ? (OSStatus)hr : noErr;
    SetEvent(au->ready);
    int reopen_wait = 0;
    while (atomic_load(&au->run)) {
        DWORD w = WaitForSingleObject(au->ev, 200);
        if (!atomic_load(&au->run)) break;
        if (!s.ac) {   // lost: the default device again, every 500 ms
            if (++reopen_wait < 3 && w == WAIT_TIMEOUT) continue;
            reopen_wait = 0;
            if (FAILED(ws_open(au, &s))) { Sleep(300); continue; }
            LOG("audio: WASAPI stream reopened on the default device");
        }
        UINT32 pad = 0;
        hr = IAudioClient_GetCurrentPadding(s.ac, &pad);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) { LOG("audio: WASAPI device gone (HRESULT %#lx): reopening on the default device", (unsigned long)hr); ws_close(&s); continue; }
        if (FAILED(hr)) continue;
        UINT32 n = s.cap - pad;
        if (!n) continue;
        if (w == WAIT_TIMEOUT && pad == 0) atomic_fetch_add(&au->glitches, 1);
        BYTE *b;
        hr = IAudioRenderClient_GetBuffer(s.rc, n, &b);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { ws_close(&s); continue; }
        if (FAILED(hr)) continue;
        render(au, b, n);
        IAudioRenderClient_ReleaseBuffer(s.rc, n, 0);
        atomic_fetch_add(&au->callbacks, 1); atomic_fetch_add(&au->frames, n);
    }
    ws_close(&s);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
    return 0;
}
static void stop_thread(struct dm_au *au) {
    if (!au->th) return;
    atomic_store(&au->run, 0); SetEvent(au->ev);
    WaitForSingleObject(au->th, INFINITE); CloseHandle(au->th); au->th = NULL;
}
OSStatus AudioComponentInstanceDispose(AudioComponentInstance au) {
    if (!au) return noErr;
    pthread_mutex_lock(&au->mu); stop_thread(au); pthread_mutex_unlock(&au->mu);
    if (au->ev) CloseHandle(au->ev);
    if (au->ready) CloseHandle(au->ready);
    pthread_mutex_destroy(&au->mu); free(au); return noErr;
}
OSStatus AudioUnitSetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size) {
    (void)scope; (void)el;
    if (id == kAudioUnitProperty_StreamFormat && size >= sizeof au->fmt) {
        const AudioStreamBasicDescription *f = data;
        if (f->mFormatID != kAudioFormatLinearPCM || f->mBitsPerChannel != 16 || !(f->mFormatFlags & kLinearPCMFormatFlagIsSignedInteger)) return -50;   // paramErr: only PCM16
        au->fmt = *f; return noErr;
    }
    if (id == kAudioUnitProperty_SetRenderCallback && size >= sizeof au->cb) { au->cb = *(const AURenderCallbackStruct *)data; return noErr; }
    return -10879;   // kAudioUnitErr_InvalidProperty
}
OSStatus AudioUnitGetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, void *data, UInt32 *size) {
    (void)scope; (void)el;
    if (id == kAudioUnitProperty_StreamFormat && *size >= sizeof au->fmt) { *(AudioStreamBasicDescription *)data = au->fmt; *size = sizeof au->fmt; return noErr; }
    return -10879;
}
OSStatus AudioUnitAddPropertyListener(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc fn, void *ctx) { (void)au; (void)id; (void)fn; (void)ctx; return noErr; }
OSStatus AudioUnitRemovePropertyListenerWithUserData(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc fn, void *ctx) { (void)au; (void)id; (void)fn; (void)ctx; return noErr; }
OSStatus AudioUnitInitialize(AudioUnit au) {
    if (!au->fmt.mChannelsPerFrame || !au->fmt.mSampleRate) return -50;
    pthread_mutex_lock(&au->mu);
    if (!au->ev) au->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!au->ready) au->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    pthread_mutex_unlock(&au->mu);
    return au->ev && au->ready ? noErr : -1;
}
OSStatus AudioUnitUninitialize(AudioUnit au) { pthread_mutex_lock(&au->mu); stop_thread(au); pthread_mutex_unlock(&au->mu); return noErr; }
OSStatus AudioOutputUnitStart(AudioUnit au) {
    if (!au->ev && AudioUnitInitialize(au)) return -1;
    pthread_mutex_lock(&au->mu);
    OSStatus st = noErr;
    if (!au->th) {
        atomic_store(&au->run, 1); ResetEvent(au->ready);
        au->th = CreateThread(NULL, 256 * 1024, render_main, au, 0, NULL);
        if (!au->th) st = -1;
        else {
            WaitForSingleObject(au->ready, 5000);
            st = au->open_status;
            if (st) stop_thread(au);
        }
    }
    pthread_mutex_unlock(&au->mu);
    return st;
}
OSStatus AudioOutputUnitStop(AudioUnit au) {
    pthread_mutex_lock(&au->mu); stop_thread(au); pthread_mutex_unlock(&au->mu);
    return noErr;
}
uint64_t dm_wasapi_callbacks(AudioUnit au) { return atomic_load(&au->callbacks); }   // tests
