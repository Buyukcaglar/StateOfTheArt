// Shared macOS/iOS host for the recompiled demo: emulation thread, audio, frame handoff.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Visible display area: the demo's overscan window (lores $71-$1D1, lines $1C-$138),
// stored at hires width (lores pixels doubled), one row per line.
extern const int SotaFrameWidth;    // 704
extern const int SotaFrameHeight;   // 284

@interface SotaEngine : NSObject

+ (instancetype)shared;

/// Loads the bundled disk image and starts (or restarts) the demo.
- (BOOL)startWithError:(NSError **)error;
- (void)restart;
- (void)pause;
- (void)resume;
@property (nonatomic, readonly, getter=isPaused) BOOL paused;
@property (nonatomic) BOOL muted;

/// Left mouse button (the demo's "fire"); click or tap.
- (void)setMouseButton:(BOOL)down;

/// Copies the newest complete frame (BGRA, SotaFrameWidth x SotaFrameHeight) if it is newer
/// than `lastSeen`. Returns the frame number copied, or `lastSeen` when nothing new.
- (uint64_t)copyLatestFrameInto:(uint32_t *)pixels lastSeen:(uint64_t)lastSeen;

/// Fraction of executed blocks that ran as recompiled code (diagnostics).
@property (nonatomic, readonly) double recompiledFraction;

@end

NS_ASSUME_NONNULL_END
