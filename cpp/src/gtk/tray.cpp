#include "auspex/gtk/tray.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdio>

#include <giomm/menu.h>
#include <giomm/menuitem.h>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <gdkmm/pixbuf.h>
#include <gdkmm/texture.h>
#include <gtkmm/icontheme.h>

#include "auspex/desktop.hpp"

namespace auspex::gtk {

namespace {

constexpr const char* kWatcherName      = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath      = "/StatusNotifierWatcher";
constexpr const char* kWatcherInterface = "org.kde.StatusNotifierWatcher";
constexpr const char* kItemInterface    = "org.kde.StatusNotifierItem";
constexpr const char* kMenuInterface    = "com.canonical.dbusmenu";
constexpr const char* kXAppInterface    = "org.x.StatusIcon";
constexpr const char* kXAppRoot         = "/org/x/StatusIcon";

// Every call into another application is time-limited.
//
// These run on the GTK thread, and the process at the other end is one we do not
// control: a wedged tray application must cost a missing icon, never a frozen
// desktop. One second is far longer than a property read needs and short enough
// that a stall is a hiccup rather than a hang.
constexpr int kCallTimeoutMs = 1000;

// The watcher interface Auspex serves when it is the one providing it.
constexpr const char* kWatcherIntrospection = R"XML(
<node>
  <interface name="org.kde.StatusNotifierWatcher">
    <method name="RegisterStatusNotifierItem">
      <arg type="s" direction="in" name="service"/>
    </method>
    <method name="RegisterStatusNotifierHost">
      <arg type="s" direction="in" name="service"/>
    </method>
    <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
    <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
    <property name="ProtocolVersion" type="i" access="read"/>
    <signal name="StatusNotifierItemRegistered">
      <arg type="s" name="service"/>
    </signal>
    <signal name="StatusNotifierItemUnregistered">
      <arg type="s" name="service"/>
    </signal>
    <signal name="StatusNotifierHostRegistered"/>
  </interface>
</node>
)XML";

std::string variant_string(GVariant* value) {
    if (value == nullptr) return {};
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ||
        g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)) {
        return g_variant_get_string(value, nullptr);
    }
    return {};
}

// Reads one org.freedesktop.DBus.Properties value, unwrapping the variant the
// Properties interface wraps everything in.
GVariant* read_property(GDBusConnection* bus, const std::string& service,
                        const std::string& path, const char* interface,
                        const char* property) {
    if (bus == nullptr || service.empty() || path.empty()) return nullptr;

    GError*   error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus, service.c_str(), path.c_str(), "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", interface, property), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &error);

    if (error != nullptr) {
        // Expected and unremarkable: applications differ over which optional
        // properties they implement, and asking is how you find out.
        g_error_free(error);
        return nullptr;
    }
    if (reply == nullptr) return nullptr;

    GVariant* boxed = nullptr;
    g_variant_get(reply, "(v)", &boxed);
    g_variant_unref(reply);
    return boxed;
}

}  // namespace

// ---------------------------------------------------------------------------
// TrayIcon
// ---------------------------------------------------------------------------
TrayIcon::TrayIcon(GDBusConnection* bus, TrayItemAddress address, int icon_size,
                   bool panel_at_top)
    : bus_(bus), address_(std::move(address)), icon_size_(icon_size) {
    add_css_class("tray-icon");
    add_css_class("flat");
    set_has_frame(false);
    image_.set_pixel_size(icon_size_);
    set_child(image_);

    menu_.set_parent(*this);
    menu_.set_has_arrow(false);
    // Away from the bar's edge, or the menu is placed off the screen entirely.
    menu_.set_position(panel_at_top ? Gtk::PositionType::BOTTOM
                                    : Gtk::PositionType::TOP);

    actions_ = Gio::SimpleActionGroup::create();
    insert_action_group("tray", actions_);

    // Left click. The specification's Activate is what an application expects from
    // a plain click -- Steam raises its window, blueman opens its applet.
    //
    // Unless the item says it IS a menu, in which case activating it does nothing
    // at all and the menu is the only interface it has. Getting this wrong makes an
    // icon that appears completely inert.
    signal_clicked().connect([this] {
        if (item_is_menu_ || !menu_path_.empty()) {
            if (item_is_menu_) {
                open_menu(0, 0);
                return;
            }
        }
        activate_item("Activate", 0, 0);
    });

    // Right click opens the menu; middle click is the protocol's secondary action.
    {
        auto secondary = Gtk::GestureClick::create();
        secondary->set_button(GDK_BUTTON_SECONDARY);
        secondary->signal_pressed().connect(
            [this](int, double x, double y) { open_menu(x, y); });
        add_controller(secondary);

        auto middle = Gtk::GestureClick::create();
        middle->set_button(GDK_BUTTON_MIDDLE);
        middle->signal_pressed().connect(
            [this](int, double, double) { activate_item("SecondaryActivate", 0, 0); });
        add_controller(middle);
    }

    // Scrolling over a tray icon is how volume applets change the volume.
    {
        auto scroll = Gtk::EventControllerScroll::create();
        scroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
        scroll->signal_scroll().connect(
            [this](double dx, double dy) {
                const int delta = static_cast<int>(dy != 0 ? dy * 120 : dx * 120);
                const char* orientation = dy != 0 ? "vertical" : "horizontal";
                if (bus_ != nullptr && delta != 0) {
                    g_dbus_connection_call(
                        bus_, address_.service.c_str(), address_.path.c_str(),
                        kItemInterface, "Scroll",
                        g_variant_new("(is)", delta, orientation), nullptr,
                        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr,
                        nullptr);
                }
                return true;
            },
            false);
        add_controller(scroll);
    }

    // The item tells us when it changes rather than being polled. A tray that polled
    // would be asking half a dozen applications for their icon every second forever.
    for (const char* signal : {"NewIcon", "NewAttentionIcon", "NewToolTip",
                               "NewStatus", "NewTitle"}) {
        const guint id = g_dbus_connection_signal_subscribe(
            bus_, address_.service.c_str(), kItemInterface, signal,
            address_.path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
               const gchar*, GVariant*, gpointer user_data) {
                static_cast<TrayIcon*>(user_data)->refresh();
            },
            this, nullptr);
        subscriptions_.push_back(id);
    }

    refresh();
}

