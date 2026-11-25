#import "SotaCredits.h"

@implementation SotaCreditLine
+ (instancetype)name:(NSString *)n role:(NSString *)r { SotaCreditLine *c = [self new]; c.name = n; c.role = r; return c; }
@end

@implementation SotaLink
+ (instancetype)title:(NSString *)t url:(NSString *)u { SotaLink *l = [self new]; l.title = t; l.url = [NSURL URLWithString:u]; return l; }
@end

@implementation SotaCredits
+ (NSString *)title { return @"State of the Art"; }
+ (NSString *)subtitle { return @"Spaceballs · 1992"; }
+ (NSString *)award { return @"1st place, Amiga demo competition, The Party 1992 (Denmark)"; }
+ (NSArray<SotaCreditLine *> *)credits
{
    return @[ [SotaCreditLine name:@"Lone Starr" role:@"Code"],
              [SotaCreditLine name:@"Major Asshole" role:@"Code"],
              [SotaCreditLine name:@"Travolta" role:@"Music"],
              [SotaCreditLine name:@"TMB Designs" role:@"Graphics"] ];
}
+ (NSString *)editionHeading { return @"About this edition"; }
+ (NSString *)editionText
{
    return @"This is the original party release, running natively. Its Amiga 500 machine code was "
           @"statically recompiled to C and runs on a cycle-driven model of the Amiga's custom chips, "
           @"reproducing the demo frame by frame. No Kickstart ROM or network connection is needed.";
}
+ (NSArray<SotaLink *> *)links
{
    return @[ [SotaLink title:@"State of the Art on Pouët" url:@"https://www.pouet.net/prod.php?which=99"],
              [SotaLink title:@"State of the Art on Demozoo" url:@"https://demozoo.org/productions/2/"],
              [SotaLink title:@"Source code on GitHub" url:@"https://github.com/Buyukcaglar/StateOfTheArt"] ];
}
+ (NSString *)legal
{
    return @"State of the Art © 1992 Spaceballs and its authors. "
           @"Recompilation and app © 2026 Onur Büyükçağlar, MIT licensed.";
}
+ (NSString *)versionString
{
    NSDictionary *i = NSBundle.mainBundle.infoDictionary;
    return [NSString stringWithFormat:@"Version %@ (%@)", i[@"CFBundleShortVersionString"] ?: @"1.0", i[@"CFBundleVersion"] ?: @"1"];
}
@end
