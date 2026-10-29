#pragma once

/// @file cocoa_perf_overlay.h
/// @brief Entry points of the performance overlay (cocoa_perf_overlay.mm)
///        for the rest of the Cocoa bridge. Main thread only.

/// Put the overlay above @p content_view (an NSView*, a window's content
/// view that has just gained a guest presentation view); a second call for
/// the same view does nothing.
void vtr_perf_overlay_attach(void* content_view);

/// True when @p ns_event (an NSEvent*) is the overlay's ⌥⌘P toggle or its
/// key-up: the caller drops it, so neither AppKit nor the guest sees it.
bool vtr_perf_overlay_consume_key_event(void* ns_event);

/// Add the overlay's item to @p view_menu (an NSMenu*) unless it has one.
void vtr_perf_overlay_add_menu_item(void* view_menu);