TrayIcon::~TrayIcon() {
    for (const guint id : subscriptions_) {
        if (bus_ != nullptr) g_dbus_connection_signal_unsubscribe(bus_, id);
    }
    // The popover is parented to this widget by hand, so it has to be unparented by
    // hand as well or GTK warns about a widget destroyed with a child still set.
    menu_.unparent();
}

std::string TrayIcon::read_string(const char* property) {
    GVariant* value = read_property(bus_, address_.service, address_.path,
                                   kItemInterface, property);
    if (value == nullptr) return {};
    const std::string text = variant_string(value);
    g_variant_unref(value);
    return text;
}

std::int32_t TrayIcon::read_int(const char* property) {
    GVariant* value = read_property(bus_, address_.service, address_.path,
                                   kItemInterface, property);
    if (value == nullptr) return 0;
    std::int32_t number = 0;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) {
        number = g_variant_get_int32(value);
    }
    g_variant_unref(value);
    return number;
}

void TrayIcon::refresh() {
    id_                  = read_string("Id");
    title_               = read_string("Title");
    icon_name_           = read_string("IconName");
    attention_icon_name_ = read_string("AttentionIconName");
    icon_theme_path_     = read_string("IconThemePath");
    menu_path_           = read_string("Menu");
    status_              = parse_tray_status(read_string("Status"));

    {
        GVariant* value = read_property(bus_, address_.service, address_.path,
                                       kItemInterface, "ItemIsMenu");
        item_is_menu_ = value != nullptr &&
                        g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) &&
                        g_variant_get_boolean(value);
        if (value != nullptr) g_variant_unref(value);
    }

    apply_icon();
    apply_tooltip();
    set_visible(tray_item_visible(status_));
}

