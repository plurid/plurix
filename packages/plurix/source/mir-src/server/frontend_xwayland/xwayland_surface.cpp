/*
 * Copyright (C) 2018 Marius Gripsgard <marius@ubports.com>
 * Copyright (C) 2020 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "xwayland_surface.h"
#include "xwayland_log.h"
#include "xwayland_surface_observer.h"

#include "mir/frontend/wayland.h"
#include "mir/scene/surface_creation_parameters.h"
#include "mir/scene/surface.h"
#include "mir/shell/shell.h"

#include "boost/throw_exception.hpp"

#include <string.h>
#include <algorithm>
#include <experimental/optional>

namespace mf = mir::frontend;
namespace msh = mir::shell;
namespace ms = mir::scene;
namespace geom = mir::geometry;

namespace
{
/// See ICCCM 4.1.3.1 (https://tronche.com/gui/x/icccm/sec-4.html)
enum class WmState: uint32_t
{
    WITHDRAWN = 0,
    NORMAL = 1,
    ICONIC = 3,
};

/// See https://specifications.freedesktop.org/wm-spec/wm-spec-1.3.html#sourceindication
enum class SourceIndication: uint32_t
{
    UNKNOWN = 0,
    APPLICATION = 1,
    PAGER = 2,
};

///See https://specifications.freedesktop.org/wm-spec/latest/ar01s04.html
enum class NetWmMoveresize: uint32_t
{
    SIZE_TOPLEFT = 0,
    SIZE_TOP = 1,
    SIZE_TOPRIGHT = 2,
    SIZE_RIGHT = 3,
    SIZE_BOTTOMRIGHT = 4,
    SIZE_BOTTOM = 5,
    SIZE_BOTTOMLEFT = 6,
    SIZE_LEFT = 7,
    MOVE = 8,           /* movement only */
    SIZE_KEYBOARD = 9,  /* size via keyboard */
    MOVE_KEYBOARD = 10, /* move via keyboard */
    CANCEL = 11,        /* cancel operation */
};

auto wm_resize_edge_to_mir_resize_edge(NetWmMoveresize wm_resize_edge) -> std::experimental::optional<MirResizeEdge>
{
    switch (wm_resize_edge)
    {
    case NetWmMoveresize::SIZE_TOPLEFT:         return mir_resize_edge_northwest;
    case NetWmMoveresize::SIZE_TOP:             return mir_resize_edge_north;
    case NetWmMoveresize::SIZE_TOPRIGHT:        return mir_resize_edge_northeast;
    case NetWmMoveresize::SIZE_RIGHT:           return mir_resize_edge_east;
    case NetWmMoveresize::SIZE_BOTTOMRIGHT:     return mir_resize_edge_southeast;
    case NetWmMoveresize::SIZE_BOTTOM:          return mir_resize_edge_south;
    case NetWmMoveresize::SIZE_BOTTOMLEFT:      return mir_resize_edge_southwest;
    case NetWmMoveresize::SIZE_LEFT:            return mir_resize_edge_west;
    case NetWmMoveresize::MOVE:                 break;
    case NetWmMoveresize::SIZE_KEYBOARD:        break;
    case NetWmMoveresize::MOVE_KEYBOARD:        break;
    case NetWmMoveresize::CANCEL:               break;
    }

    return std::experimental::nullopt;
}

/// Sets up the position, either as a child window with and aux rect or a toplevel
/// Parent can be nullptr
/// top_left should be desired global top_left of the decorations of this window
void set_position(std::shared_ptr<ms::Surface> parent, geom::Point top_left, msh::SurfaceSpecification& spec)
{
    if (parent)
    {
        auto const local_top_left =
            top_left -
            as_displacement(parent->top_left()) -
            parent->content_offset();
        spec.aux_rect = {local_top_left, {1, 1}};
        spec.placement_hints = MirPlacementHints{};
        spec.surface_placement_gravity = mir_placement_gravity_northwest;
        spec.aux_rect_placement_gravity = mir_placement_gravity_northwest;
    }
    else
    {
        spec.top_left = top_left;
    }
}

