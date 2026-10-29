// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file cocoa_control_delegates.h
/// @brief The delegate and data-source classes standard controls are wired to.
///
/// Private to the Cocoa adapter. Each is the object AppKit calls back into when
/// something happens to a control, and each carries the Win32 identity —
/// parent HWND, control HWND, control ID — needed to turn that callback into
/// the WM_COMMAND or WM_NOTIFY the guest is waiting for. That pairing is the
/// whole reason they exist: AppKit reports to an object, Win32 reports to a
/// window, and something has to hold both.
///
/// Only the @interface blocks live here. Every @implementation is in
/// cocoa_control_handlers.mm or cocoa_folder_picker.mm (both split out of
/// cocoa_bridge.mm); this header exists so cocoa_control_classes.mm and
/// cocoa_control_props.mm, which construct these objects, can see their shape.
/// An ObjC class has external linkage either way — it is the declaration that
/// does not cross a translation unit, not the class.

#import <Cocoa/Cocoa.h>

#include "windowing/cocoa/cocoa_bridge.hpp"

#include <vector>

// =============================================================================
// VtrEditHandler — posts WM_COMMAND(EN_CHANGE) when an EDIT's text changes
// =============================================================================
// Win32 apps track edit-field values via EN_CHANGE notifications as the user
// types, not by reading the control on OK. Without this, the typed text never
// reaches the dialog and its confirm action silently does nothing.
@interface VtrEditHandler : NSObject <NSTextFieldDelegate, NSComboBoxDelegate>
@property (nonatomic, assign) vitreo::windowing::HWND parentHwnd;
@property (nonatomic, assign) vitreo::windowing::HWND controlHwnd;
@property (nonatomic, assign) uint32_t controlId;
@property (nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@end

// =============================================================================
// VtrRichEditScrollHandler — posts WM_COMMAND(EN_VSCROLL) when the user scrolls
// =============================================================================
// Installer EULA dialogs enable Agree after GetScrollInfo shows the caret at the
// document end; they typically re-check on EN_VSCROLL / WM_VSCROLL.
@interface VtrRichEditScrollHandler : NSObject
@property (nonatomic, assign) vitreo::windowing::HWND parentHwnd;
@property (nonatomic, assign) vitreo::windowing::HWND controlHwnd;
@property (nonatomic, assign) uint32_t controlId;
@property (nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@end

@interface VtrFolderNode : NSObject
@property (nonatomic, strong) NSString* name;           ///< Display name
@property (nonatomic, strong) NSString* windowsPath;    ///< Full Windows path (e.g., "C:\Users")
@property (nonatomic, assign) BOOL isExpandable;        ///< Has children (directories)
@property (nonatomic, strong) NSMutableArray* children; ///< Child nodes (populated on expand)
@property (nonatomic, assign) BOOL childrenLoaded;      ///< Whether children have been loaded
@end

@interface VtrFolderPickerDataSource : NSObject <NSOutlineViewDataSource, NSOutlineViewDelegate>
@property (nonatomic, strong) NSMutableArray* rootNodes;
@property (nonatomic, copy) std::vector<vitreo::windowing::CocoaBridge::VfsDirEntry> (^listCallback)(NSString*);
@property (nonatomic, copy) BOOL (^createDirCallback)(NSString*);
@property (nonatomic, assign) NSTextField* pathLabel;
@property (nonatomic, assign) NSOutlineView* outlineView;
@property (nonatomic, strong) NSString* selectedPath;
- (void)loadChildrenForNode:(VtrFolderNode*)node;
- (void)makeNewFolderClicked:(id)sender;
- (void)okClicked:(id)sender;
- (void)cancelClicked:(id)sender;
@end

// =============================================================================
// VtrListBoxController — NSTableView data source/delegate for Win32 LISTBOX.
// Reads item text from the bridge's backing store and posts LBN_SELCHANGE /
// LBN_DBLCLK as WM_COMMAND on selection (so e.g. Saved Sessions can be loaded).
// =============================================================================
@interface VtrListBoxController : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property (nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@property (nonatomic, assign) vitreo::windowing::HWND hwnd;     // listbox control
@property (nonatomic, assign) vitreo::windowing::HWND parent;   // owning dialog
@property (nonatomic, assign) uint32_t ctrlId;
@property (nonatomic, assign) vitreo::windowing::MessageQueue* queue;
@property (nonatomic, assign) BOOL suppressNotify;
@end