void TrayIcon::apply_icon() {
    // An application may ship its icon outside the icon theme entirely -- Steam
    // points at its own install directory -- so the path it names is added to the
    // theme's search path before the name is looked up. Without this the icon
    // simply does not resolve and the tray shows a broken-image placeholder.
    if (!icon_theme_path_.empty()) {
        if (auto theme = Gtk::IconTheme::get_for_display(Gdk::Display::get_default())) {
            const auto paths = theme->get_search_path();
            if (std::find(paths.begin(), paths.end(), icon_theme_path_) == paths.end()) {
                theme->add_search_path(icon_theme_path_);
            }
        }
    }

    const std::string name = tray_icon_name(status_, icon_name_, attention_icon_name_);
    if (!name.empty()) {
        image_.set_from_icon_name(name);
        image_.set_pixel_size(icon_size_);
        return;
    }

    // No name: the application supplied raw pixels instead. This is the path
    // Discord, Telegram and anything drawing its own badge takes.
    const char* pixmap_property =
        status_ == TrayStatus::NeedsAttention ? "AttentionIconPixmap" : "IconPixmap";
    GVariant* pixmaps = read_property(bus_, address_.service, address_.path,
                                      kItemInterface, pixmap_property);
    if (pixmaps == nullptr || !g_variant_is_of_type(pixmaps, G_VARIANT_TYPE("a(iiay)"))) {
        if (pixmaps != nullptr) g_variant_unref(pixmaps);
        image_.set_from_icon_name("application-x-executable-symbolic");
        return;
    }

    std::vector<TrayPixmapSize> sizes;
    const gsize count = g_variant_n_children(pixmaps);
    for (gsize i = 0; i < count; ++i) {
        GVariant* entry = g_variant_get_child_value(pixmaps, i);
        gint32 width = 0, height = 0;
        GVariant* bytes = nullptr;
        g_variant_get(entry, "(ii@ay)", &width, &height, &bytes);
        sizes.push_back({.width = width, .height = height});
        if (bytes != nullptr) g_variant_unref(bytes);
        g_variant_unref(entry);
    }

    const int chosen = best_tray_pixmap(sizes);
    if (chosen < 0) {
        g_variant_unref(pixmaps);
        image_.set_from_icon_name("application-x-executable-symbolic");
        return;
    }

    GVariant* entry = g_variant_get_child_value(pixmaps, static_cast<gsize>(chosen));
    gint32 width = 0, height = 0;
    GVariant* bytes = nullptr;
    g_variant_get(entry, "(ii@ay)", &width, &height, &bytes);

    gsize length = 0;
    const auto* data = static_cast<const std::uint8_t*>(
        g_variant_get_fixed_array(bytes, &length, sizeof(guchar)));

    const auto rgba = tray_argb_to_rgba(data, length, width, height);
    if (!rgba.empty()) {
        auto pixbuf = Gdk::Pixbuf::create_from_data(
            rgba.data(), Gdk::Colorspace::RGB, /*has_alpha=*/true, 8, width, height,
            width * 4);
        // Copied, because the vector above dies at the end of this function and the
        // pixbuf would otherwise be pointing at freed pixels the next time it draws.
        image_.set(Gdk::Texture::create_for_pixbuf(pixbuf->copy()));
        image_.set_pixel_size(icon_size_);
    } else {
        image_.set_from_icon_name("application-x-executable-symbolic");
    }

    if (bytes != nullptr) g_variant_unref(bytes);
    g_variant_unref(entry);
    g_variant_unref(pixmaps);
}

void TrayIcon::apply_tooltip() {
    std::string tip_title;
    std::string tip_body;

    GVariant* tooltip = read_property(bus_, address_.service, address_.path,
                                      kItemInterface, "ToolTip");
    if (tooltip != nullptr) {
        if (g_variant_is_of_type(tooltip, G_VARIANT_TYPE("(sa(iiay)ss)"))) {
            const gchar* icon = nullptr;
            GVariant*    pixmap = nullptr;
            const gchar* head = nullptr;
            const gchar* body = nullptr;
            g_variant_get(tooltip, "(&s@a(iiay)&s&s)", &icon, &pixmap, &head, &body);
            tip_title = head != nullptr ? head : "";
            tip_body  = body != nullptr ? body : "";
            if (pixmap != nullptr) g_variant_unref(pixmap);
        }
        g_variant_unref(tooltip);
    }

    const std::string fallback = !title_.empty() ? title_ : id_;
    const std::string text = tray_tooltip(tip_title, tip_body, fallback);
    if (!text.empty()) set_tooltip_text(text);
}

void TrayIcon::activate_item(const char* method, int x, int y) {
    if (bus_ == nullptr) return;
    // Fire and forget. The reply carries nothing, and waiting for one would put the
    // application's response time in front of the panel's.
    g_dbus_connection_call(bus_, address_.service.c_str(), address_.path.c_str(),
                           kItemInterface, method, g_variant_new("(ii)", x, y),
                           nullptr, G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
                           nullptr, nullptr);
}