template<typename T>
auto property_handler(
    std::shared_ptr<mf::XCBConnection> const& connection,
    xcb_window_t window,
    xcb_atom_t property,
    std::function<void(T)>&& handler,
    std::function<void()>&& on_error = [](){}) -> std::pair<xcb_atom_t, std::function<std::function<void()>()>>
{
    return std::make_pair(
        property,
        [connection, window, property, handler = move(handler), on_error = move(on_error)]()
        {
            return connection->read_property(window, property, handler, on_error);
        });
}
}

mf::XWaylandSurface::XWaylandSurface(
    XWaylandWM *wm,
    std::shared_ptr<XCBConnection> const& connection,
    WlSeat& seat,
    std::shared_ptr<shell::Shell> const& shell,
    xcb_create_notify_event_t *event)
    : xwm(wm),
      connection{connection},
      seat(seat),
      shell{shell},
      window(event->window),
      property_handlers{
          property_handler<std::string const&>(
              connection,
              window,
              XCB_ATOM_WM_CLASS,
              [this](auto value)
              {
                  std::lock_guard<std::mutex> lock{mutex};
                  this->pending_spec(lock).application_id = value;
              }),
          property_handler<std::string const&>(
              connection,
              window,
              XCB_ATOM_WM_NAME,
              [this](auto value)
              {
                  std::lock_guard<std::mutex> lock{mutex};
                  this->pending_spec(lock).name = value;
              }),
          property_handler<std::string const&>(
              connection,
              window,
              connection->net_wm_name,
              [this](auto value)
              {
                  std::lock_guard<std::mutex> lock{mutex};
                  this->pending_spec(lock).name = value;
              }),
          property_handler<xcb_window_t>(
              connection,
              window,
              XCB_ATOM_WM_TRANSIENT_FOR,
              [this](xcb_window_t value)
              {
                  is_transient_for(value);
              },
              [this]()
              {
                  is_transient_for(XCB_WINDOW_NONE);
              }),
          property_handler<std::vector<xcb_atom_t> const&>(
              connection,
              window,
              connection->wm_protocols,
              [this](std::vector<xcb_atom_t> const& value)
              {
                  std::lock_guard<std::mutex> lock{mutex};
                  this->cached.supported_wm_protocols = std::set<xcb_atom_t>{value.begin(), value.end()};
              },
              [this]()
              {
                  std::lock_guard<std::mutex> lock{mutex};
                  this->cached.supported_wm_protocols.clear();
              })}
{
    cached.override_redirect = event->override_redirect;
    cached.size = {event->width, event->height};
    cached.top_left = {event->x, event->y};

    uint32_t const value = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE;
    xcb_change_window_attributes(*connection, window, XCB_CW_EVENT_MASK, &value);
}

mf::XWaylandSurface::~XWaylandSurface()
{
    close();
}

void mf::XWaylandSurface::map()
{
    WindowState state;
    {
        std::lock_guard<std::mutex> lock{mutex};
        state = cached.state;
    }

    uint32_t const workspace = 1;
    connection->set_property<XCBType::CARDINAL32>(
        window,
        connection->net_wm_desktop,
        workspace);

    state.withdrawn = false;
    inform_client_of_window_state(state);
    request_scene_surface_state(state.mir_window_state());
    xcb_map_window(*connection, window);
    connection->flush();
}

