#import "ExtensionLauncher.h"

/* Private Foundation API. Declared locally; linked dynamically at runtime. */
@interface NSExtension : NSObject
+ (instancetype)extensionWithIdentifier:(NSString *)identifier error:(NSError **)error;
- (void)beginExtensionRequestWithInputItems:(NSArray *)items
                                 completion:(void (^)(NSUUID *requestIdentifier))completion;
- (void)setRequestCompletionBlock:(void (^)(NSUUID *uuid, NSArray *extensionItems))block;
- (void)setRequestInterruptionBlock:(void (^)(NSUUID *uuid))block;
- (void)setRequestCancellationBlock:(void (^)(NSUUID *uuid, NSError *error))block;
- (int)pidForRequestIdentifier:(NSUUID *)uuid;
@property (nonatomic, copy) NSArray *preferredLanguages;
@end

@implementation ExtensionLauncher

static NSExtension *g_live;   /* keep the request alive; the blocks fire on it */

+ (BOOL)extensionEmbedded:(NSString *)bundleID path:(NSString **)outPath {
    NSURL *plugins = NSBundle.mainBundle.builtInPlugInsURL;
    if (!plugins) return NO;
    NSArray<NSURL *> *items = [NSFileManager.defaultManager
        contentsOfDirectoryAtURL:plugins includingPropertiesForKeys:nil options:0 error:nil];
    for (NSURL *u in items) {
        NSBundle *b = [NSBundle bundleWithURL:u];
        if ([b.bundleIdentifier isEqualToString:bundleID]) {
            if (outPath) *outPath = u.path;
            return YES;
        }
    }
    return NO;
}

+ (void)launch:(NSString *)bundleID onEvent:(ExtLaunchEvent)onEvent {
    Class cls = NSClassFromString(@"NSExtension");
    if (!cls) { onEvent(@"error", @"NSExtension class not available"); return; }

    NSError *err = nil;
    NSExtension *ext = [cls extensionWithIdentifier:bundleID error:&err];
    if (!ext) {
        onEvent(@"error", [NSString stringWithFormat:@"extensionWithIdentifier failed: %@",
                           err.localizedDescription ?: @"(no error)"]);
        return;
    }
    g_live = ext;

    __weak NSExtension *weak = ext;
    [ext setRequestInterruptionBlock:^(NSUUID *uuid) {
        /* The system terminated the extension. For the memory ladder this is
         * the result: the last rung it managed to fsync is the limit. */
        onEvent(@"interrupted", @"extension process was terminated by the system");
    }];
    [ext setRequestCompletionBlock:^(NSUUID *uuid, NSArray *items) {
        onEvent(@"completed", @"extension finished on its own (hit the ceiling, not a limit)");
    }];
    [ext setRequestCancellationBlock:^(NSUUID *uuid, NSError *e) {
        onEvent(@"cancelled", e.localizedDescription ?: @"cancelled");
    }];
    ext.preferredLanguages = @[];
    /* LiveContainer passes a real item with userInfo; an empty input array is
     * not guaranteed to be accepted by every extension point. */
    NSExtensionItem *item = [NSExtensionItem new];
    item.userInfo = @{ @"purpose": @"memprobe-ladder" };
    [ext beginExtensionRequestWithInputItems:@[item] completion:^(NSUUID *uuid) {
        if (!uuid) { onEvent(@"error", @"beginExtensionRequest returned no identifier -- process failed to start"); return; }
        int pid = [weak pidForRequestIdentifier:uuid];
        onEvent(@"launched", [NSString stringWithFormat:@"extension running, pid %d", pid]);
    }];
}

@end
