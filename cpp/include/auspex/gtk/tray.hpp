// The system tray widget and its D-Bus plumbing.
//
// Auspex is a StatusNotifierHost, and a StatusNotifierWatcher only when nobody else
// is. That split is not defensive coding, it is what the protocol asks for and what
// makes the tray work in both of Auspex's lives:
//
//   * Running as a panel inside an existing Xfce session, xfce4-panel already owns
//     org.kde.StatusNotifierWatcher. Auspex registers as a host with it and both
//     panels show the same icons. Trying to take the name here would fail, and
//     taking it by force would break the other panel's tray.
//
//   * Running as the session's own desktop, nothing owns it, so Auspex must -- or
//     applications have nowhere to register and no icons appear at all.
//
// Ownership can also change under us: quit xfce4-panel and the name is free, so the
// watcher is re-attempted rather than resolved once at startup.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <giomm/menu.h>
#include <giomm/simpleactiongroup.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/image.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/revealer.h>
#include <gtkmm/togglebutton.h>

#include <gio/gio.h>

#include "auspex/tray.hpp"

namespace auspex::gtk {

// One application's icon in the tray.
class TrayIcon : public Gtk::Button {
public:
    TrayIcon(GDBusConnection* bus, TrayItemAddress address, int icon_size,
             bool panel_at_top);
    ~TrayIcon() override;

    const TrayItemAddress& address() const { return address_; }

    // Re-reads every property. Called on creation and whenever the item signals
    // that something about it changed.
    void refresh();

private:
    void apply_icon();
    void apply_tooltip();
    void open_menu(double x, double y);
    void activate_item(const char* method, int x, int y);

    // Property reads are synchronous but always time-limited. A tray icon belongs
    // to somebody else's process, and a panel that blocks forever because an
    // application wedged is a panel that takes the desktop down with it.
    std::string  read_string(const char* property);
    std::int32_t read_int(const char* property);

    GDBusConnection* bus_ = nullptr;
    TrayItemAddress  address_;
    int              icon_size_ = 22;

    Gtk::Image      image_;
    Gtk::PopoverMenu menu_;

    std::string  icon_name_;
    std::string  attention_icon_name_;
    std::string  icon_theme_path_;
    std::string  title_;
    std::string  id_;
    std::string  menu_path_;
    bool         item_is_menu_ = false;
    TrayStatus   status_ = TrayStatus::Active;

    // Subscriptions to the item's own change signals, unsubscribed on destruction:
    // the item outlives this widget when an application merely changes its icon,
    // and a stale subscription would fire into freed memory.
    std::vector<guint> subscriptions_;

    // The menu model is rebuilt on every open. dbusmenu is explicitly a
    // request-on-demand protocol -- AboutToShow exists precisely because
    // applications populate their menus lazily -- so a cached layout goes stale.
    Glib::RefPtr<Gio::Menu> model_;
    Glib::RefPtr<Gio::SimpleActionGroup> actions_;
    int next_action_ = 0;
};

// One Mint-style status icon.
//
// Much simpler than its SNI cousin: there is no menu to fetch and no pixmap to
// decode, only a name, a tooltip, and clicks reported straight back to the
// application, which puts up its own menu.
class XAppTrayIcon : public Gtk::Button {
public:
    XAppTrayIcon(GDBusConnection* bus, std::string service, std::string path,
                 int icon_size, bool panel_at_top);
    ~XAppTrayIcon() override;

    const std::string& service() const { return service_; }
    const std::string& path() const { return path_; }

    void refresh();

private:
    void report_click(unsigned button);

    GDBusConnection* bus_ = nullptr;
    std::string      service_;
    std::string      path_;
    int              icon_size_;
    bool             panel_at_top_;

    Gtk::Image image_;
    guint      subscription_ = 0;
};

// The tray itself: a toggle, and the icons behind it.
//
// Collapsed by default and collapsible by clicking the arrow, because a tray is
// mostly things you are not currently doing anything about -- and on a panel shared
// with a window list, permanent space for six idle icons is space taken from the
// thing you are actually using.
class SystemTray : public Gtk::Box {
public:
    explicit SystemTray(int icon_size = 22, bool panel_at_top = true);
    ~SystemTray() override;

private:
    void connect_bus();
    void become_host();
    void try_become_watcher();
    void sync_items();
    void add_item(const std::string& registered);
    void remove_item(const std::string& service);
    void update_toggle();

    // The second protocol. Mint's own applications are here and nowhere else.
    void sync_xapp_services();
    void add_xapp_service(const std::string& service);
    void remove_xapp_service(const std::string& service);

    static void on_name_signal(GDBusConnection* bus, const gchar* sender,
                               const gchar* path, const gchar* interface,
                               const gchar* signal, GVariant* parameters,
                               gpointer user_data);

    GDBusConnection* bus_ = nullptr;
    guint            host_name_id_    = 0;
    guint            watcher_name_id_ = 0;
    guint            watcher_object_id_ = 0;
    std::vector<guint> subscriptions_;

    int icon_size_;

    Gtk::ToggleButton toggle_;
    Gtk::Image        toggle_icon_;
    Gtk::Revealer     revealer_;
    Gtk::Box          icons_{Gtk::Orientation::HORIZONTAL, 2};

    bool panel_at_top_ = true;

    std::map<std::string, std::unique_ptr<TrayIcon>> items_;
    // Keyed by service, then by object path: one Mint service commonly publishes
    // several icons (mintreport's tray carries both its own and the process
    // monitor's), so a service is not an icon.
    std::map<std::string, std::map<std::string, std::unique_ptr<XAppTrayIcon>>> xapp_;
};

}  // namespace auspex::gtk
