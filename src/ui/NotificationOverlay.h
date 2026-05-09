#pragma once

// NotificationOverlay — top-right toast stack drawn via the foreground draw
// list so it sits above modal popups and floating viewports. Anchored to the
// MAIN viewport (toasts shouldn't follow a floating chart).
//
// Single free function; per-frame state lives in a function-local static.

namespace core::services { class NotificationService; }

namespace ui {

void RenderNotificationOverlay(core::services::NotificationService& svc);

}   // namespace ui
