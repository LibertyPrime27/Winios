/* Launch our own app extension directly, without the share sheet.
 *
 * NSExtension is private API, but it is what LiveContainer uses to start
 * LiveProcess, and it is the only way an app can bring up one of its extensions
 * on demand. It also gives us the one thing the share sheet cannot: a callback
 * when the extension process is terminated -- which for the memory ladder is
 * the measurement itself, not an error.
 *
 * This is the same mechanism a wineserver-as-extension would rely on, so
 * proving it here is not a detour.
 */
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^ExtLaunchEvent)(NSString *event, NSString *detail);

@interface ExtensionLauncher : NSObject

/// Is the extension actually inside this bundle? Sideloading tools strip
/// PlugIns by default on free accounts, and the failure looks like "it just
/// isn't in the share sheet" with no other symptom.
+ (BOOL)extensionEmbedded:(NSString *)bundleID path:(NSString * _Nullable * _Nullable)outPath;

/// Start the extension. Events: "launched" (with pid), "interrupted" (the
/// system killed it -- the ladder hit the limit), "completed", "cancelled",
/// "error".
+ (void)launch:(NSString *)bundleID onEvent:(ExtLaunchEvent)onEvent;

@end

NS_ASSUME_NONNULL_END