void mf::XWaylandSurface::close()
{
    WindowState state;
    std::shared_ptr<scene::Surface> scene_surface;
    std::shared_ptr<XWaylandSurfaceObserver> observer;

    {
        std::lock_guard<std::mutex> lock{mutex};

        state = cached.state;

        scene_surface = weak_scene_surface.lock();
        weak_scene_surface.reset();

        weak_session.reset();

        if (surface_observer)
        {
            observer = surface_observer.value();
        }
        surface_observer = std::experimental::nullopt;
    }

    connection->delete_property(window, connection->net_wm_desktop);

    state.withdrawn = true;
    inform_client_of_window_state(state);

    xcb_unmap_window(*connection, window);
    connection->flush();

    if (scene_surface && observer)
    {
        scene_surface->remove_observer(observer);
    }

    if (scene_surface)
    {
        shell->destroy_surface(scene_surface->session().lock(), scene_surface);
        scene_surface.reset();
        // Someone may still be holding on to the surface somewhere, and that's fine
    }

    if (observer)
    {
        // make sure surface observer is deleted and will not spew any more events
        std::weak_ptr<XWaylandSurfaceObserver> const weak_observer{observer};
        observer.reset();
        if (auto const should_be_dead_observer = weak_observer.lock())
        {
            fatal_error(
                "surface observer should have been deleted, but was not (use count %d)",
                should_be_dead_observer.use_count());
        }
    }
}

void mf::XWaylandSurface::take_focus()
{
    bool supports_take_focus;
    {
        std::lock_guard<std::mutex> lock{mutex};

        if (cached.override_redirect)
            return;

        supports_take_focus = (
            cached.supported_wm_protocols.find(connection->wm_take_focus) !=
            cached.supported_wm_protocols.end());
    }

    if (supports_take_focus)
    {
        uint32_t const client_message_data[]{
            connection->wm_take_focus,
            XCB_TIME_CURRENT_TIME};

        connection->send_client_message<XCBType::WM_PROTOCOLS>(
            window,
            XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
            client_message_data);
    }

    // TODO: only send if allowed based on wm hints input mode
    // see https://tronche.com/gui/x/icccm/sec-4.html#s-4.1.7
    xcb_set_input_focus(
        *connection,
        XCB_INPUT_FOCUS_POINTER_ROOT,
        window,
        XCB_CURRENT_TIME);

    connection->flush();
}

void mf::XWaylandSurface::configure_request(xcb_configure_request_event_t* event)
{
    std::shared_ptr<scene::Surface> scene_surface;

    {
        std::lock_guard<std::mutex> lock{mutex};
        scene_surface = weak_scene_surface.lock();
    }

    if (scene_surface)
    {
        auto const content_offset = scene_surface->content_offset();

        geom::Point const old_position{scene_surface->top_left() + content_offset};
        geom::Point const new_position{
            event->value_mask & XCB_CONFIG_WINDOW_X ? geom::X{event->x} : old_position.x,
            event->value_mask & XCB_CONFIG_WINDOW_Y ? geom::Y{event->y} : old_position.y,
        };

        geom::Size const old_size{scene_surface->content_size()};
        geom::Size const new_size{
            event->value_mask & XCB_CONFIG_WINDOW_WIDTH ? geom::Width{event->width} : old_size.width,
            event->value_mask & XCB_CONFIG_WINDOW_HEIGHT ? geom::Height{event->height} : old_size.height,
        };

        shell::SurfaceSpecification mods;

        if (old_position != new_position)
        {
            set_position(scene_surface->parent(), new_position - content_offset, mods);
        }

        if (old_size != new_size)
        {
            // Mir appears to not respect size request unless both width and height are set
            mods.width = new_size.width;
            mods.height = new_size.height;
        }

        if (!mods.is_empty())
        {
            shell->modify_surface(scene_surface->session().lock(), scene_surface, mods);
        }
    }
    else
    {
        geom::Point const top_left{
            event->value_mask & XCB_CONFIG_WINDOW_X ? geom::X{event->x} : cached.top_left.x,
            event->value_mask & XCB_CONFIG_WINDOW_Y ? geom::Y{event->y} : cached.top_left.y};

        geom::Size const size{
            event->value_mask & XCB_CONFIG_WINDOW_WIDTH ? geom::Width{event->width} : cached.size.width,
            event->value_mask & XCB_CONFIG_WINDOW_HEIGHT ? geom::Height{event->height} : cached.size.height};

        connection->configure_window(
            window,
            top_left,
            size,
            std::experimental::nullopt,
            std::experimental::nullopt);

        connection->flush();
    }
}

