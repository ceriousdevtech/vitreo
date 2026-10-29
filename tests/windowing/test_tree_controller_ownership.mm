// SysTreeView32's AppKit model objects must own the collections they create.
//
// Regression: VtrTreeController and VtrTreeNode (compiled without ARC)
// assigned [NSMutableArray array] / [NSMutableDictionary dictionary] straight
// to their ivars. Nothing retained them, so they died with the autorelease
// pool that was current at -init, and the next treeview_insert_item sent
// -addObject: to freed memory: putty's settings dialog aborted with an
// unrecognized-selector exception. A dealloc sentinel attached to each
// collection makes the lifetime observable without touching freed memory.

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <gtest/gtest.h>
#include <windowing/cocoa/objc_helpers.h>

@interface VtrDeallocSentinel : NSObject
@property(nonatomic, assign) bool* flag;
@end
@implementation VtrDeallocSentinel
- (void)dealloc {
    if (_flag) *_flag = true;
    [super dealloc];
}
@end

namespace {

const char kSentinelKey = 0;

/// Sets *@p freed when @p object is deallocated.
void watch(id object, bool* freed) {
    VtrDeallocSentinel* sentinel = [[VtrDeallocSentinel alloc] init];
    sentinel.flag = freed;
    objc_setAssociatedObject(object, &kSentinelKey, sentinel,
                             OBJC_ASSOCIATION_RETAIN);
    [sentinel release];
}

TEST(CocoaTreeControllerTest, CollectionsOutliveThePoolTheyWereCreatedIn) {
    bool roots_freed = false;
    bool index_freed = false;
    bool children_freed = false;
    VtrTreeController* controller = nil;
    VtrTreeNode* node = nil;
    @autoreleasepool {
        controller = [[VtrTreeController alloc] init];
        node = [[VtrTreeNode alloc] init];
        watch(controller.roots, &roots_freed);
        watch(controller.nodeByItem, &index_freed);
        watch(node.children, &children_freed);
    }
    // The pool has drained: only their owners keep the collections alive.
    EXPECT_FALSE(roots_freed);
    EXPECT_FALSE(index_freed);
    EXPECT_FALSE(children_freed);

    // treeview_insert_item's sequence, after the drain.
    [controller.nodeByItem setObject:node forKey:@(1)];
    [controller.roots addObject:node];
    EXPECT_EQ(controller.roots.count, 1u);
    EXPECT_EQ([controller.nodeByItem objectForKey:@(1)], node);

    // Releasing the owners frees what they own.
    [controller release];
    EXPECT_TRUE(roots_freed);
    EXPECT_TRUE(index_freed);
    EXPECT_FALSE(children_freed);  // the node still holds its +1
    [node release];
    EXPECT_TRUE(children_freed);
}

}  // namespace
