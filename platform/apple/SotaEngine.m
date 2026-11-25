#import "SotaEngine.h"
#import <AVFoundation/AVFoundation.h>
#import <os/lock.h>
#import <mach/mach_time.h>
#include <stdatomic.h>
#include "../../runtime/amiga/amiga.h"
#include "../../runtime/core/exec.h"
#include "../../runtime/cpu/m68k.h"

void recomp_register_all(void);
size_t audio_available(void);

const int SotaFrameWidth = 704;
const int SotaFrameHeight = 284;
static const int kCropX = 194, kCropY = 28;           // hires x of lores $71, line $1C
static const double kFrameHz = 3546895.0 / (227.0 * 313.0);   // 49.92 Hz
static const double kSampleRate = 48000.0;
static const double kAudioTarget = 0.07;               // seconds buffered ahead

// Triple buffer: the emulation thread writes `back`, then swaps it with `ready`.
typedef struct {
    uint32_t *buf[3];
    int back, ready, front;
    uint64_t readyFrame;
    bool fresh;            // `ready` holds a frame the renderer has not taken yet
    os_unfair_lock lock;
} FrameSwap;

static FrameSwap gSwap;

// Looping: the demo ends on a static screen. Once the picture has not changed for 10 s
// the demo restarts. The longest still moment inside the demo is ~5.5 s (credits).
static const int kLoopStillFrames = 499;               // 10 s at 49.92 Hz
static uint64_t gLastHash;
static int gStillFrames;
static bool gLoopRequested;

static uint64_t frameHash(const uint32_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t k = 0; k < n; k++) { h ^= p[k]; h *= 1099511628211ull; }
    return h;
}

static void onFrame(uint64_t frame)
{
    uint32_t *dst = gSwap.buf[gSwap.back];
    for (int y = 0; y < SotaFrameHeight; y++)
        memcpy(dst + (size_t)y * SotaFrameWidth, &framebuffer[kCropY + y][kCropX], SotaFrameWidth * 4);
    uint64_t h = frameHash(dst, (size_t)SotaFrameWidth * SotaFrameHeight);
    gStillFrames = h == gLastHash ? gStillFrames + 1 : 0;
    gLastHash = h;
    if (gStillFrames >= kLoopStillFrames) { gLoopRequested = true; gStillFrames = 0; }
    os_unfair_lock_lock(&gSwap.lock);
    int t = gSwap.ready; gSwap.ready = gSwap.back; gSwap.back = t;
    gSwap.readyFrame = frame;
    gSwap.fresh = true;
    os_unfair_lock_unlock(&gSwap.lock);
}

@implementation SotaEngine {
    NSThread *_thread;
    atomic_bool _running;
    atomic_bool _pausedFlag;
    dispatch_semaphore_t _stopped;
    NSCondition *_pauseCond;
    AVAudioEngine *_audio;
    AVAudioSourceNode *_source;
    NSData *_adf;
    float *_pullBuf;
    BOOL _registered;
}

+ (instancetype)shared
{
    static SotaEngine *e;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ e = [SotaEngine new]; });
    return e;
}

- (instancetype)init
{
    if ((self = [super init])) {
        for (int k = 0; k < 3; k++) gSwap.buf[k] = calloc((size_t)SotaFrameWidth * SotaFrameHeight, 4);
        gSwap.back = 0; gSwap.ready = 1; gSwap.front = 2;
        gSwap.lock = OS_UNFAIR_LOCK_INIT;
        _pauseCond = [NSCondition new];
        _pullBuf = calloc(8192 * 2, sizeof(float));
    }
    return self;
}

- (BOOL)isPaused { return atomic_load(&_pausedFlag); }

#pragma mark - audio

- (void)startAudio
{
    if (_audio) return;
#if TARGET_OS_IOS
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback error:nil];
    [session setPreferredSampleRate:kSampleRate error:nil];
    [session setActive:YES error:nil];
#endif
    _audio = [AVAudioEngine new];
    AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kSampleRate channels:2];
    float *pull = _pullBuf;
    __weak SotaEngine *weakSelf = self;
    _source = [[AVAudioSourceNode alloc] initWithFormat:fmt renderBlock:
        ^OSStatus(BOOL *silence, const AudioTimeStamp *ts, AVAudioFrameCount frames, AudioBufferList *out) {
            (void)ts;
            float *l = out->mBuffers[0].mData, *r = out->mNumberBuffers > 1 ? out->mBuffers[1].mData : NULL;
            size_t done = 0;
            while (done < frames) {
                size_t want = frames - done; if (want > 4096) want = 4096;
                size_t got = audio_pull(pull, want);
                for (size_t k = 0; k < got; k++) { l[done + k] = pull[k * 2]; if (r) r[done + k] = pull[k * 2 + 1]; }
                if (got < want) {   // underrun: pad with silence
                    for (size_t k = got; k < want; k++) { l[done + k] = 0; if (r) r[done + k] = 0; }
                }
                done += want;
                if (got < want) break;
            }
            for (size_t k = done; k < frames; k++) { l[k] = 0; if (r) r[k] = 0; }
            SotaEngine *s = weakSelf;
            if (s && s->_muted) {
                memset(l, 0, frames * sizeof(float)); if (r) memset(r, 0, frames * sizeof(float));
            }
            *silence = NO;
            return noErr;
        }];
    [_audio attachNode:_source];
    [_audio connect:_source to:_audio.mainMixerNode format:fmt];
    NSError *err = nil;
    if (![_audio startAndReturnError:&err]) NSLog(@"audio start failed: %@", err);
}