void mf::XWaylandSurface::configure_notify(xcb_configure_notify_event_t* event)
{
    std::lock_guard<std::mutex> lock{mutex};
    cached.override_redirect = event->override_redirect;
    cached.top_left = geom::Point{event->x, event->y},
    cached.size = geom::Size{event->width, event->height};
}

void mf::XWaylandSurface::net_wm_state_client_message(uint32_t const (&data)[5])
{
    // The client is requesting a change in state
    // see https://specifications.freedesktop.org/wm-spec/wm-spec-1.3.html#idm45390969565536

    enum class Action: uint32_t
    {
        REMOVE = 0,
        ADD = 1,
        TOGGLE = 2,
    };

    auto const* pdata = data;
    auto const action = static_cast<Action>(*pdata++);
    xcb_atom_t const properties[2] = { static_cast<xcb_atom_t>(*pdata++),  static_cast<xcb_atom_t>(*pdata++) };
    auto const source_indication = static_cast<SourceIndication>(*pdata++);

    (void)source_indication;

    WindowState new_window_state;

    {
        std::lock_guard<std::mutex> lock{mutex};

        new_window_state = cached.state;

        for (xcb_atom_t const property : properties)
        {
            if (property) // if there is only one property, the 2nd is 0
            {
                bool nil{false}, *prop_ptr = &nil;

                if (property == connection->net_wm_state_hidden)
                    prop_ptr = &new_window_state.minimized;
                else if (property == connection->net_wm_state_maximized_horz) // assume vert is also set
                    prop_ptr = &new_window_state.maximized;
                else if (property == connection->net_wm_state_fullscreen)
                    prop_ptr = &new_window_state.fullscreen;

                switch (action)
                {
                case Action::REMOVE: *prop_ptr = false; break;
                case Action::ADD: *prop_ptr = true; break;
                case Action::TOGGLE: *prop_ptr = !*prop_ptr; break;
                }
            }
        }
    }

    inform_client_of_window_state(new_window_state);
    request_scene_surface_state(new_window_state.mir_window_state());
}

void mf::XWaylandSurface::wm_change_state_client_message(uint32_t const (&data)[5])
{
    // See ICCCM 4.1.4 (https://tronche.com/gui/x/icccm/sec-4.html)

    WmState const requested_state = static_cast<WmState>(data[0]);

    WindowState new_window_state;

    {
        std::lock_guard<std::mutex> lock{mutex};

        new_window_state = cached.state;

        switch (requested_state)
        {
        case WmState::NORMAL:
            new_window_state.minimized = false;
            break;

        case WmState::ICONIC:
            new_window_state.minimized = true;
            break;

        default:
            BOOST_THROW_EXCEPTION(std::runtime_error(
                "WM_CHANGE_STATE client message sent invalid state " +
                std::to_string(static_cast<std::underlying_type<WmState>::type>(requested_state))));
        }
    }

    inform_client_of_window_state(new_window_state);
    request_scene_surface_state(new_window_state.mir_window_state());
}