namespace {

// Reads one dbusmenu layout node and its children out of the (ia{sv}av) GVariant.
TrayMenuNode parse_menu_node(GVariant* node) {
    TrayMenuNode parsed;
    if (node == nullptr) return parsed;

    gint32    id = 0;
    GVariant* properties = nullptr;
    GVariant* children   = nullptr;
    g_variant_get(node, "(i@a{sv}@av)", &id, &properties, &children);
    parsed.id = id;

    if (properties != nullptr) {
        GVariantIter iter;
        const gchar* key = nullptr;
        GVariant*    value = nullptr;
        g_variant_iter_init(&iter, properties);
        while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
            const std::string name = key != nullptr ? key : "";
            if (name == "label") {
                parsed.label = strip_menu_mnemonics(variant_string(value));
            } else if (name == "enabled") {
                if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
                    parsed.enabled = g_variant_get_boolean(value);
                }
            } else if (name == "visible") {
                if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
                    parsed.visible = g_variant_get_boolean(value);
                }
            } else if (name == "type") {
                parsed.separator = variant_string(value) == "separator";
            } else if (name == "toggle-type") {
                parsed.checkable = !variant_string(value).empty();
            } else if (name == "toggle-state") {
                if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) {
                    parsed.checked = g_variant_get_int32(value) == 1;
                }
            } else if (name == "icon-name") {
                parsed.icon_name = variant_string(value);
            }
            g_variant_unref(value);
        }
        g_variant_unref(properties);
    }

    if (children != nullptr) {
        const gsize count = g_variant_n_children(children);
        for (gsize i = 0; i < count; ++i) {
            GVariant* boxed = g_variant_get_child_value(children, i);
            GVariant* child = g_variant_get_variant(boxed);
            parsed.children.push_back(parse_menu_node(child));
            g_variant_unref(child);
            g_variant_unref(boxed);
        }
        g_variant_unref(children);
    }

    return parsed;
}

}  // namespace

void TrayIcon::open_menu(double x, double y) {
    if (bus_ == nullptr || menu_path_.empty()) {
        // No menu of its own. The protocol's own fallback is to ask the application
        // to put one up itself, wherever it likes.
        activate_item("ContextMenu", 0, 0);
        return;
    }

    // AboutToShow first. dbusmenu is a lazy protocol: applications are entitled to
    // build their menu only when told it is about to be seen, and skipping this
    // gets an empty or stale layout from anything that does.
    g_dbus_connection_call_sync(
        bus_, address_.service.c_str(), menu_path_.c_str(), kMenuInterface,
        "AboutToShow", g_variant_new("(i)", 0), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr);

    GError*   error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, address_.service.c_str(), menu_path_.c_str(), kMenuInterface,
        "GetLayout",
        g_variant_new("(iias)", 0, -1, nullptr), G_VARIANT_TYPE("(u(ia{sv}av))"),
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &error);

    if (error != nullptr) {
        g_error_free(error);
        activate_item("ContextMenu", 0, 0);
        return;
    }
    if (reply == nullptr) return;

    guint32   revision = 0;
    GVariant* root = nullptr;
    g_variant_get(reply, "(u@(ia{sv}av))", &revision, &root);
    TrayMenuNode tree = parse_menu_node(root);
    if (root != nullptr) g_variant_unref(root);
    g_variant_unref(reply);

    tree.children = tidy_tray_menu(std::move(tree.children));
    if (tree.children.empty()) {
        activate_item("ContextMenu", 0, 0);
        return;
    }

    // Rebuilt from scratch each time, actions included. Menu ids are the
    // application's and can be reused between builds, so keeping old actions around
    // would mean a click landing on a previous menu's entry.
    actions_ = Gio::SimpleActionGroup::create();
    next_action_ = 0;
    model_ = Gio::Menu::create();

    const auto send_event = [this](std::int32_t id) {
        g_dbus_connection_call(
            bus_, address_.service.c_str(), menu_path_.c_str(), kMenuInterface,
            "Event",
            g_variant_new("(isvu)", id, "clicked", g_variant_new_int32(0),
                          static_cast<guint32>(0)),
            nullptr, G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr,
            nullptr);
    };

    // Recursive build. Declared as a std::function because a lambda cannot name
    // itself, and the layout is a tree of arbitrary depth.
    std::function<void(const std::vector<TrayMenuNode>&, const Glib::RefPtr<Gio::Menu>&)>
        build = [&](const std::vector<TrayMenuNode>& nodes,
                    const Glib::RefPtr<Gio::Menu>& into) {
            // A section per run of entries between separators, which is how GMenu
            // expresses separators -- there is no separator item to insert.
            auto section = Gio::Menu::create();
            const auto flush = [&] {
                if (section->get_n_items() > 0) into->append_section(section);
                section = Gio::Menu::create();
            };

            for (const auto& node : nodes) {
                if (node.separator) {
                    flush();
                    continue;
                }

                if (!node.children.empty()) {
                    auto submenu = Gio::Menu::create();
                    build(node.children, submenu);
                    section->append_submenu(node.label, submenu);
                    continue;
                }

                const std::string action = "a" + std::to_string(next_action_++);
                if (node.checkable) {
                    auto item = Gio::SimpleAction::create_bool(action, node.checked);
                    const std::int32_t id = node.id;
                    item->signal_activate().connect(
                        [send_event, id](const Glib::VariantBase&) { send_event(id); });
                    item->set_enabled(node.enabled);
                    actions_->add_action(item);
                } else {
                    auto item = Gio::SimpleAction::create(action);
                    const std::int32_t id = node.id;
                    item->signal_activate().connect(
                        [send_event, id](const Glib::VariantBase&) { send_event(id); });
                    item->set_enabled(node.enabled);
                    actions_->add_action(item);
                }
                section->append(node.label, "tray." + action);
            }
            flush();
        };

    build(tree.children, model_);

    insert_action_group("tray", actions_);
    menu_.set_menu_model(model_);
    menu_.set_pointing_to(Gdk::Rectangle(static_cast<int>(x), static_cast<int>(y), 1, 1));
    menu_.popup();
}

