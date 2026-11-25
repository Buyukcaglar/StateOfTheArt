// iOS host: full-screen Metal view; tap toggles aspect fit / aspect fill (never stretched),
// drag pans the cropped picture in fill mode; ⓘ (shown on tap) or long-press opens the info card;
// pauses when inactive.
#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import "../apple/SotaEngine.h"
#import "../apple/SotaRenderer.h"
#import "SotaInfoViewController.h"

@interface SotaViewController : UIViewController <UIGestureRecognizerDelegate>
@property (strong) SotaRenderer *renderer;
@property (strong) UIButton *infoButton;
@property (nonatomic, readonly) BOOL infoShown;
@end

static NSString *const kFillKey = @"SotaFillsView";

@implementation SotaViewController

- (void)loadView
{
    MTKView *v = [[MTKView alloc] initWithFrame:UIScreen.mainScreen.bounds device:MTLCreateSystemDefaultDevice()];
    v.preferredFramesPerSecond = 120;
    v.backgroundColor = UIColor.blackColor;
    v.multipleTouchEnabled = NO;
    self.renderer = [[SotaRenderer alloc] initWithView:v];
    [self.renderer setFillsView:[NSUserDefaults.standardUserDefaults boolForKey:kFillKey] animated:NO];
    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(toggleZoom:)];
    [v addGestureRecognizer:tap];
    UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(panView:)];
    pan.maximumNumberOfTouches = 1;
    [v addGestureRecognizer:pan];
    UILongPressGestureRecognizer *hold = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(longPress:)];
    [v addGestureRecognizer:hold];
    tap.delegate = self;

    // info button: hidden; a tap shows it for a few seconds, like video player controls
    UIButtonConfiguration *cfg = [UIButtonConfiguration plainButtonConfiguration];
    cfg.image = [UIImage systemImageNamed:@"info.circle.fill"
                        withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:28]];
    cfg.baseForegroundColor = [UIColor colorWithWhite:1 alpha:0.85];
    cfg.contentInsets = NSDirectionalEdgeInsetsMake(12, 12, 12, 12);
    UIButton *info = [UIButton buttonWithConfiguration:cfg primaryAction:nil];
    [info addTarget:self action:@selector(showInfo) forControlEvents:UIControlEventPrimaryActionTriggered];
    info.accessibilityLabel = @"About State of the Art";
    info.alpha = 0;
    info.layer.shadowColor = UIColor.blackColor.CGColor;
    info.layer.shadowOpacity = 0.6;
    info.layer.shadowRadius = 4;
    info.layer.shadowOffset = CGSizeZero;
    info.translatesAutoresizingMaskIntoConstraints = NO;
    [v addSubview:info];
    [NSLayoutConstraint activateConstraints:@[
        [info.trailingAnchor constraintEqualToAnchor:v.safeAreaLayoutGuide.trailingAnchor constant:-4],
        [info.bottomAnchor constraintEqualToAnchor:v.safeAreaLayoutGuide.bottomAnchor constant:-4],
    ]];
    self.infoButton = info;
    self.view = v;
}

static const NSTimeInterval kInfoVisible = 3.0;

- (void)revealInfoButton
{
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(hideInfoButton) object:nil];
    [UIView animateWithDuration:0.2 delay:0 options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
                     animations:^{ self.infoButton.alpha = 1; } completion:nil];
    // VoiceOver users need time to reach it: keep it up while VoiceOver runs
    if (!UIAccessibilityIsVoiceOverRunning())
        [self performSelector:@selector(hideInfoButton) withObject:nil afterDelay:kInfoVisible];
}

- (void)hideInfoButton
{
    [UIView animateWithDuration:0.4 delay:0 options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
                     animations:^{ self.infoButton.alpha = 0; } completion:nil];
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)g shouldReceiveTouch:(UITouch *)t
{
    (void)g;
    return ![t.view isDescendantOfView:self.infoButton];   // tapping ⓘ must not also toggle the zoom
}

- (void)longPress:(UILongPressGestureRecognizer *)g
{
    if (g.state == UIGestureRecognizerStateBegan) [self showInfo];
}

- (void)showInfo
{
    if (_infoShown) return;
    _infoShown = YES;
    [self hideInfoButton];
    SotaEngine *e = [SotaEngine shared];
    BOOL wasPaused = e.paused;
    [e pause];
    SotaInfoViewController *vc = [SotaInfoViewController new];
    __weak typeof(self) weakSelf = self;
    vc.onDismiss = ^{
        SotaViewController *me = weakSelf;
        if (!me || !me->_infoShown) return;
        me->_infoShown = NO;
        if (!wasPaused) [[SotaEngine shared] resume];
    };
    UISheetPresentationController *sheet = vc.sheetPresentationController;
    sheet.detents = @[ UISheetPresentationControllerDetent.mediumDetent, UISheetPresentationControllerDetent.largeDetent ];
    sheet.prefersGrabberVisible = YES;
    sheet.prefersEdgeAttachedInCompactHeight = YES;
    sheet.widthFollowsPreferredContentSizeWhenEdgeAttached = YES;
    [self presentViewController:vc animated:YES completion:nil];
}

- (void)toggleZoom:(UITapGestureRecognizer *)g
{
    if (g.state != UIGestureRecognizerStateEnded) return;
    [self revealInfoButton];
    BOOL fill = !self.renderer.fillsView;
    [self.renderer setFillsView:fill animated:YES];
    [NSUserDefaults.standardUserDefaults setBool:fill forKey:kFillKey];
}

- (void)panView:(UIPanGestureRecognizer *)g
{
    MTKView *v = (MTKView *)self.view;
    CGPoint d = [g translationInView:v];
    [g setTranslation:CGPointZero inView:v];
    [self.renderer panBy:d view:v];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    static BOOL started;
    if (started) return;
    started = YES;
    NSError *err = nil;
    if (!self.renderer || ![[SotaEngine shared] startWithError:&err]) {
        UIAlertController *a = [UIAlertController alertControllerWithTitle:@"State of the Art could not start"
            message:err.localizedDescription ?: @"Metal is not available." preferredStyle:UIAlertControllerStyleAlert];
        [self presentViewController:a animated:YES completion:nil];
        return;
    }
    // testing aid: `-SotaShowInfoAtLaunch YES` opens the info card at once
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"SotaShowInfoAtLaunch"]) {
        [self showInfo];
        if ([NSUserDefaults.standardUserDefaults boolForKey:@"SotaShowInfoLarge"])
            ((UIViewController *)self.presentedViewController).sheetPresentationController.selectedDetentIdentifier =
                UISheetPresentationControllerDetentIdentifierLarge;
    }
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }


@end

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o
{
    (void)s; (void)o;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.backgroundColor = UIColor.blackColor;
    self.window.rootViewController = [SotaViewController new];
    [self.window makeKeyAndVisible];
}
- (void)sceneWillResignActive:(UIScene *)scene { (void)scene; [[SotaEngine shared] pause]; }
- (void)sceneDidBecomeActive:(UIScene *)scene
{
    (void)scene;
    SotaViewController *vc = (SotaViewController *)self.window.rootViewController;
    if (!vc.infoShown) [[SotaEngine shared] resume];     // the info card keeps the demo paused
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o
{
    (void)a; (void)o;
    [UIApplication sharedApplication].idleTimerDisabled = YES;   // it's a show: keep the screen on
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)a configurationForConnectingSceneSession:(UISceneSession *)s
                              options:(UISceneConnectionOptions *)o
{
    (void)a; (void)o;
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:s.role];
    c.delegateClass = SceneDelegate.class;
    return c;
}
@end

int main(int argc, char *argv[])
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
