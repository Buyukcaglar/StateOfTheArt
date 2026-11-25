// Shared Metal presenter for macOS and iOS.
#import <MetalKit/MetalKit.h>

NS_ASSUME_NONNULL_BEGIN
@interface SotaRenderer : NSObject <MTKViewDelegate>
- (nullable instancetype)initWithView:(MTKView *)view;
/// NO: whole picture visible, letterboxed (aspect fit). YES: picture covers the view,
/// overflow cropped (aspect fill). Never stretched. Animated when `animated`.
@property (nonatomic, readonly) BOOL fillsView;
- (void)setFillsView:(BOOL)fill animated:(BOOL)animated;
/// In fill mode, moves the picture by a drag delta given in view points, clamped so no
/// border shows. Ignored in fit mode. Entering fill starts centred; returning to fit
/// recentres (animated with the zoom).
- (void)panBy:(CGPoint)delta view:(MTKView *)view;
@end
NS_ASSUME_NONNULL_END