// ---------------------------------------------------------------------------
// XAppTrayIcon
// ---------------------------------------------------------------------------
XAppTrayIcon::XAppTrayIcon(GDBusConnection* bus, std::string service, std::string path,
                           int icon_size, bool panel_at_top)
    : bus_(bus), service_(std::move(service)), path_(std::move(path)),
      icon_size_(icon_size), panel_at_top_(panel_at_top) {
    add_css_class("tray-icon");
    add_css_class("flat");
    set_has_frame(false);
    image_.set_pixel_size(icon_size_);
    set_child(image_);

    // Left and right both go straight back to the application. XApp has no menu
    // interface at all -- the application owns its menu and only needs to be told
    // where the click was, which is the whole protocol.
    signal_clicked().connect([this] { report_click(1); });
    {
        auto secondary = Gtk::GestureClick::create();
        secondary->set_button(GDK_BUTTON_SECONDARY);
        secondary->signal_pressed().connect(
            [this](int, double, double) { report_click(3); });
        add_controller(secondary);

        auto middle = Gtk::GestureClick::create();
        middle->set_button(GDK_BUTTON_MIDDLE);
        middle->signal_pressed().connect(
            [this](int, double, double) { report_click(2); });
        add_controller(middle);
    }

    {
        auto scroll = Gtk::EventControllerScroll::create();
        scroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
        scroll->signal_scroll().connect(
            [this](double dx, double dy) {
                const int delta = static_cast<int>(dy != 0 ? dy : dx);
                // XApp's orientation is 0 horizontal, 1 vertical.
                const int orientation = dy != 0 ? 1 : 0;
                if (bus_ != nullptr && delta != 0) {
                    g_dbus_connection_call(
                        bus_, service_.c_str(), path_.c_str(), kXAppInterface, "Scroll",
                        g_variant_new("(iiu)", delta, orientation,
                                      static_cast<guint32>(0)),
                        nullptr, G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
                        nullptr, nullptr);
                }
                return true;
            },
            false);
        add_controller(scroll);
    }

    subscription_ = g_dbus_connection_signal_subscribe(
        bus_, service_.c_str(), "org.freedesktop.DBus.Properties",
        "PropertiesChanged", path_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
           GVariant*, gpointer user_data) {
            static_cast<XAppTrayIcon*>(user_data)->refresh();
        },
        this, nullptr);

    // The panel tells the application how big to draw, which is the one writable
    // property in the interface. Applications that render their own image use it.
    g_dbus_connection_call(
        bus_, service_.c_str(), path_.c_str(), "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", kXAppInterface, "IconSize",
                      g_variant_new_int32(icon_size_)),
        nullptr, G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr, nullptr);

    refresh();
}

XAppTrayIcon::~XAppTrayIcon() {
    if (bus_ != nullptr && subscription_ != 0) {
        g_dbus_connection_signal_unsubscribe(bus_, subscription_);
    }
}

void XAppTrayIcon::refresh() {
    const auto read = [this](const char* property) -> std::string {
        GVariant* value = read_property(bus_, service_, path_, kXAppInterface, property);
        if (value == nullptr) return {};
        const std::string text = variant_string(value);
        g_variant_unref(value);
        return text;
    };

    // Visible is honoured, and it matters: Mint's process-monitor icon sits on the
    // bus permanently with Visible false and an icon name of a single space, so a
    // tray that ignored it would show a blank button that does nothing.
    bool visible = true;
    if (GVariant* value = read_property(bus_, service_, path_, kXAppInterface,
                                        "Visible");
        value != nullptr) {
        if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
            visible = g_variant_get_boolean(value);
        }
        g_variant_unref(value);
    }

    const std::string icon = read("IconName");
    const std::string tip  = read("TooltipText");
    const std::string name = read("Name");

    // A name of whitespace is how an application says "nothing to show" without
    // clearing the property.
    const bool blank = icon.empty() || icon.find_first_not_of(" \t") == std::string::npos;
    set_visible(visible && !blank);

    if (!blank) {
        if (tray_icon_is_path(icon)) {
            image_.set(icon);
        } else {
            image_.set_from_icon_name(icon);
        }
        image_.set_pixel_size(icon_size_);
    }

    const std::string text = tray_tooltip("", tip, name);
    if (!text.empty()) set_tooltip_text(text);
}