void mf::XWaylandSurface::property_notify(xcb_atom_t property)
{
    auto const handler = property_handlers.find(property);
    if (handler != property_handlers.end())
    {
        auto completion = handler->second();
        completion();

        std::shared_ptr<scene::Surface> scene_surface;
        std::experimental::optional<std::unique_ptr<shell::SurfaceSpecification>> spec;

        {
            std::lock_guard<std::mutex> lock{mutex};
            scene_surface = weak_scene_surface.lock();
            spec = consume_pending_spec(lock);
        }

        if (spec && scene_surface)
        {
            if (spec.value()->application_id.is_set() &&
                spec.value()->application_id.value() == scene_surface->application_id())
                spec.value()->application_id.consume();

            if (spec.value()->name.is_set() &&
                spec.value()->name.value() == scene_surface->name())
                spec.value()->name.consume();

            if (spec.value()->parent.is_set() &&
                spec.value()->parent.value().lock() == scene_surface->parent())
                spec.value()->parent.consume();

            if (!spec.value()->is_empty())
                shell->modify_surface(scene_surface->session().lock(), scene_surface, *spec.value());
        }
    }
}

void mf::XWaylandSurface::attach_wl_surface(WlSurface* wl_surface)
{
    // We assume we are on the Wayland thread

    if (verbose_xwayland_logging_enabled())
    {
        log_debug(
            "Attaching wl_surface@%d to %s...",
            wl_resource_get_id(wl_surface->resource),
            connection->window_debug_string(window).c_str());
    }

    WindowState state;
    std::shared_ptr<scene::Session> session;
    scene::SurfaceCreationParameters params;

    auto const observer = std::make_shared<XWaylandSurfaceObserver>(seat, wl_surface, this);

    {
        std::lock_guard<std::mutex> lock{mutex};

        if (surface_observer || weak_session.lock() || weak_scene_surface.lock())
            BOOST_THROW_EXCEPTION(std::runtime_error("XWaylandSurface::set_wl_surface() called multiple times"));

        session = get_session(wl_surface->resource);

        surface_observer = observer;
        weak_session = session;

        state = cached.state;
        state.withdrawn = false;

        params.streams = std::vector<shell::StreamSpecification>{};
        params.input_shape = std::vector<geom::Rectangle>{};
        wl_surface->populate_surface_data(
            params.streams.value(),
            params.input_shape.value(),
            {});
        params.size = cached.size;
        params.top_left = cached.top_left;
        params.type = mir_window_type_freestyle;
        params.state = state.mir_window_state();
        params.server_side_decorated = !cached.override_redirect;
    }

    std::vector<std::function<void()>> reply_functions;

    // Read all properties
    for (auto const& handler : property_handlers)
    {
        reply_functions.push_back(handler.second());
    }

    // Wait for and process all the XCB replies
    for (auto const& reply_function : reply_functions)
    {
        reply_function();
    }

    // property_handlers will have updated the pending spec. Use it.
    {
        std::lock_guard<std::mutex> lock{mutex};
        if (auto const spec = consume_pending_spec(lock))
        {
            params.update_from(*spec.value());
        }
    }

    auto const surface = shell->create_surface(session, params, observer);
    inform_client_of_window_state(state);
    connection->configure_window(
        window,
        surface->top_left() + surface->content_offset(),
        surface->content_size(),
        std::experimental::nullopt,
        XCB_STACK_MODE_ABOVE);

    {
        std::lock_guard<std::mutex> lock{mutex};
        weak_scene_surface = surface;
    }
}

void mf::XWaylandSurface::move_resize(uint32_t detail)
{
    std::shared_ptr<scene::Surface> scene_surface;
    std::chrono::nanoseconds timestamp;
    {
        std::lock_guard<std::mutex> lock{mutex};
        scene_surface = weak_scene_surface.lock();
        timestamp = latest_input_timestamp(lock);
    }

    auto const action = static_cast<NetWmMoveresize>(detail);
    if (action == NetWmMoveresize::MOVE)
    {
        if (scene_surface)
        {
            shell->request_move(scene_surface->session().lock(), scene_surface, timestamp.count());
        }
    }
    else if (auto const edge = wm_resize_edge_to_mir_resize_edge(action))
    {
        if (scene_surface)
        {
            shell->request_resize(scene_surface->session().lock(), scene_surface, timestamp.count(), edge.value());
        }
    }
    else
    {
        mir::log_warning("XWaylandSurface::move_resize() called with unknown detail %d", detail);
    }
}

