// Credits / info card, presented as a sheet over the demo.
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface SotaInfoViewController : UIViewController
/// Called once the card is gone (Done button or swipe down).
@property (nonatomic, copy, nullable) void (^onDismiss)(void);
@end

NS_ASSUME_NONNULL_END