void XAppTrayIcon::report_click(unsigned button) {
    if (bus_ == nullptr) return;

    // The application places its own menu from these coordinates, so they must be
    // root coordinates. GTK4 will not convert a surface position into one without
    // linking gdk-x11, so the pointer is asked for directly through the display
    // seam. If it cannot answer, zeros are sent and the application falls back to
    // positioning the menu itself -- a menu in a slightly odd place beats no menu.
    const auto pointer = auspex::pointer_position();
    const int x = pointer ? pointer->x : 0;
    const int y = pointer ? pointer->y : 0;
    const int position = xapp_panel_position(panel_at_top_);

    // Press then release, both. Applications watch for the release to decide a
    // click actually happened, and sending only the press leaves menus that open
    // and never close.
    g_dbus_connection_call(
        bus_, service_.c_str(), path_.c_str(), kXAppInterface, "ButtonPress",
        g_variant_new("(iiuui)", x, y, button, static_cast<guint32>(0), position),
        nullptr, G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr, nullptr);
    g_dbus_connection_call(
        bus_, service_.c_str(), path_.c_str(), kXAppInterface, "ButtonRelease",
        g_variant_new("(iiuui)", x, y, button, static_cast<guint32>(0), position),
        nullptr, G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// SystemTray
// ---------------------------------------------------------------------------
SystemTray::SystemTray(int icon_size, bool panel_at_top)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 2), icon_size_(icon_size),
      panel_at_top_(panel_at_top) {
    add_css_class("system-tray");

    // The toggle is always present; the icons live behind it.
    //
    // A tray is mostly applications you are not currently doing anything about, and
    // on a bar shared with a window list their permanent space is taken from the
    // thing you are actually using. Collapsed, this costs one arrow.
    toggle_.set_has_frame(false);
    toggle_.add_css_class("flat");
    toggle_.add_css_class("tray-toggle");
    toggle_icon_.set_pixel_size(icon_size_ - 6);
    toggle_.set_child(toggle_icon_);
    toggle_.signal_toggled().connect([this] {
        revealer_.set_reveal_child(toggle_.get_active());
        update_toggle();
    });
    append(toggle_);

    revealer_.set_transition_type(Gtk::RevealerTransitionType::SLIDE_LEFT);
    revealer_.set_transition_duration(150);
    revealer_.set_child(icons_);
    revealer_.set_reveal_child(false);
    append(revealer_);

    update_toggle();
    connect_bus();
}

SystemTray::~SystemTray() {
    items_.clear();
    xapp_.clear();
    if (bus_ != nullptr) {
        for (const guint id : subscriptions_) g_dbus_connection_signal_unsubscribe(bus_, id);
        if (watcher_object_id_ != 0) {
            g_dbus_connection_unregister_object(bus_, watcher_object_id_);
        }
    }
    if (host_name_id_ != 0)    g_bus_unown_name(host_name_id_);
    if (watcher_name_id_ != 0) g_bus_unown_name(watcher_name_id_);
    if (bus_ != nullptr)       g_object_unref(bus_);
}

void SystemTray::update_toggle() {
    const bool open = toggle_.get_active();
    // The arrow points the way the tray will go, which is the convention every
    // panel with a hideable tray uses.
    toggle_icon_.set_from_icon_name(open ? "pan-end-symbolic" : "pan-start-symbolic");
    toggle_.set_tooltip_text(open ? "Hide the tray" : "Show the tray");

    // Nothing registered: no point offering to open an empty drawer.
    set_visible(!(items_.empty() && xapp_.empty()));
}

void SystemTray::connect_bus() {
    GError* error = nullptr;
    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (bus_ == nullptr) {
        std::fprintf(stderr, "auspex: no session bus, tray disabled: %s\n",
                     error != nullptr ? error->message : "unknown");
        if (error != nullptr) g_error_free(error);
        set_visible(false);
        return;
    }

    become_host();
    try_become_watcher();

    // The watcher's own signals, so items appearing and disappearing are noticed
    // without polling. Subscribed by interface rather than by sender, because the
    // watcher may be replaced while we are running.
    for (const char* signal : {"StatusNotifierItemRegistered",
                               "StatusNotifierItemUnregistered"}) {
        subscriptions_.push_back(g_dbus_connection_signal_subscribe(
            bus_, nullptr, kWatcherInterface, signal, kWatcherPath, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, &SystemTray::on_name_signal, this, nullptr));
    }

    // Mint's protocol has no watcher to ask, so its services are found by name and
    // followed through NameOwnerChanged: a service appearing is an icon appearing.
    subscriptions_.push_back(g_dbus_connection_signal_subscribe(
        bus_, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
           GVariant* parameters, gpointer user_data) {
            auto* self = static_cast<SystemTray*>(user_data);
            const gchar* name = nullptr;
            const gchar* old_owner = nullptr;
            const gchar* new_owner = nullptr;
            g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
            if (name == nullptr || !is_xapp_status_service(name)) return;

            if (new_owner != nullptr && *new_owner != '\0') {
                self->add_xapp_service(name);
            } else {
                self->remove_xapp_service(name);
            }
        },
        this, nullptr));

    sync_items();
    sync_xapp_services();
}