auto mf::XWaylandSurface::WindowState::operator==(WindowState const& that) const -> bool
{
    return
        withdrawn == that.withdrawn &&
        minimized == that.minimized &&
        maximized == that.maximized &&
        fullscreen == that.fullscreen;
}

auto mf::XWaylandSurface::WindowState::mir_window_state() const -> MirWindowState
{
    // withdrawn is ignored
    if (minimized)
        return mir_window_state_minimized;
    else if (fullscreen)
        return mir_window_state_fullscreen;
    else if (maximized)
        return mir_window_state_maximized;
    else
        return mir_window_state_restored;
}

auto mf::XWaylandSurface::WindowState::updated_from(MirWindowState state) const -> WindowState
{
    auto updated = *this;

    // If there is a MirWindowState to update from, the surface should not be withdrawn
    updated.withdrawn = false;

    switch (state)
    {
    case mir_window_state_hidden:
    case mir_window_state_minimized:
        updated.minimized = true;
        // don't change maximized or fullscreen
        break;

    case mir_window_state_fullscreen:
        updated.minimized = false;
        updated.fullscreen = true;
        // don't change maximizeds
        break;

    case mir_window_state_maximized:
    case mir_window_state_vertmaximized:
    case mir_window_state_horizmaximized:
        updated.minimized = false;
        updated.maximized = true;
        updated.fullscreen = false;
        break;

    case mir_window_state_restored:
    case mir_window_state_unknown:
    case mir_window_state_attached:
        updated.minimized = false;
        updated.maximized = false;
        updated.fullscreen = false;
        break;

    case mir_window_states:
        break;
    }

    return updated;
}

void mf::XWaylandSurface::scene_surface_focus_set(bool has_focus)
{
    xwm->set_focus(window, has_focus);
    // HACK: A window being focused does not necessarily mean it's on top
    // TODO: plumb through access to the real stacking order
    connection->configure_window(
        window,
        std::experimental::nullopt,
        std::experimental::nullopt,
        std::experimental::nullopt,
        XCB_STACK_MODE_ABOVE);
}

void mf::XWaylandSurface::scene_surface_state_set(MirWindowState new_state)
{
    WindowState state;
    {
        std::lock_guard<std::mutex> lock{mutex};
        state = cached.state.updated_from(new_state);
    }
    inform_client_of_window_state(state);
    if (new_state == mir_window_state_minimized || new_state == mir_window_state_minimized)
    {
        connection->configure_window(
            window,
            std::experimental::nullopt,
            std::experimental::nullopt,
            std::experimental::nullopt,
            XCB_STACK_MODE_BELOW);
    }
}

void mf::XWaylandSurface::scene_surface_resized(geometry::Size const& new_size)
{
    connection->configure_window(
        window,
        std::experimental::nullopt,
        new_size,
        std::experimental::nullopt,
        std::experimental::nullopt);
    connection->flush();
}

void mf::XWaylandSurface::scene_surface_moved_to(geometry::Point const& new_top_left)
{
    std::shared_ptr<scene::Surface> scene_surface;
    {
        std::lock_guard<std::mutex> lock{mutex};
        scene_surface = weak_scene_surface.lock();
    }
    auto const content_offset = scene_surface ? scene_surface->content_offset() : geom::Displacement{};
    connection->configure_window(
        window,
        new_top_left + content_offset,
        std::experimental::nullopt,
        std::experimental::nullopt,
        std::experimental::nullopt);
    connection->flush();
}

