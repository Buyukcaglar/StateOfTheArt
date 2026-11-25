// Credits and info text shared by the iOS info card and the macOS About window.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface SotaCreditLine : NSObject
@property (nonatomic, copy) NSString *name, *role;
@end

@interface SotaLink : NSObject
@property (nonatomic, copy) NSString *title;
@property (nonatomic, copy) NSURL *url;
@end

@interface SotaCredits : NSObject
@property (class, readonly) NSString *title;            // "State of the Art"
@property (class, readonly) NSString *subtitle;         // "Spaceballs · 1992"
@property (class, readonly) NSString *award;
@property (class, readonly) NSArray<SotaCreditLine *> *credits;
@property (class, readonly) NSString *editionHeading;
@property (class, readonly) NSString *editionText;
@property (class, readonly) NSArray<SotaLink *> *links;
@property (class, readonly) NSString *legal;
@property (class, readonly) NSString *versionString;    // "Version 1.0 (1)"
@end

NS_ASSUME_NONNULL_END