#pragma mark - emulation

- (BOOL)startWithError:(NSError **)error
{
    if (!_adf) {
        NSString *path = [[NSBundle mainBundle] pathForResource:@"StateOfTheArt" ofType:@"adf"];
        _adf = path ? [NSData dataWithContentsOfFile:path] : nil;
        if (!_adf) {
            if (error) *error = [NSError errorWithDomain:@"SotaEngine" code:1
                                                userInfo:@{ NSLocalizedDescriptionKey: @"The demo disk image is missing from the app bundle." }];
            return NO;
        }
    }
    [self stopThread];
    if (![self bootMachine]) {
        if (error) *error = [NSError errorWithDomain:@"SotaEngine" code:2
                                            userInfo:@{ NSLocalizedDescriptionKey: @"The disk image is not a valid 880 KB ADF." }];
        return NO;
    }
    if (!_registered) { recomp_register_all(); _registered = YES; }
    exec_use_recomp = true;
    [self startAudio];

    atomic_store(&_running, true);
    _stopped = dispatch_semaphore_create(0);
    _thread = [[NSThread alloc] initWithTarget:self selector:@selector(runLoop) object:nil];
    _thread.qualityOfService = NSQualityOfServiceUserInteractive;
    _thread.name = @"Amiga";
    [_thread start];
    return YES;
}

/// Resets the machine and boots the disk. Call only while the emulation thread is
/// stopped, or from the emulation thread itself.
- (BOOL)bootMachine
{
    amiga_log_level = 0;
    amiga_init();
    if (!amiga_load_adf_memory(_adf.bytes, _adf.length)) return NO;
    audio_set_rate(kSampleRate);
    amiga_set_frame_callback(onFrame);
    gStillFrames = 0; gLastHash = 0; gLoopRequested = false;
    amiga_boot_hle();
    return YES;
}

- (void)stopThread
{
    if (!_thread) return;
    atomic_store(&_running, false);
    [self resume];
    dispatch_semaphore_wait(_stopped, DISPATCH_TIME_FOREVER);
    _thread = nil;
}

- (void)restart { [self startWithError:nil]; }

- (void)pause
{
    atomic_store(&_pausedFlag, true);
    [_audio pause];
}

- (void)resume
{
    [_pauseCond lock];
    atomic_store(&_pausedFlag, false);
    [_pauseCond broadcast];
    [_pauseCond unlock];
    if (_audio && !_audio.isRunning) [_audio startAndReturnError:nil];
}

- (void)setMouseButton:(BOOL)down { amiga_set_mouse_button(down, false); }

- (void)runLoop
{
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const double ticksPerSec = 1e9 * tb.denom / tb.numer;
    uint64_t next = mach_absolute_time();
#if DEBUG
    // screenshot aid (debug builds only): `-SotaFreezeAtFrame N` runs at full speed to frame N,
    // then holds that picture
    uint64_t frameNo = 0;
    const uint64_t freezeAt = (uint64_t)[NSUserDefaults.standardUserDefaults integerForKey:@"SotaFreezeAtFrame"];
#endif
    while (atomic_load(&_running)) {
        if (atomic_load(&_pausedFlag)) {
            [_pauseCond lock];
            while (atomic_load(&_pausedFlag) && atomic_load(&_running)) [_pauseCond wait];
            [_pauseCond unlock];
            next = mach_absolute_time();
            continue;
        }
        @autoreleasepool { exec_run_frames(1); }
        if (gLoopRequested) [self bootMachine];       // end screen held for 10 s: start over
#if DEBUG
        if (freezeAt && ++frameNo <= freezeAt) {
            if (frameNo == freezeAt) atomic_store(&_pausedFlag, true);
            next = mach_absolute_time();
            continue;
        }
#endif
        // pace to PAL, nudged by the audio fill level so sound neither starves nor lags
        double fill = audio_available() / kSampleRate;
        double adj = (fill - kAudioTarget) / kAudioTarget;
        if (adj > 1) adj = 1; if (adj < -1) adj = -1;
        double period = (1.0 / kFrameHz) * (1.0 + 0.02 * adj);
        if (fill < kAudioTarget * 0.5) period = 0;    // catch up after a hiccup
        next += (uint64_t)(period * ticksPerSec);
        uint64_t now = mach_absolute_time();
        if (next + (uint64_t)(0.25 * ticksPerSec) < now) next = now;   // fell far behind: resync
        if (next > now) mach_wait_until(next);
    }
    dispatch_semaphore_signal(_stopped);
}

- (uint64_t)copyLatestFrameInto:(uint32_t *)pixels lastSeen:(uint64_t)lastSeen
{
    os_unfair_lock_lock(&gSwap.lock);
    if (!gSwap.fresh) { os_unfair_lock_unlock(&gSwap.lock); return lastSeen; }
    uint64_t f = gSwap.readyFrame;
    int t = gSwap.front; gSwap.front = gSwap.ready; gSwap.ready = t;
    gSwap.fresh = false;
    os_unfair_lock_unlock(&gSwap.lock);
    memcpy(pixels, gSwap.buf[gSwap.front], (size_t)SotaFrameWidth * SotaFrameHeight * 4);
    return f;
}

- (double)recompiledFraction
{
    double r = (double)exec_stats.recomp_blocks, i = (double)exec_stats.interp_blocks;
    return r + i > 0 ? r / (r + i) : 0;
}

@end
