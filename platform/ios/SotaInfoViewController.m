#import "SotaInfoViewController.h"
#import "../apple/SotaCredits.h"

@interface SotaInfoViewController () <UIAdaptivePresentationControllerDelegate>
@end

@implementation SotaInfoViewController

static UILabel *label(NSString *text, UIFont *font, UIColor *color)
{
    UILabel *l = [UILabel new];
    l.text = text;
    l.font = font;
    l.textColor = color;
    l.numberOfLines = 0;
    l.adjustsFontForContentSizeCategory = YES;
    return l;
}

static UIFont *styled(UIFontTextStyle style, UIFontWeight weight)
{
    UIFont *base = [UIFont preferredFontForTextStyle:style];
    return [[UIFontMetrics metricsForTextStyle:style] scaledFontForFont:[UIFont systemFontOfSize:base.pointSize weight:weight]];
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.presentationController.delegate = self;

    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.alwaysBounceVertical = YES;
    [self.view addSubview:scroll];

    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 6;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];

    // header: artwork (frame 2622, the app icon's source)
    UIImageView *art = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"InfoArt"]];
    art.contentMode = UIViewContentModeScaleAspectFill;
    art.clipsToBounds = YES;
    art.layer.cornerRadius = 14;
    art.layer.magnificationFilter = kCAFilterNearest;
    art.backgroundColor = UIColor.blackColor;
    art.isAccessibilityElement = YES;
    art.accessibilityLabel = @"A dancing silhouette from the demo";
    [art.heightAnchor constraintEqualToAnchor:art.widthAnchor multiplier:568.0 / 704.0].active = YES;
    [stack addArrangedSubview:art];
    [stack setCustomSpacing:18 afterView:art];

    UILabel *title = label(SotaCredits.title, styled(UIFontTextStyleLargeTitle, UIFontWeightBold), UIColor.labelColor);
    title.accessibilityTraits = UIAccessibilityTraitHeader;
    [stack addArrangedSubview:title];
    [stack addArrangedSubview:label(SotaCredits.subtitle, styled(UIFontTextStyleTitle3, UIFontWeightRegular), UIColor.secondaryLabelColor)];
    UILabel *award = label(SotaCredits.award, styled(UIFontTextStyleSubheadline, UIFontWeightRegular), UIColor.secondaryLabelColor);
    [stack addArrangedSubview:award];
    [stack setCustomSpacing:24 afterView:award];

    [stack addArrangedSubview:[self heading:@"Credits" in:stack]];
    for (SotaCreditLine *c in SotaCredits.credits) {
        UILabel *name = label(c.name, styled(UIFontTextStyleBody, UIFontWeightSemibold), UIColor.labelColor);
        UILabel *role = label(c.role, [UIFont preferredFontForTextStyle:UIFontTextStyleBody], UIColor.secondaryLabelColor);
        role.textAlignment = NSTextAlignmentRight;
        [role setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[ name, role ]];
        row.spacing = 12;
        row.isAccessibilityElement = YES;
        row.accessibilityLabel = [NSString stringWithFormat:@"%@, %@", c.name, c.role];
        [stack addArrangedSubview:row];
    }
    [stack setCustomSpacing:24 afterView:stack.arrangedSubviews.lastObject];

    [stack addArrangedSubview:[self heading:SotaCredits.editionHeading in:stack]];
    UILabel *edition = label(SotaCredits.editionText, [UIFont preferredFontForTextStyle:UIFontTextStyleBody], UIColor.labelColor);
    [stack addArrangedSubview:edition];
    [stack setCustomSpacing:20 afterView:edition];

    for (SotaLink *l in SotaCredits.links) {
        UIButtonConfiguration *cfg = [UIButtonConfiguration plainButtonConfiguration];
        cfg.title = l.title;
        cfg.image = [UIImage systemImageNamed:@"arrow.up.right.square"];
        cfg.imagePlacement = NSDirectionalRectEdgeTrailing;
        cfg.imagePadding = 6;
        cfg.contentInsets = NSDirectionalEdgeInsetsMake(8, 0, 8, 0);
        NSURL *url = l.url;
        UIButton *b = [UIButton buttonWithConfiguration:cfg primaryAction:[UIAction actionWithHandler:^(UIAction *a) {
            (void)a; [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil];
        }]];
        b.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
        [stack addArrangedSubview:b];
    }
    [stack setCustomSpacing:20 afterView:stack.arrangedSubviews.lastObject];

    [stack addArrangedSubview:label(SotaCredits.legal, [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote], UIColor.secondaryLabelColor)];
    [stack addArrangedSubview:label(SotaCredits.versionString, [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote], UIColor.tertiaryLabelColor)];

    UIButton *done = [UIButton buttonWithType:UIButtonTypeClose];
    done.translatesAutoresizingMaskIntoConstraints = NO;
    done.accessibilityLabel = @"Close";
    [done addTarget:self action:@selector(close) forControlEvents:UIControlEventPrimaryActionTriggered];
    [self.view addSubview:done];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    UILayoutGuide *content = scroll.contentLayoutGuide, *frame = scroll.frameLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:content.topAnchor constant:56],
        [stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-32],
        [stack.centerXAnchor constraintEqualToAnchor:frame.centerXAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:560],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:24],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-24],
        [done.topAnchor constraintEqualToAnchor:self.view.topAnchor constant:16],
        [done.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
    ]];
    // fill the sheet's width (minus margins), capped at a readable 560 pt on iPad
    NSLayoutConstraint *wide = [stack.widthAnchor constraintEqualToAnchor:safe.widthAnchor constant:-48];
    wide.priority = 999;
    wide.active = YES;
}

- (UILabel *)heading:(NSString *)text in:(UIStackView *)stack
{
    (void)stack;
    UILabel *h = label(text, styled(UIFontTextStyleTitle2, UIFontWeightBold), UIColor.labelColor);
    h.accessibilityTraits = UIAccessibilityTraitHeader;
    return h;
}

- (void)close
{
    [self dismissViewControllerAnimated:YES completion:^{ if (self.onDismiss) self.onDismiss(); }];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)p
{
    (void)p;
    if (self.onDismiss) self.onDismiss();
}

@end
