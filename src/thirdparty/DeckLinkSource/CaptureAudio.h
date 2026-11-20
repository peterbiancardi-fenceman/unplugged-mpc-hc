// CaptureAudio.h — ClipGuard Media Player 2.69.0: plays the capture card's HDMI audio.
//
// Only when ClipGuard Media Player asks for it: the player creates the shared block
// "Local\ClipGuard_CaptureAudio" (magic "CGA1", enabled = 1). Without it (Unplugged,
// or any other host) nothing here runs and the filter stays video-only, exactly as
// before.
//
// The SDK callback hands each audio packet to Push() (a copy into a ring, no waiting).
// A render thread plays the ring on the default Windows playback device (the one the
// player's own audio renderer uses), WASAPI exclusive when the device allows it,
// shared otherwise. The sound is held back by the picture's look-ahead (frames from
// the player, at the capture frame rate) plus the player's "Game audio delay" trim.
//
// Bitstream guard: a Dolby / DTS bitstream (IEC 61937) carried over HDMI arrives at
// the card as if it were PCM, and played as PCM it is loud noise. Its burst preamble
// (Pa = 0xF872, Pb = 0x4E1F) is looked for in every packet; while it is seen (and for
// 2 s after) the sound is replaced with silence, and what is already queued is
// cleared. Only real PCM (a PS5 game set to Linear PCM, a PC) is ever played.

#pragma once

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

struct CgCaptureAudioShm {          // written by ClipGuard Media Player (ClipGuardPlex.cs)
    volatile LONG magic;            // 'CGA1'
    volatile LONG version;          // 1
    volatile LONG enabled;          // 1 = play the card's audio
    volatile LONG lookAhead;        // frames the picture is held back (0, 2, 4, 6, 12)
    volatile LONG trimMs;           // Game audio delay (Settings), -500..500
    // 2.70.0: written back here, for the player's Info card
    volatile LONG state;            // 0 off, 1 PCM playing, 2 bitstream (muted), 3 no sound on the card
    volatile LONG active;           // channels carrying sound in the last second
    volatile LONG device;           // 0 none, 1 exclusive, 2 shared
};

class CaptureAudio {
public:
    typedef void (*LogFn)(void* ctx, const char* line);

    CaptureAudio() { InitializeCriticalSection(&m_cs); }
    ~CaptureAudio() {
        Stop();
        if (m_shm) UnmapViewOfFile((LPCVOID)m_shm);
        if (m_map) CloseHandle(m_map);
        DeleteCriticalSection(&m_cs);
    }

    void SetLog(void* ctx, LogFn fn) { m_logCtx = ctx; m_logFn = fn; }