void mf::XWaylandSurface::scene_surface_close_requested()
{
    bool delete_window;
    {
        std::lock_guard<std::mutex> lock{mutex};
        delete_window = (
            cached.supported_wm_protocols.find(connection->wm_delete_window) !=
            cached.supported_wm_protocols.end());
    }

    if (delete_window)
    {
        if (verbose_xwayland_logging_enabled())
        {
            log_debug(
                "Sending WM_DELETE_WINDOW request to %s",
                connection->window_debug_string(window).c_str());
        }
        uint32_t const client_message_data[]{
            connection->wm_delete_window,
            XCB_TIME_CURRENT_TIME,
        };
        connection->send_client_message<XCBType::WM_PROTOCOLS>(window, XCB_EVENT_MASK_NO_EVENT, client_message_data);
    }
    else
    {
        if (verbose_xwayland_logging_enabled())
        {
            log_debug(
                "Killing %s because it does not support WM_DELETE_WINDOW",
                connection->window_debug_string(window).c_str());
        }
        xcb_kill_client(*connection, window);
    }
    connection->flush();
}

void mf::XWaylandSurface::run_on_wayland_thread(std::function<void()>&& work)
{
    xwm->run_on_wayland_thread(std::move(work));
}

void mf::XWaylandSurface::wl_surface_destroyed()
{
    if (verbose_xwayland_logging_enabled())
    {
        log_debug("%s's wl_surface destoyed", connection->window_debug_string(window).c_str());
    }
    close();
}

auto mf::XWaylandSurface::scene_surface() const -> std::experimental::optional<std::shared_ptr<scene::Surface>>
{
    std::lock_guard<std::mutex> lock{mutex};
    if (auto const scene_surface = weak_scene_surface.lock())
        return scene_surface;
    else
        return std::experimental::nullopt;
}

auto mf::XWaylandSurface::pending_spec(std::lock_guard<std::mutex> const&) -> msh::SurfaceSpecification&
{
    if (!nullable_pending_spec)
        nullable_pending_spec = std::make_unique<msh::SurfaceSpecification>();
    return *nullable_pending_spec;
}

auto mf::XWaylandSurface::consume_pending_spec(
    std::lock_guard<std::mutex> const&) -> std::experimental::optional<std::unique_ptr<msh::SurfaceSpecification>>
{
    if (nullable_pending_spec)
        return move(nullable_pending_spec);
    else
        return std::experimental::nullopt;
}

void mf::XWaylandSurface::is_transient_for(xcb_window_t transient_for)
{
    std::shared_ptr<scene::Surface> parent_scene_surface; // May remain nullptr

    // returns nullptr on error
    auto const get_scene_surface_from = [this](xcb_window_t xcb_window) -> std::shared_ptr<scene::Surface>
        {
            if (auto const xwayland_surface = this->xwm->get_wm_surface(xcb_window))
            {
                std::lock_guard<std::mutex> lock{xwayland_surface.value()->mutex};
                auto const scene_surface = xwayland_surface.value()->weak_scene_surface.lock();
                if (verbose_xwayland_logging_enabled())
                {
                    if (scene_surface)
                    {
                        log_debug(
                            "%s set as transient for %s",
                            connection->window_debug_string(window).c_str(),
                            connection->window_debug_string(xcb_window).c_str());
                    }
                    else
                    {
                        log_debug(
                            "%s can not be transient for %s as the latter does not have a scene surface",
                            connection->window_debug_string(window).c_str(),
                            connection->window_debug_string(xcb_window).c_str());
                    }
                }
                return scene_surface;
            }
            else
            {
                if (verbose_xwayland_logging_enabled())
                {
                    log_debug(
                        "%s can not be transient for %s as the latter does not have an XWayland surface",
                        connection->window_debug_string(window).c_str(),
                        connection->window_debug_string(xcb_window).c_str());
                }
                return nullptr;
            }
        };

    if (transient_for != XCB_WINDOW_NONE)
    {
        parent_scene_surface = get_scene_surface_from(transient_for);

        if (!parent_scene_surface)
        {
            auto const focused_window = xwm->get_focused_window();
            if (focused_window)
            {
                if (verbose_xwayland_logging_enabled())
                {
                    log_debug(
                        "Falling back to the currently focused window (%s)",
                        connection->window_debug_string(focused_window.value()).c_str());
                }
                parent_scene_surface = get_scene_surface_from(focused_window.value());
            }
            else
            {
                if (verbose_xwayland_logging_enabled())
                {
                    log_debug(
                        "There is no focused window",
                        connection->window_debug_string(window).c_str(),
                        connection->window_debug_string(transient_for).c_str());
                }
            }
        }

        if (!parent_scene_surface && verbose_xwayland_logging_enabled())
        {
            log_debug(
                "Failed to find a window for %s to be transient for",
                connection->window_debug_string(window).c_str());
        }
    }
    else if (verbose_xwayland_logging_enabled())
    {
        log_debug(
            "%s is not transient",
            connection->window_debug_string(window).c_str());
    }

    {
        std::lock_guard<std::mutex> lock{this->mutex};
        this->pending_spec(lock).parent = parent_scene_surface;
        set_position(parent_scene_surface, this->cached.top_left, this->pending_spec(lock));
    }
}

