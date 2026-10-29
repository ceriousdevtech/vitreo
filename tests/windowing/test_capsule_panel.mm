// The Open/Save panel stays inside the capsule's drives (user decision
// 2026-10-26): items outside are disabled except folders above a drive root,
// and choosing outside is refused with an error the panel shows as an alert.

#import <Cocoa/Cocoa.h>
#include <gtest/gtest.h>

@interface VtrCapsulePanelDelegate : NSObject <NSOpenSavePanelDelegate>
- (instancetype)initWithRoots:(NSArray<NSString*>*)roots;
@end

namespace {

NSURL* url(NSString* path) { return [NSURL fileURLWithPath:path]; }

TEST(CapsulePanelDelegate, EnablesOnlyTheDrivesAndTheirAncestors) {
    NSString* root = [NSTemporaryDirectory() stringByAppendingPathComponent:@"vitreo_panel_root"];
    [[NSFileManager defaultManager] createDirectoryAtPath:[root stringByAppendingPathComponent:@"Users"]
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* resolved = [[[url(root) URLByResolvingSymlinksInPath] path] stringByStandardizingPath];
    VtrCapsulePanelDelegate* delegate =
        [[VtrCapsulePanelDelegate alloc] initWithRoots:@[resolved]];
    EXPECT_TRUE([delegate panel:nil shouldEnableURL:url(root)]);
    EXPECT_TRUE([delegate panel:nil shouldEnableURL:url([root stringByAppendingPathComponent:@"Users"])]);
    EXPECT_TRUE([delegate panel:nil shouldEnableURL:url([resolved stringByDeletingLastPathComponent])])
        << "a folder above the drive root stays navigable";
    EXPECT_FALSE([delegate panel:nil shouldEnableURL:url(@"/Applications")]);
    EXPECT_FALSE([delegate panel:nil shouldEnableURL:url([resolved stringByAppendingString:@"_sibling"])])
        << "a sibling sharing the root's name prefix is outside";

    NSError* error = nil;
    EXPECT_TRUE([delegate panel:nil validateURL:url([root stringByAppendingPathComponent:@"Users"]) error:&error]);
    EXPECT_FALSE([delegate panel:nil validateURL:url(@"/Applications") error:&error]);
    ASSERT_NE(error, nil);
    EXPECT_GT([[error localizedDescription] length], 0u);
    EXPECT_FALSE([delegate panel:nil validateURL:url([resolved stringByDeletingLastPathComponent]) error:&error])
        << "an ancestor is navigable but not a choice";
    [[NSFileManager defaultManager] removeItemAtPath:root error:nil];
}

}  // namespace
