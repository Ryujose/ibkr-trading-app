#pragma once

// NotificationsWindow — singleton history view for the in-app notification
// stream. Toasts disappear after their hold/fade timeline; this window keeps
// a scrollable record so the user can re-read what fired.

namespace core::services { class NotificationService; }

namespace ui {

class NotificationsWindow {
public:
    NotificationsWindow() = default;
    ~NotificationsWindow() = default;

    bool  Render(core::services::NotificationService& svc);
    bool& open() { return m_open; }

private:
    bool m_open = false;
    bool m_autoScroll = true;
};

}   // namespace ui