void mf::XWaylandSurface::inform_client_of_window_state(WindowState const& new_window_state)
{
    {
        std::lock_guard<std::mutex> lock{mutex};

        if (new_window_state == cached.state)
            return;

        cached.state = new_window_state;
    }

    if (verbose_xwayland_logging_enabled())
    {
        log_debug(
            "%s state set to %s%s%s%s",
            connection->window_debug_string(window).c_str(),
            new_window_state.withdrawn ? "withdrawn, " : "",
            new_window_state.minimized ? "minimized, " : "",
            new_window_state.fullscreen ? "fullscreen, " : "",
            new_window_state.maximized ? "maximized" : "unmaximized");
    }

    WmState wm_state;

    if (new_window_state.withdrawn)
        wm_state = WmState::WITHDRAWN;
    else if (new_window_state.minimized)
        wm_state = WmState::ICONIC;
    else
        wm_state = WmState::NORMAL;

    uint32_t const wm_state_properties[]{
        static_cast<uint32_t>(wm_state),
        XCB_WINDOW_NONE // Icon window
    };
    connection->set_property<XCBType::WM_STATE>(window, connection->wm_state, wm_state_properties);

    if (new_window_state.withdrawn)
    {
        xcb_delete_property(
            *connection,
            window,
            connection->net_wm_state);
    }
    else
    {
        std::vector<xcb_atom_t> net_wm_states;

        if (new_window_state.minimized)
        {
            net_wm_states.push_back(connection->net_wm_state_hidden);
        }
        if (new_window_state.maximized)
        {
            net_wm_states.push_back(connection->net_wm_state_maximized_horz);
            net_wm_states.push_back(connection->net_wm_state_maximized_vert);
        }
        if (new_window_state.fullscreen)
        {
            net_wm_states.push_back(connection->net_wm_state_fullscreen);
        }
        // TODO: Set _NET_WM_STATE_MODAL if appropriate

        connection->set_property<XCBType::ATOM>(window, connection->net_wm_state, net_wm_states);
    }

    connection->flush();
}

void mf::XWaylandSurface::request_scene_surface_state(MirWindowState new_state)
{
    std::shared_ptr<scene::Surface> scene_surface;

    {
        std::lock_guard<std::mutex> lock{mutex};
        scene_surface = weak_scene_surface.lock();
    }

    if (scene_surface && scene_surface->state() != new_state)
    {
        shell::SurfaceSpecification mods;
        mods.state = new_state;
        shell->modify_surface(scene_surface->session().lock(), scene_surface, mods);
    }
}

auto mf::XWaylandSurface::latest_input_timestamp(std::lock_guard<std::mutex> const&) -> std::chrono::nanoseconds
{
    if (surface_observer)
    {
        return surface_observer.value()->latest_timestamp();
    }
    else
    {
        log_warning("Can not get timestamp because surface_observer is null");
        return {};
    }
}
