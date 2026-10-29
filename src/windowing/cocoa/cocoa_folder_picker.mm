// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_folder_picker.mm
/// @brief The folder tree behind a folder-browse dialog.
///
/// VtrFolderNode and VtrFolderPickerDataSource back the NSOutlineView that
/// shows the virtual file system's folders for the guest to choose from.
///
/// Split out of cocoa_bridge.mm, which was 7,624 lines. The classes are
/// unchanged; only their translation unit is new.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/model/message_box_layout.hpp"
#include "windowing/model/diag_flags.hpp"
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_control_delegates.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/messages/input_latency.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/cocoa/window_presentation.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
#include "graphics/common/perf_diag.hpp"
#include "graphics/common/surface_presenter.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <limits>
#include <algorithm>
#include <CoreFoundation/CFRunLoop.h>
#include <Block.h>
#include <deque>
#include <mutex>
#include <atomic>
#include <CoreGraphics/CGEventSource.h>
#include <libproc.h>
#include <cerrno>
#include <signal.h>
#include <mach/mach_time.h>
#include "windowing/cocoa/cocoa_bridge_internal.h"

// =============================================================================
// VtrFolderNode — represents a folder in the VFS tree picker
// =============================================================================


@implementation VtrFolderNode
- (instancetype)initWithName:(NSString*)name path:(NSString*)path expandable:(BOOL)expandable {
    if (self = [super init]) {
        _name = name;
        _windowsPath = path;
        _isExpandable = expandable;
        _children = [[NSMutableArray alloc] init];
        _childrenLoaded = NO;
    }
    return self;
}
@end

// =============================================================================
// VtrFolderPickerDataSource — data source/delegate for VFS folder tree
// =============================================================================


@implementation VtrFolderPickerDataSource

- (instancetype)init {
    if (self = [super init]) {
        _rootNodes = [[NSMutableArray alloc] init];
        _selectedPath = @"";
    }
    return self;
}

- (void)loadChildrenForNode:(VtrFolderNode*)node {
    if (node.childrenLoaded) return;

    VITREO_TRACE( "[cocoa] loadChildrenForNode: path='%s'\n",
                 [node.windowsPath UTF8String]);

    if (_listCallback) {
        auto entries = _listCallback(node.windowsPath);
        VITREO_TRACE( "[cocoa] loadChildrenForNode: got %zu entries\n", entries.size());
        for (const auto& entry : entries) {
            VITREO_TRACE( "[cocoa]   entry: '%s' is_dir=%d\n",
                         entry.name.c_str(), entry.is_directory);
            if (entry.is_directory) {
                NSString* name = [NSString stringWithUTF8String:entry.name.c_str()];
                if (!name) continue;  // skip entries with invalid UTF-8
                NSString* childPath;
                if ([node.windowsPath hasSuffix:@"\\"]) {
                    childPath = [NSString stringWithFormat:@"%@%@", node.windowsPath, name];
                } else {
                    childPath = [NSString stringWithFormat:@"%@\\%@", node.windowsPath, name];
                }
                VtrFolderNode* child = [[VtrFolderNode alloc] initWithName:name
                                                                     path:childPath
                                                               expandable:YES];
                // Eagerly probe one level deeper so isItemExpandable returns the
                // correct value immediately — prevents phantom chevrons on first draw.
                [self loadChildrenForNode:child];
                [node.children addObject:child];
            }
        }
    }
    node.childrenLoaded = YES;
    // Update expandable flag to reflect whether subdirectories actually exist.
    // This prevents AppKit from calling child:index:ofItem: on a childless node.
    node.isExpandable = (node.children.count > 0);
}

#pragma mark - NSOutlineViewDataSource

- (NSInteger)outlineView:(NSOutlineView*)outlineView numberOfChildrenOfItem:(id)item {
    if (item == nil) {
        return (NSInteger)_rootNodes.count;
    }
    VtrFolderNode* node = (VtrFolderNode*)item;
    [self loadChildrenForNode:node];
    // If loading revealed no subdirectories, collapse the disclosure triangle
    // after AppKit finishes the current pass (cannot mutate outline view state
    // from within a data-source callback).
    if (node.children.count == 0) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [outlineView collapseItem:node];
        });
    }
    return (NSInteger)node.children.count;
}