void SystemTray::sync_xapp_services() {
    GError*   error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "ListNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE,
        kCallTimeoutMs, nullptr, &error);
    if (reply == nullptr) {
        if (error != nullptr) g_error_free(error);
        return;
    }

    GVariant* names = g_variant_get_child_value(reply, 0);
    gsize count = 0;
    const gchar** entries = g_variant_get_strv(names, &count);
    for (gsize i = 0; i < count; ++i) {
        if (is_xapp_status_service(entries[i])) add_xapp_service(entries[i]);
    }
    g_free(entries);
    g_variant_unref(names);
    g_variant_unref(reply);
}

void SystemTray::add_xapp_service(const std::string& service) {
    if (xapp_.count(service) != 0) return;

    // One service, several icons. The object manager is how XApp publishes them and
    // the only way to learn their paths -- they are numbered, not named.
    GError*   error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, service.c_str(), kXAppRoot, "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &error);
    if (reply == nullptr) {
        if (error != nullptr) g_error_free(error);
        return;
    }

    auto& icons = xapp_[service];

    GVariant* objects = g_variant_get_child_value(reply, 0);
    GVariantIter iter;
    const gchar* path = nullptr;
    GVariant*    interfaces = nullptr;
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        // Only objects that actually implement the icon interface; the same tree
        // carries the application's own GTK objects.
        if (g_variant_lookup_value(interfaces, kXAppInterface,
                                   G_VARIANT_TYPE("a{sv}")) != nullptr) {
            auto icon = std::make_unique<XAppTrayIcon>(bus_, service, path, icon_size_,
                                                       panel_at_top_);
            icons_.append(*icon);
            icons.emplace(path, std::move(icon));
        }
        g_variant_unref(interfaces);
    }
    g_variant_unref(objects);
    g_variant_unref(reply);

    if (icons.empty()) xapp_.erase(service);
    update_toggle();
}

void SystemTray::remove_xapp_service(const std::string& service) {
    const auto found = xapp_.find(service);
    if (found == xapp_.end()) return;
    for (auto& [path, icon] : found->second) icons_.remove(*icon);
    xapp_.erase(found);
    update_toggle();
}

void SystemTray::on_name_signal(GDBusConnection*, const gchar*, const gchar*,
                                const gchar*, const gchar* signal, GVariant* parameters,
                                gpointer user_data) {
    auto* self = static_cast<SystemTray*>(user_data);
    const gchar* service = nullptr;
    if (parameters != nullptr &&
        g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)"))) {
        g_variant_get(parameters, "(&s)", &service);
    }
    const std::string name = service != nullptr ? service : "";

    if (std::string(signal) == "StatusNotifierItemRegistered") {
        self->add_item(name);
    } else {
        self->remove_item(name);
    }
}

void SystemTray::become_host() {
    // A host is required, not optional. Several watchers -- including the one
    // xfce4-panel provides -- only tell items to show themselves once at least one
    // host has registered, so skipping this yields a tray that is permanently empty
    // for no visible reason.
    const std::string name = "org.kde.StatusNotifierHost-" + std::to_string(::getpid());

    host_name_id_ = g_bus_own_name_on_connection(
        bus_, name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
        [](GDBusConnection* bus, const gchar* acquired, gpointer) {
            // Registered with whichever watcher is present. Failure is fine and
            // silent: it means no watcher yet, and the one we may install below
            // will see us when it starts.
            g_dbus_connection_call(bus, kWatcherName, kWatcherPath, kWatcherInterface,
                                   "RegisterStatusNotifierHost",
                                   g_variant_new("(s)", acquired), nullptr,
                                   G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
                                   nullptr, nullptr);
        },
        nullptr, nullptr, nullptr);
}