    // True when ClipGuard Media Player has asked for the card's audio.
    bool Wanted() {
        if (!m_shm) {
            m_map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, L"Local\\ClipGuard_CaptureAudio");
            if (!m_map) return false;
            m_shm = (CgCaptureAudioShm*)MapViewOfFile(m_map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(CgCaptureAudioShm));
            if (!m_shm) { CloseHandle(m_map); m_map = nullptr; return false; }
        }
        return m_shm->magic == 0x31414743 && m_shm->enabled == 1;
    }

    void Start() {
        if (m_thread.joinable()) return;
        m_quit = 0;
        m_cap = 48000 * 4;                       // 4 s ring, 8 channels
        m_ring.assign((size_t)m_cap * 8, 0);
        m_r = m_w = 0;
        m_startTick = GetTickCount64();
        m_muteUntil = 0;
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_thread = std::thread(&CaptureAudio::RenderLoop, this);
    }

    // Bounded: the render loop waits at most 100 ms at a time.
    void Stop() {
        if (!m_thread.joinable()) return;
        InterlockedExchange(&m_quit, 1);
        if (m_wake) SetEvent(m_wake);
        m_thread.join();
        if (m_wake) { CloseHandle(m_wake); m_wake = nullptr; }
        if (m_shm) { Pub(&m_shm->state, 0); Pub(&m_shm->active, 0); Pub(&m_shm->device, 0); }
    }

    void SetFps(LONG num, LONG den) {
        if (num > 0 && den > 0) { InterlockedExchange(&m_fpsNum, num); InterlockedExchange(&m_fpsDen, den); }
    }

    // SDK callback thread: copy only, never block for long.
    void Push(const int32_t* in, long frames, int ch) {
        if (!in || frames <= 0 || (ch != 2 && ch != 8) || !m_thread.joinable()) return;
        bool sync = false;
        for (long f = 0; f < frames && !sync; f++) {
            const int32_t* s = in + (size_t)f * ch;
            for (int c = 0; c + 1 < ch; c += 2) {
                const uint32_t a = ((uint32_t)s[c] >> 16) & 0xFFFF, b = ((uint32_t)s[c + 1] >> 16) & 0xFFFF;
                if ((a == 0xF872 && b == 0x4E1F) || (a == 0x72F8 && b == 0x1F4E)) { sync = true; break; }
            }
        }
        const ULONGLONG now = GetTickCount64();
        DWORD mask = 0;
        for (long f = 0; f < frames; f += 4) {
            const int32_t* s = in + (size_t)f * ch;
            for (int c = 0; c < ch; c++) if ((s[c] >> 8) != 0 && (s[c] >> 8) != -1) mask |= 1u << c;   // above the noise floor
        }
        EnterCriticalSection(&m_cs);
        m_lastPush = now;
        m_mask |= mask;
        if (now - m_winStart >= 1000) {
            const bool bs = sync || now < m_muteUntil;
            if (m_shm) {
                Pub(&m_shm->active, bs ? 0 : Bits(m_mask));
                Pub(&m_shm->state, bs ? 2 : (m_mask ? 1 : 3));
            }
            m_mask = 0;
            m_winStart = now;
        }
        if (sync) {
            if (now >= m_muteUntil) m_logBitstream = 1;
            m_muteUntil = now + 2000;
            for (ULONGLONG i = m_r; i < m_w; i++) memset(&m_ring[(size_t)(i % m_cap) * 8], 0, 8 * sizeof(int32_t));
        }
        const bool mute = now < m_muteUntil || now < m_startTick + 300;
        if (!mute && m_wasMuted && m_muteUntil != 0) m_logPcm = 1;
        m_wasMuted = mute;
        if (m_w - m_r + (ULONGLONG)frames > (ULONGLONG)m_cap) m_r = m_w + frames - m_cap;   // full: drop the oldest
        for (long f = 0; f < frames; f++) {
            int32_t* d = &m_ring[(size_t)((m_w + f) % m_cap) * 8];
            if (mute) { memset(d, 0, 8 * sizeof(int32_t)); continue; }
            const int32_t* s = in + (size_t)f * ch;
            if (ch == 8) {   // HDMI (CEA-861) order FL FR LFE FC RL RR RLC RRC -> Windows FL FR FC LFE BL BR SL SR
                d[0] = s[0]; d[1] = s[1]; d[2] = s[3]; d[3] = s[2];
                d[4] = s[6]; d[5] = s[7]; d[6] = s[4]; d[7] = s[5];
            } else {
                d[0] = s[0]; d[1] = s[1]; d[2] = d[3] = d[4] = d[5] = d[6] = d[7] = 0;
            }
        }
        m_w += frames;
        m_pushes++;
        LeaveCriticalSection(&m_cs);
    }

