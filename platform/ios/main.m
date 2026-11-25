// iOS host: full-screen Metal view; tap toggles aspect fit / aspect fill (never stretched),
// drag pans the cropped picture in fill mode;
// pauses when inactive.
#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import "../apple/SotaEngine.h"
#import "../apple/SotaRenderer.h"

@interface SotaViewController : UIViewController
@property (strong) SotaRenderer *renderer;
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
    self.view = v;
}

- (void)toggleZoom:(UITapGestureRecognizer *)g
{
    if (g.state != UIGestureRecognizerStateEnded) return;
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
- (void)sceneDidBecomeActive:(UIScene *)scene { (void)scene; [[SotaEngine shared] resume]; }
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
