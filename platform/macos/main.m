// macOS host: one window with the Metal view, a small menu bar, mouse = Amiga left button.
#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>
#import "../apple/SotaEngine.h"
#import "../apple/SotaRenderer.h"

@interface SotaView : MTKView
@end

@implementation SotaView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (void)mouseDown:(NSEvent *)e { (void)e; [[SotaEngine shared] setMouseButton:YES]; }
- (void)mouseUp:(NSEvent *)e { (void)e; [[SotaEngine shared] setMouseButton:NO]; }
- (void)keyDown:(NSEvent *)e
{
    if (e.keyCode == 53 && (self.window.styleMask & NSWindowStyleMaskFullScreen)) [self.window toggleFullScreen:nil];
    else if (e.keyCode == 49) [[SotaEngine shared] setMouseButton:YES];   // space = fire
    else [super keyDown:e];
}
- (void)keyUp:(NSEvent *)e
{
    if (e.keyCode == 49) [[SotaEngine shared] setMouseButton:NO];
    else [super keyUp:e];
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@property (strong) SotaRenderer *renderer;
@end

@implementation AppDelegate

static NSMenuItem *item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mods)
{
    NSMenuItem *i = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    i.keyEquivalentModifierMask = mods;
    return i;
}

- (void)buildMenu
{
    NSString *name = @"State of the Art";
    NSMenu *bar = [NSMenu new];

    NSMenu *app = [[NSMenu alloc] initWithTitle:name];
    [app addItem:item([@"About " stringByAppendingString:name], @selector(orderFrontStandardAboutPanel:), @"", 0)];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:item([@"Hide " stringByAppendingString:name], @selector(hide:), @"h", NSEventModifierFlagCommand)];
    [app addItem:item(@"Hide Others", @selector(hideOtherApplications:), @"h", NSEventModifierFlagCommand | NSEventModifierFlagOption)];
    [app addItem:item(@"Show All", @selector(unhideAllApplications:), @"", 0)];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:item([@"Quit " stringByAppendingString:name], @selector(terminate:), @"q", NSEventModifierFlagCommand)];

    NSMenu *demo = [[NSMenu alloc] initWithTitle:@"Demo"];
    [demo addItem:item(@"Restart", @selector(restartDemo:), @"r", NSEventModifierFlagCommand)];
    [demo addItem:item(@"Pause", @selector(togglePause:), @"p", NSEventModifierFlagCommand)];
    [demo addItem:item(@"Mute", @selector(toggleMute:), @"m", NSEventModifierFlagCommand | NSEventModifierFlagShift)];

    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [view addItem:item(@"Actual Size (2×)", @selector(actualSize:), @"0", NSEventModifierFlagCommand)];
    [view addItem:item(@"Enter Full Screen", @selector(toggleFullScreen:), @"f", NSEventModifierFlagCommand | NSEventModifierFlagControl)];

    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    [win addItem:item(@"Minimize", @selector(performMiniaturize:), @"m", NSEventModifierFlagCommand)];
    [win addItem:item(@"Zoom", @selector(performZoom:), @"", 0)];
    NSApp.windowsMenu = win;

    for (NSMenu *m in @[app, demo, view, win]) {
        NSMenuItem *top = [NSMenuItem new];
        top.submenu = m;
        [bar addItem:top];
    }
    NSApp.mainMenu = bar;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    (void)n;
    [self buildMenu];
    NSRect frame = NSMakeRect(0, 0, 1056, 852);   // 1.5x the 704x568 display
    self.window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"State of the Art";
    self.window.contentMinSize = NSMakeSize(352, 284);
    self.window.contentAspectRatio = NSMakeSize(704, 568);
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.backgroundColor = NSColor.blackColor;
    self.window.delegate = self;
    self.window.frameAutosaveName = @"MainWindow";

    SotaView *view = [[SotaView alloc] initWithFrame:frame device:MTLCreateSystemDefaultDevice()];
    view.preferredFramesPerSecond = 120;
    self.renderer = [[SotaRenderer alloc] initWithView:view];
    self.window.contentView = view;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:view];

    NSError *err = nil;
    if (!self.renderer || ![[SotaEngine shared] startWithError:&err]) {
        NSAlert *a = [NSAlert new];
        a.messageText = @"State of the Art could not start";
        a.informativeText = err.localizedDescription ?: @"Metal is not available on this Mac.";
        [a runModal];
        [NSApp terminate:nil];
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)a { (void)a; return YES; }

- (void)applicationDidHide:(NSNotification *)n { (void)n; [[SotaEngine shared] pause]; }
- (void)applicationDidUnhide:(NSNotification *)n { (void)n; [[SotaEngine shared] resume]; }
- (void)windowDidMiniaturize:(NSNotification *)n { (void)n; [[SotaEngine shared] pause]; }
- (void)windowDidDeminiaturize:(NSNotification *)n { (void)n; [[SotaEngine shared] resume]; }

- (void)restartDemo:(id)s { (void)s; [[SotaEngine shared] restart]; }
- (void)togglePause:(id)s
{
    (void)s;
    SotaEngine *e = [SotaEngine shared];
    if (e.paused) [e resume]; else [e pause];
}
- (void)toggleMute:(id)s { (void)s; SotaEngine *e = [SotaEngine shared]; e.muted = !e.muted; }
- (void)actualSize:(id)s
{
    (void)s;
    if (self.window.styleMask & NSWindowStyleMaskFullScreen) return;
    NSRect r = [self.window frameRectForContentRect:NSMakeRect(0, 0, 704, 568)];
    r.origin = self.window.frame.origin;
    [self.window setFrame:r display:YES animate:YES];
}

- (BOOL)validateMenuItem:(NSMenuItem *)m
{
    SotaEngine *e = [SotaEngine shared];
    if (m.action == @selector(togglePause:)) m.title = e.paused ? @"Resume" : @"Pause";
    if (m.action == @selector(toggleMute:)) m.state = e.muted ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

@end

int main(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *d = [AppDelegate new];
        app.delegate = d;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