private:
    CRITICAL_SECTION m_cs;
    std::vector<int32_t> m_ring;
    LONG m_cap = 0;
    ULONGLONG m_r = 0, m_w = 0;               // frames read / written (fill = w - r)
    ULONGLONG m_startTick = 0, m_muteUntil = 0;
    bool m_wasMuted = true;
    LONG m_logBitstream = 0, m_logPcm = 0;
    int m_refusedLogs = 0;
    unsigned long long m_pushes = 0;
    volatile LONG m_quit = 0;
    volatile LONG m_fpsNum = 24000, m_fpsDen = 1001;
    HANDLE m_wake = nullptr;
    std::thread m_thread;
    HANDLE m_map = nullptr;
    CgCaptureAudioShm* m_shm = nullptr;
    ULONGLONG m_lastPush = 0, m_winStart = 0;
    DWORD m_mask = 0;
    void Pub(volatile LONG* f, LONG v) { if (m_shm) InterlockedExchange(f, v); }
    static LONG Bits(DWORD m) { LONG n = 0; while (m) { n += (LONG)(m & 1); m >>= 1; } return n; }
    void* m_logCtx = nullptr;
    LogFn m_logFn = nullptr;

    enum Kind { kInt32, kInt16, kFloat };

    void Log(const char* fmt, ...) {
        char buf[512];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        if (m_logFn) m_logFn(m_logCtx, buf); else OutputDebugStringA(buf);
    }

    LONG TargetFrames() {
        LONG la = 0, trim = 0;
        if (m_shm && m_shm->magic == 0x31414743) { la = m_shm->lookAhead; trim = m_shm->trimMs; }
        if (la < 0 || la > 24) la = 0;
        if (trim < -1000 || trim > 1000) trim = 0;
        const double fps = (double)m_fpsNum / (double)(m_fpsDen > 0 ? m_fpsDen : 1);
        double ms = (fps > 1.0 ? la * 1000.0 / fps : 0.0) + trim;
        if (ms < 50.0) ms = 50.0;              // floor: room for the card's packet cadence
        if (ms > 3000.0) ms = 3000.0;
        return (LONG)(ms * 48.0);
    }

    static GUID SubPcm()   { GUID g = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }; return g; }
    static GUID SubFloat() { GUID g = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }; return g; }

    static WAVEFORMATEXTENSIBLE Fmt(int ch, int container, int valid, bool flt) {
        WAVEFORMATEXTENSIBLE f = {};
        f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f.Format.nChannels = (WORD)ch;
        f.Format.nSamplesPerSec = 48000;
        f.Format.wBitsPerSample = (WORD)container;
        f.Format.nBlockAlign = (WORD)(ch * container / 8);
        f.Format.nAvgBytesPerSec = 48000 * f.Format.nBlockAlign;
        f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        f.Samples.wValidBitsPerSample = (WORD)valid;
        f.dwChannelMask = ch == 8 ? 0x63F : 0x3;   // 7.1 surround (FL FR FC LFE BL BR SL SR) / stereo
        f.SubFormat = flt ? SubFloat() : SubPcm();
        return f;
    }

    // For each output channel, the ring channel(s) it plays. Ring: FL FR FC LFE BL BR SL SR.
    static void BuildMap(DWORD mask, int outCh, int map[8][2]) {
        static const DWORD bits[8] = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x200, 0x400 };
        const bool hasBack = (mask & 0x30) != 0, hasSide = (mask & 0x600) != 0;
        int o = 0;
        for (int b = 0; b < 32 && o < outCh && o < 8; b++) {
            const DWORD bit = 1u << b;
            if (!(mask & bit)) continue;
            map[o][0] = map[o][1] = -1;
            for (int r = 0; r < 8; r++) if (bits[r] == bit) map[o][0] = r;
            if (bit == 0x10 && !hasSide) map[o][1] = 6;   // 5.1 (back): side surrounds play there
            if (bit == 0x20 && !hasSide) map[o][1] = 7;
            if (bit == 0x200 && !hasBack) map[o][1] = 4;  // 5.1 (side): back surrounds play there
            if (bit == 0x400 && !hasBack) map[o][1] = 5;
            o++;
        }
        for (; o < 8; o++) map[o][0] = map[o][1] = -1;
        if (mask == 0) { map[0][0] = 0; map[0][1] = -1; if (outCh > 1) { map[1][0] = 1; map[1][1] = -1; } }
    }

    // Takes n frames for the device (or silence), keeping the queue at the target delay.
    void Take(int32_t* tmp, UINT32 n) {
        const LONG target = TargetFrames();
        const LONG tol = 960;                    // 20 ms
        EnterCriticalSection(&m_cs);
        ULONGLONG fill = m_w - m_r;
        if ((LONGLONG)fill > (LONGLONG)target + tol) { m_r = m_w - (ULONGLONG)target; fill = target; }
        if ((LONGLONG)fill < (LONGLONG)target - tol || fill < n) {
            memset(tmp, 0, (size_t)n * 8 * sizeof(int32_t));   // build up to the delay (or wait for the card)
        } else {
            for (UINT32 i = 0; i < n; i++) memcpy(tmp + (size_t)i * 8, &m_ring[(size_t)((m_r + i) % m_cap) * 8], 8 * sizeof(int32_t));
            m_r += n;
        }
        if (m_shm && GetTickCount64() - m_lastPush > 1500 && m_shm->state != 3) { Pub(&m_shm->state, 3); Pub(&m_shm->active, 0); }
        const LONG lb = m_logBitstream, lp = m_logPcm;
        m_logBitstream = m_logPcm = 0;
        LeaveCriticalSection(&m_cs);
        if (lb) Log("[CAPTURE AUDIO] bitstream (Dolby / DTS) on the card: muted (only PCM is played)");
        if (lp) Log("[CAPTURE AUDIO] PCM again: playing");
    }

    void RenderLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        Log("[CAPTURE AUDIO] started (delay: look-ahead + Game audio delay)");
        int fails = 0;
        while (!m_quit) {
            const HRESULT hr = PlayOnce();
            if (m_quit) break;
            if (++fails <= 3) Log("[CAPTURE AUDIO] device stopped hr=0x%08X; trying again in 2 s", (unsigned)hr);
            for (int i = 0; i < 20 && !m_quit; i++) WaitForSingleObject(m_wake, 100);
        }
        Log("[CAPTURE AUDIO] stopped");
        CoUninitialize();
    }

    HRESULT PlayOnce() {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* ac = nullptr;
        IAudioRenderClient* rc = nullptr;
        HANDLE ev = nullptr;
        WAVEFORMATEX* mix = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
        if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);

        WAVEFORMATEXTENSIBLE fmt = {};
        Kind kind = kInt32;
        int outCh = 0;
        bool exclusive = false;
        if (SUCCEEDED(hr)) {
            struct Try { int ch, cont, valid; Kind k; } tries[] = {
                { 8, 32, 24, kInt32 }, { 8, 32, 32, kInt32 }, { 8, 16, 16, kInt16 },
                { 2, 32, 24, kInt32 }, { 2, 32, 32, kInt32 }, { 2, 16, 16, kInt16 } };
            for (const Try& t : tries) {
                WAVEFORMATEXTENSIBLE f = Fmt(t.ch, t.cont, t.valid, false);
                if (ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&f, nullptr) == S_OK) {
                    fmt = f; kind = t.k; outCh = t.ch; exclusive = true; break;
                }
            }
        }
        if (SUCCEEDED(hr) && exclusive) {
            REFERENCE_TIME def = 0, mn = 0;
            ac->GetDevicePeriod(&def, &mn);
            if (def <= 0) def = 100000;
            hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, def, def, (WAVEFORMATEX*)&fmt, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 nf = 0;
                ac->GetBufferSize(&nf);
                ac->Release(); ac = nullptr;
                def = (REFERENCE_TIME)(10000.0 * 1000.0 * nf / 48000.0 + 0.5);
                hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
                if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, def, def, (WAVEFORMATEX*)&fmt, nullptr);
            }
            if (FAILED(hr)) {   // in use, or refused: try shared
                if (++m_refusedLogs <= 3) Log("[CAPTURE AUDIO] exclusive %d ch refused hr=0x%08X; using shared mode", outCh, (unsigned)hr);
                if (ac) { ac->Release(); ac = nullptr; }
                exclusive = false;
                hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
            }
        }
        DWORD mask = exclusive ? fmt.dwChannelMask : 0;
        if (SUCCEEDED(hr) && !exclusive) {
            hr = ac->GetMixFormat(&mix);
            if (SUCCEEDED(hr)) {
                outCh = mix->nChannels;
                const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)mix;
                const bool ext = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22;
                const bool flt = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (ext && IsEqualGUID(x->SubFormat, SubFloat()));
                if (flt && mix->wBitsPerSample == 32) kind = kFloat;
                else if (mix->wBitsPerSample == 32) kind = kInt32;
                else if (mix->wBitsPerSample == 16) kind = kInt16;
                else hr = E_NOTIMPL;
                mask = ext ? x->dwChannelMask : (outCh == 2 ? 0x3 : 0);
                if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, mix, nullptr);
            }
        }
        UINT32 bufFrames = 0;
        if (SUCCEEDED(hr)) {
            ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            hr = ac->SetEventHandle(ev);
        }
        if (SUCCEEDED(hr)) hr = ac->GetBufferSize(&bufFrames);
        if (SUCCEEDED(hr)) hr = ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc);

        int map[8][2];
        BuildMap(mask, outCh, map);
        std::vector<int32_t> tmp;
        if (SUCCEEDED(hr)) {
            const int sr = exclusive ? 48000 : (mix ? (int)mix->nSamplesPerSec : 48000);
            if (sr != 48000) hr = E_NOTIMPL;   // the card is 48 kHz; Windows' own rate must match in shared mode
            else {
                Log("[CAPTURE AUDIO] playing on the default device: %s, %d ch, %s, buffer %u frames",
                    exclusive ? "exclusive" : "shared", outCh, kind == kFloat ? "float" : (kind == kInt16 ? "16-bit" : "32-bit"), (unsigned)bufFrames);
                tmp.assign((size_t)bufFrames * 8, 0);
                if (m_shm) Pub(&m_shm->device, exclusive ? 1 : 2);
                BYTE* p = nullptr;
                if (SUCCEEDED(rc->GetBuffer(bufFrames, &p))) rc->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
                hr = ac->Start();
            }
        }
        while (SUCCEEDED(hr) && !m_quit) {
            HANDLE h[2] = { ev, m_wake };
            WaitForMultipleObjects(2, h, FALSE, 100);
            if (m_quit) break;
            UINT32 n = bufFrames;
            if (!exclusive) {
                UINT32 pad = 0;
                hr = ac->GetCurrentPadding(&pad);
                if (FAILED(hr)) break;
                n = bufFrames - pad;
            }
            if (n == 0) continue;
            BYTE* p = nullptr;
            hr = rc->GetBuffer(n, &p);
            if (FAILED(hr)) break;
            Take(tmp.data(), n);
            for (UINT32 i = 0; i < n; i++) {
                const int32_t* s = tmp.data() + (size_t)i * 8;
                for (int c = 0; c < outCh; c++) {
                    int64_t v = 0;
                    if (c < 8) { if (map[c][0] >= 0) v += s[map[c][0]]; if (map[c][1] >= 0) v += s[map[c][1]]; }
                    if (v > INT32_MAX) v = INT32_MAX; else if (v < INT32_MIN) v = INT32_MIN;
                    const size_t k = (size_t)i * outCh + c;
                    if (kind == kInt32) ((int32_t*)p)[k] = (int32_t)v;
                    else if (kind == kInt16) ((int16_t*)p)[k] = (int16_t)(v >> 16);
                    else ((float*)p)[k] = (float)((double)v / 2147483648.0);
                }
            }
            hr = rc->ReleaseBuffer(n, 0);
        }
        if (m_shm) Pub(&m_shm->device, 0);
        if (ac) ac->Stop();
        if (rc) rc->Release();
        if (ac) ac->Release();
        if (mix) CoTaskMemFree(mix);
        if (dev) dev->Release();
        if (en) en->Release();
        if (ev) CloseHandle(ev);
        return hr;
    }
};