void SystemTray::try_become_watcher() {
    static const GDBusInterfaceVTable vtable = {
        // Method call.
        [](GDBusConnection* bus, const gchar* sender, const gchar*, const gchar*,
           const gchar* method, GVariant* parameters,
           GDBusMethodInvocation* invocation, gpointer user_data) {
            auto* self = static_cast<SystemTray*>(user_data);
            const gchar* service = nullptr;
            g_variant_get(parameters, "(&s)", &service);

            // The argument may be a bus name OR an object path, and when it is a
            // path the bus name is whoever SENT the call. blueman registers as
            // "/org/blueman/sni" and says nothing about itself; ignoring the sender
            // made that a malformed name, so it was dropped and the tray stayed
            // empty with nothing to say why.
            //
            // Found the moment Auspex became the only watcher on the desktop: every
            // application using the path form -- which is the common one --
            // registered into nothing.
            std::string name = service != nullptr ? service : "";
            if (!name.empty() && name.front() == '/' && sender != nullptr) {
                name = std::string(sender) + name;
            }

            if (std::string(method) == "RegisterStatusNotifierItem") {
                self->add_item(name);
                // Announced to every other host on the bus, not just to ourselves.
                // Auspex being the watcher must not stop another panel's tray from
                // working -- that is precisely the failure we refuse to inflict on
                // xfce4-panel when it is the one holding the name.
                g_dbus_connection_emit_signal(
                    bus, nullptr, kWatcherPath, kWatcherInterface,
                    "StatusNotifierItemRegistered", g_variant_new("(s)", name.c_str()),
                    nullptr);
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
        },
        // Get property.
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
           const gchar* property, GError**, gpointer user_data) -> GVariant* {
            auto* self = static_cast<SystemTray*>(user_data);
            const std::string name = property;
            if (name == "RegisteredStatusNotifierItems") {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
                for (const auto& [service, icon] : self->items_) {
                    const std::string registered = service + icon->address().path;
                    g_variant_builder_add(&builder, "s", registered.c_str());
                }
                return g_variant_builder_end(&builder);
            }
            if (name == "IsStatusNotifierHostRegistered") {
                return g_variant_new_boolean(TRUE);
            }
            if (name == "ProtocolVersion") return g_variant_new_int32(0);
            return nullptr;
        },
        nullptr,  // Set property: everything here is read-only.
        {nullptr},
    };

    GError* error = nullptr;
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kWatcherIntrospection, &error);
    if (info == nullptr) {
        if (error != nullptr) g_error_free(error);
        return;
    }

    watcher_object_id_ = g_dbus_connection_register_object(
        bus_, kWatcherPath, info->interfaces[0], &vtable, this, nullptr, nullptr);

    // DO_NOT_QUEUE, deliberately. Queuing would mean silently taking the tray away
    // from xfce4-panel the moment it quit, halfway through a session, with items
    // registered against it -- the icons would vanish rather than move. If Auspex
    // is to be the watcher it should be so from the start.
    watcher_name_id_ = g_bus_own_name_on_connection(
        bus_, kWatcherName, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
        [](GDBusConnection* bus, const gchar*, gpointer) {
            // Tell any host already waiting that a watcher exists now.
            g_dbus_connection_emit_signal(bus, nullptr, kWatcherPath, kWatcherInterface,
                                          "StatusNotifierHostRegistered", nullptr,
                                          nullptr);
        },
        nullptr, nullptr, nullptr);

    g_dbus_node_info_unref(info);
}

void SystemTray::sync_items() {
    GVariant* value = read_property(bus_, kWatcherName, kWatcherPath, kWatcherInterface,
                                   "RegisteredStatusNotifierItems");
    if (value == nullptr) return;

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING_ARRAY)) {
        gsize count = 0;
        const gchar** entries = g_variant_get_strv(value, &count);
        for (gsize i = 0; i < count; ++i) add_item(entries[i]);
        g_free(entries);
    }
    g_variant_unref(value);
}

void SystemTray::add_item(const std::string& registered) {
    const TrayItemAddress address = parse_tray_item_address(registered);
    if (address.service.empty()) return;
    if (items_.count(address.service) != 0) return;

    auto icon = std::make_unique<TrayIcon>(bus_, address, icon_size_, panel_at_top_);
    icons_.append(*icon);
    items_.emplace(address.service, std::move(icon));
    update_toggle();
}

void SystemTray::remove_item(const std::string& service) {
    // The watcher reports the same string it was registered with, which may carry
    // the object path; the map is keyed on the service alone.
    const TrayItemAddress address = parse_tray_item_address(service);
    const std::string key = address.service.empty() ? service : address.service;

    const auto found = items_.find(key);
    if (found == items_.end()) return;

    icons_.remove(*found->second);
    items_.erase(found);
    update_toggle();
}

}  // namespace auspex::gtk