- (id)outlineView:(NSOutlineView*)outlineView child:(NSInteger)index ofItem:(id)item {
    if (item == nil) {
        if ((NSUInteger)index >= _rootNodes.count) return nil;
        return _rootNodes[(NSUInteger)index];
    }
    VtrFolderNode* node = (VtrFolderNode*)item;
    [self loadChildrenForNode:node];
    if ((NSUInteger)index >= node.children.count) return nil;
    return node.children[(NSUInteger)index];
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item {
    VtrFolderNode* node = (VtrFolderNode*)item;
    // Return the known value if children have already been loaded.
    // If not yet loaded, return YES optimistically — NSOutlineView calling
    // this from within its own internal expand/reload machinery makes it
    // unsafe to trigger a blocking VFS load here (doing so corrupts the
    // outline view's row cache).  numberOfChildrenOfItem: will load on
    // demand and schedule a deferred collapse if the folder is empty.
    return node.childrenLoaded ? node.isExpandable : YES;
}

#pragma mark - NSOutlineViewDelegate

- (NSView*)outlineView:(NSOutlineView*)outlineView
    viewForTableColumn:(NSTableColumn*)tableColumn
                  item:(id)item {
    if (!item) return nil;  // guard against nil items from bounds-clamped child:index:ofItem:
    VtrFolderNode* node = (VtrFolderNode*)item;

    NSTableCellView* cell = [outlineView makeViewWithIdentifier:@"FolderCell" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] init];
        cell.identifier = @"FolderCell";

        // Folder icon — use Auto Layout so positions stay correct at every indent level
        NSImageView* imageView = [[NSImageView alloc] init];
        imageView.translatesAutoresizingMaskIntoConstraints = NO;
        imageView.imageScaling = NSImageScaleProportionallyDown;
        [cell addSubview:imageView];
        cell.imageView = imageView;

        // Text label
        NSTextField* textField = [[NSTextField alloc] init];
        textField.translatesAutoresizingMaskIntoConstraints = NO;
        textField.bordered = NO;
        textField.editable = NO;
        textField.drawsBackground = NO;
        textField.lineBreakMode = NSLineBreakByTruncatingTail;
        textField.font = [NSFont systemFontOfSize:[NSFont systemFontSize]];
        // Allow the text field to compress rather than push other views
        [textField setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [cell addSubview:textField];
        cell.textField = textField;

        // Constraints: icon leading-aligned, 16×16, vertically centred; text 4 pt
        // after icon and stretches to the trailing edge, also vertically centred.
        [NSLayoutConstraint activateConstraints:@[
            [imageView.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor],
            [imageView.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
            [imageView.widthAnchor constraintEqualToConstant:16],
            [imageView.heightAnchor constraintEqualToConstant:16],

            [textField.leadingAnchor constraintEqualToAnchor:imageView.trailingAnchor constant:4],
            [textField.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor],
            [textField.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }

    cell.imageView.image = [NSImage imageNamed:NSImageNameFolder];
    cell.textField.stringValue = node.name;
    return cell;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification {
    NSOutlineView* outlineView = notification.object;
    NSInteger row = outlineView.selectedRow;
    if (row >= 0) {
        VtrFolderNode* node = [outlineView itemAtRow:row];
        _selectedPath = node.windowsPath;
        if (_pathLabel) {
            _pathLabel.stringValue = node.windowsPath;
        }
    }
}

- (void)makeNewFolderClicked:(id)sender {
    (void)sender;
    
    // Need a selected folder to create subfolder in
    if (_selectedPath.length == 0) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Select a folder first";
        alert.informativeText = @"Please select a folder where you want to create the new folder.";
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
        return;
    }
    
    // Show input dialog for folder name
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Make New Folder";
    alert.informativeText = @"Enter a name for the new folder:";
    [alert addButtonWithTitle:@"Create"];
    [alert addButtonWithTitle:@"Cancel"];
    
    NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 250, 24)];
    input.stringValue = @"New Folder";
    alert.accessoryView = input;
    [alert.window makeFirstResponder:input];
    
    NSModalResponse response = [alert runModal];
    if (response != NSAlertFirstButtonReturn) {
        return;  // User cancelled
    }
    
    NSString* folderName = input.stringValue;
    if (folderName.length == 0) {
        return;
    }
    
    // Build the full path for the new folder
    NSString* newPath;
    if ([_selectedPath hasSuffix:@":"]) {
        newPath = [NSString stringWithFormat:@"%@\\%@", _selectedPath, folderName];
    } else {
        newPath = [NSString stringWithFormat:@"%@\\%@", _selectedPath, folderName];
    }
    
    // Create the directory via callback
    if (_createDirCallback && _createDirCallback(newPath)) {
        // Find the selected node and reload its children
        NSInteger selectedRow = _outlineView.selectedRow;
        if (selectedRow >= 0) {
            VtrFolderNode* parentNode = [_outlineView itemAtRow:selectedRow];
            // Reset children to force reload
            parentNode.childrenLoaded = NO;
            [parentNode.children removeAllObjects];
            [_outlineView reloadItem:parentNode reloadChildren:YES];
            [_outlineView expandItem:parentNode];
            
            // Try to select the new folder
            for (NSUInteger i = 0; i < parentNode.children.count; i++) {
                VtrFolderNode* child = parentNode.children[i];
                if ([child.name isEqualToString:folderName]) {
                    NSInteger newRow = [_outlineView rowForItem:child];
                    if (newRow >= 0) {
                        [_outlineView selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)newRow]
                              byExtendingSelection:NO];
                        [_outlineView scrollRowToVisible:newRow];
                    }
                    break;
                }
            }
        }
    } else {
        NSAlert* errorAlert = [[NSAlert alloc] init];
        errorAlert.messageText = @"Failed to create folder";
        errorAlert.informativeText = [NSString stringWithFormat:@"Could not create folder at %@", newPath];
        errorAlert.alertStyle = NSAlertStyleWarning;
        [errorAlert addButtonWithTitle:@"OK"];
        [errorAlert runModal];
    }
}

- (void)okClicked:(id)sender {
    VITREO_TRACE( "[cocoa] okClicked: selectedPath='%s'\n", [_selectedPath UTF8String]);
    [NSApp stopModalWithCode:NSModalResponseOK];
}

- (void)cancelClicked:(id)sender {
    VITREO_TRACE( "[cocoa] cancelClicked\n");
    [NSApp stopModalWithCode:NSModalResponseCancel];
}

@end
