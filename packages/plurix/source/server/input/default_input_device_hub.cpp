/*
 * Copyright © 2014-2015 Canonical Ltd.
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
 * Authored by: Andreas Pokorny <andreas.pokorny@canonical.com>
 */

#include "default_input_device_hub.h"
#include "default_device.h"

#include "mir/input/input_device.h"
#include "mir/input/input_device_observer.h"
#include "mir/input/mir_pointer_config.h"
#include "mir/input/mir_touchpad_config.h"
#include "mir/input/mir_keyboard_config.h"
#include "mir/geometry/point.h"
#include "mir/server_status_listener.h"
#include "mir/dispatch/multiplexing_dispatchable.h"
#include "mir/dispatch/action_queue.h"
#include "mir/server_action_queue.h"
#include "mir/cookie/authority.h"
#define MIR_LOG_COMPONENT "Input"
#include "mir/log.h"

#include "boost/throw_exception.hpp"

#include <algorithm>
#include <atomic>
#include <memory>

namespace mi = mir::input;

struct mi::ExternalInputDeviceHub::Internal : InputDeviceObserver
{
    Internal(std::shared_ptr<ServerActionQueue> const& queue) :
        observer_queue{queue}
    {}
    void device_added(std::shared_ptr<Device> const& device) override;
    void device_changed(std::shared_ptr<Device> const& device) override;
    void device_removed(std::shared_ptr<Device> const& device) override;
    void changes_complete() override;

    std::shared_ptr<ServerActionQueue> const observer_queue;
    ThreadSafeList<std::shared_ptr<InputDeviceObserver>> observers;
    std::vector<std::shared_ptr<Device>> devices_added;
    std::vector<std::shared_ptr<Device>> devices_changed;
    std::vector<std::shared_ptr<Device>> devices_removed;
    std::vector<std::shared_ptr<Device>> handles;
};

mi::ExternalInputDeviceHub::ExternalInputDeviceHub(std::shared_ptr<mi::InputDeviceHub> const& hub, std::shared_ptr<mir::ServerActionQueue> const& queue)
    : data{std::make_shared<mi::ExternalInputDeviceHub::Internal>(queue)}, hub{hub}
{
    hub->add_observer(data);
}

mi::ExternalInputDeviceHub::~ExternalInputDeviceHub()
{
    data->observer_queue->pause_processing_for(data.get());
}

void mi::ExternalInputDeviceHub::add_observer(std::shared_ptr<InputDeviceObserver> const& observer)
{
    data->observer_queue->enqueue(
        data.get(),
        [observer, data = this->data]
        {
            for (auto const& item : data->handles)
                observer->device_added(item);
            observer->changes_complete();
            data->observers.add(observer);
        });
}

void mi::ExternalInputDeviceHub::remove_observer(std::weak_ptr<InputDeviceObserver> const& obs)
{
    auto observer = obs.lock();
    if (observer)
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool removed{false};

        data->observer_queue->enqueue_with_guaranteed_execution(
            [&,this]
            {
                // Unless remove() is on the same thread as add() external synchronization would be needed
                data->observers.remove(observer);

                std::lock_guard<decltype(mutex)> lock{mutex};
                removed = true;
                cv.notify_one();
            });

        std::unique_lock<decltype(mutex)> lock{mutex};

        // Before returning wait for the remove - otherwise notifications can still happen
        cv.wait(lock, [&] { return removed; });
    }
}

void mi::ExternalInputDeviceHub::for_each_input_device(std::function<void(Device const& device)> const& callback)
{
    hub->for_each_input_device(callback);
}

void mi::ExternalInputDeviceHub::for_each_mutable_input_device(std::function<void(Device& device)> const& callback)
{
    hub->for_each_mutable_input_device(callback);
}

void mi::ExternalInputDeviceHub::Internal::device_added(std::shared_ptr<Device> const& device)
{
    devices_added.push_back(device);
}

void mi::ExternalInputDeviceHub::Internal::device_changed(std::shared_ptr<Device> const& device)
{
    devices_changed.push_back(device);
}

void mi::ExternalInputDeviceHub::Internal::device_removed(std::shared_ptr<Device> const& device)
{
    devices_removed.push_back(device);
}

void mi::ExternalInputDeviceHub::Internal::changes_complete()
{
    decltype(devices_added) added, changed, removed;

    std::swap(devices_added, added);
    std::swap(devices_changed, changed);
    std::swap(devices_removed, removed);

    if (!(added.empty() && changed.empty() && removed.empty()))
        observer_queue->enqueue(
            this,
            [this, added, changed, removed]
            {
                observers.for_each([&](std::shared_ptr<InputDeviceObserver> const& observer)
                    {
                        for (auto const& dev : added)
                            observer->device_added(dev);
                        for (auto const& dev : changed)
                            observer->device_changed(dev);
                        for (auto const& dev : removed)
                            observer->device_removed(dev);
                        observer->changes_complete();
                    });

                auto end_it = handles.end();
                for (auto const& dev : removed)
                    end_it = remove(begin(handles), end(handles), dev);
                if (end_it != handles.end())
                    handles.erase(end_it, end(handles));
                for (auto const& dev : added)
                    handles.push_back(dev);
            });
}

mi::DefaultInputDeviceHub::DefaultInputDeviceHub(
    std::shared_ptr<mi::Seat> const& seat,
    std::shared_ptr<dispatch::MultiplexingDispatchable> const& input_multiplexer,
    std::shared_ptr<mir::cookie::Authority> const& cookie_authority,
    std::shared_ptr<mi::KeyMapper> const& key_mapper,
    std::shared_ptr<mir::ServerStatusListener> const& server_status_listener)
    : seat{seat},
      input_dispatchable{input_multiplexer},
      device_queue(std::make_shared<dispatch::ActionQueue>()),
      cookie_authority(cookie_authority),
      key_mapper(key_mapper),
      server_status_listener(server_status_listener),
      device_id_generator{0}
{
    input_dispatchable->add_watch(device_queue);
}

void mi::DefaultInputDeviceHub::add_device(std::shared_ptr<InputDevice> const& device)
{
    if (!device)
        BOOST_THROW_EXCEPTION(std::invalid_argument("Invalid input device"));

    auto it = find_if(devices.cbegin(),
                      devices.cend(),
                      [&device](std::unique_ptr<RegisteredDevice> const& item)
                      {
                          return item->device_matches(device);
                      });

    if (it == end(devices))
    {
        auto queue = std::make_shared<dispatch::ActionQueue>();
        auto handle = restore_or_create_device(*device, queue);
        // send input device info to observer loop..
        devices.push_back(std::make_unique<RegisteredDevice>(
            device, handle->id(), queue, cookie_authority, handle));

        auto const& dev = devices.back();
        add_device_handle(handle);

        seat->add_device(*handle);
        dev->start(seat, input_dispatchable);
    }
    else
    {
        log_error("Input device %s added twice", device->get_device_info().name.c_str());
        BOOST_THROW_EXCEPTION(std::logic_error("Input device already managed by server"));
    }
}

void mi::DefaultInputDeviceHub::remove_device(std::shared_ptr<InputDevice> const& device)
{
    if (!device)
        BOOST_THROW_EXCEPTION(std::invalid_argument("Invalid input device"));

    auto pos = remove_if(
        begin(devices),
        end(devices),
        [&device,this](std::unique_ptr<RegisteredDevice> const& item)
        {
            if (item->device_matches(device))
            {
                store_device_config(*item->handle);
                auto seat = item->seat;
                if (seat)
                {
                    seat->remove_device(*item->handle);
                    item->stop(input_dispatchable);
                }
                remove_device_handle(item->id());

                return true;
            }
            return false;
        });
    if (pos == end(devices))
    {
        log_error("Input device %s not found", device->get_device_info().name.c_str());
        BOOST_THROW_EXCEPTION(std::logic_error("Input device not managed by server"));
    }

    devices.erase(pos, end(devices));
}

mi::DefaultInputDeviceHub::RegisteredDevice::RegisteredDevice(
    std::shared_ptr<InputDevice> const& dev,
    MirInputDeviceId device_id,
    std::shared_ptr<dispatch::ActionQueue> const& queue,
    std::shared_ptr<mir::cookie::Authority> const& cookie_authority,
    std::shared_ptr<mi::DefaultDevice> const& handle)
    : handle(handle),
      device_id(device_id),
      cookie_authority(cookie_authority),
      device(dev),
      queue(queue)
{
}

MirInputDeviceId mi::DefaultInputDeviceHub::create_new_device_id()
{
    return ++device_id_generator;
}

MirInputDeviceId mi::DefaultInputDeviceHub::RegisteredDevice::id()
{
    return device_id;
}

void mi::DefaultInputDeviceHub::RegisteredDevice::handle_input(std::shared_ptr<MirEvent> const& event)
{
    auto type = mir_event_get_type(event.get());

    if (type != mir_event_type_input &&
        type != mir_event_type_input_device_state)
        BOOST_THROW_EXCEPTION(std::invalid_argument("Invalid input event received from device"));

    if (!seat)
        return;

    seat->dispatch_event(event);
}

bool mi::DefaultInputDeviceHub::RegisteredDevice::device_matches(std::shared_ptr<InputDevice> const& dev) const
{
    return dev == device;
}

void mi::DefaultInputDeviceHub::RegisteredDevice::start(std::shared_ptr<Seat> const& seat, std::shared_ptr<dispatch::MultiplexingDispatchable> const& multiplexer)
{
    multiplexer->add_watch(queue);

    this->seat = seat;
    builder = std::make_unique<DefaultEventBuilder>(device_id, cookie_authority, seat);
    device->start(this, builder.get());
}

void mi::DefaultInputDeviceHub::RegisteredDevice::stop(std::shared_ptr<dispatch::MultiplexingDispatchable> const& multiplexer)
{
    multiplexer->remove_watch(queue);
    handle->disable_queue();

    device->stop();
    seat = nullptr;
    queue.reset();
    builder.reset();
}

mir::geometry::Rectangle mi::DefaultInputDeviceHub::RegisteredDevice::bounding_rectangle() const
{
    if (!seat)
        BOOST_THROW_EXCEPTION(std::runtime_error("Device not started and has no seat assigned"));

    return seat->bounding_rectangle();
}

mi::OutputInfo mi::DefaultInputDeviceHub::RegisteredDevice::output_info(uint32_t output_id) const
{
    if (!seat)
        BOOST_THROW_EXCEPTION(std::runtime_error("Device not started and has no seat assigned"));

    return seat->output_info(output_id);
}

void mi::DefaultInputDeviceHub::RegisteredDevice::key_state(std::vector<uint32_t> const& scan_codes)
{
    if (!seat)
        BOOST_THROW_EXCEPTION(std::runtime_error("Device not started and has no seat assigned"));

    seat->set_key_state(*handle, scan_codes);
}

void mi::DefaultInputDeviceHub::RegisteredDevice::pointer_state(MirPointerButtons buttons)
{
    if (!seat)
        BOOST_THROW_EXCEPTION(std::runtime_error("Device not started and has no seat assigned"));

    seat->set_pointer_state(*handle, buttons);
}

void mi::DefaultInputDeviceHub::add_observer(std::shared_ptr<InputDeviceObserver> const& observer)
{
    device_queue->enqueue(
        [this,observer]()
        {
            std::unique_lock<std::mutex> lock(handles_guard);
            for (auto const& item : handles)
                observer->device_added(item);
            observer->changes_complete();
            observers.add(observer);
        });
}

void mi::DefaultInputDeviceHub::for_each_input_device(std::function<void(Device const&)> const& callback)
{
    std::unique_lock<std::mutex> lock(handles_guard);
    for (auto const& item : handles)
        callback(*item);
}

void mi::DefaultInputDeviceHub::for_each_mutable_input_device(std::function<void(Device&)> const& callback)
{
    {
        std::unique_lock<std::mutex> lock(changed_devices_guard);
        changed_devices = std::make_unique<std::vector<std::shared_ptr<Device>>>();
    }

    {
        std::unique_lock<std::mutex> lock(handles_guard);
        for (auto const& item : handles)
            callback(*item);
    }

    emit_changed_devices();
}

void mi::DefaultInputDeviceHub::remove_observer(std::weak_ptr<InputDeviceObserver> const& element)
{
    auto observer = element.lock();
    observers.remove(observer);
}

void mi::DefaultInputDeviceHub::add_device_handle(std::shared_ptr<DefaultDevice> const& handle)
{
    {
        std::unique_lock<std::mutex> lock(handles_guard);
        handles.push_back(handle);
    }

    observers.for_each([&handle](std::shared_ptr<InputDeviceObserver> const& observer)
        {
            observer->device_added(handle);
            observer->changes_complete();
        });

    if (!ready)
    {
        server_status_listener->ready_for_user_input();
        ready = true;
    }
}

void mi::DefaultInputDeviceHub::remove_device_handle(MirInputDeviceId id)
{
    decltype(handles) removed_devices;
    decltype(handles.size()) no_of_devices{};

    {
        std::unique_lock<std::mutex> lock(handles_guard);
        auto handle_it = remove_if(
            begin(handles),
            end(handles),
            [&](auto const& handle)
                {
                if (handle->id() != id)
                    return false;
                removed_devices.push_back(handle);
                return true;
                });

        if (handle_it == end(handles))
            return;

        handles.erase(handle_it, end(handles));
        no_of_devices = handles.size();
    }

    for (auto const& handle : removed_devices)
    {
        observers.for_each([&](std::shared_ptr<InputDeviceObserver> const& observer)
            {
                observer->device_removed(handle);
                observer->changes_complete();
            });
    }

    removed_devices.clear();

    if (ready && 0 == no_of_devices)
    {
        ready = false;
        server_status_listener->stop_receiving_input();
    }
}

void mi::DefaultInputDeviceHub::device_changed(Device* dev)
{
    auto more_changes_in_progress = false;
    {
        std::unique_lock<std::mutex> lock(changed_devices_guard);
        if (changed_devices)
        {
            more_changes_in_progress = true;
            auto dev_it = find_if(begin(handles), end(handles), [dev](auto const& ptr){return ptr.get() == dev;});
            changed_devices->push_back(*dev_it);
        }
    }

    if (!more_changes_in_progress)
    {
        std::shared_ptr<Device> device;
        {
            std::unique_lock<std::mutex> lock(handles_guard);
            auto dev_it = find_if(begin(handles), end(handles), [dev](auto const& ptr){return ptr.get() == dev;});

            if (dev_it==end(handles))
                return;

            device = *dev_it;
        }

        observers.for_each([&](std::shared_ptr<InputDeviceObserver> const& observer)
            {
                observer->device_changed(device);
                observer->changes_complete();
            });
    }
}

void mi::DefaultInputDeviceHub::emit_changed_devices()
{
    std::vector<std::shared_ptr<mi::Device>> devices_to_notify;
    {
        std::unique_lock<std::mutex> lock(changed_devices_guard);
        if (changed_devices)
        {
            std::swap(devices_to_notify, *changed_devices);
            changed_devices.reset();
        }
    }
    {
        observers.for_each([&](std::shared_ptr<InputDeviceObserver> const& observer)
            {
                for (auto const& dev : devices_to_notify)
                    observer->device_changed(dev);
                observer->changes_complete();
            });
    }
}

void mi::DefaultInputDeviceHub::store_device_config(mi::DefaultDevice const& dev)
{
    std::lock_guard<std::mutex> lock(stored_configurations_guard);
    stored_devices.push_back(dev.config());
}

mir::optional_value<MirInputDevice>
mi::DefaultInputDeviceHub::get_stored_device_config(std::string const& id)
{
    mir::optional_value<MirInputDevice> optional_config;
    std::lock_guard<std::mutex> lock(stored_configurations_guard);
    auto pos = remove_if(
        begin(stored_devices),
        end(stored_devices),
        [&optional_config,id](auto const& handle)
        {
            if (id == handle.unique_id())
            {
                optional_config = handle;
                return true;
            }
            return false;
        });
    stored_devices.erase(pos, end(stored_devices));

    return optional_config;
}

std::shared_ptr<mi::DefaultDevice> mi::DefaultInputDeviceHub::restore_or_create_device(
    mi::InputDevice& device, std::shared_ptr<mir::dispatch::ActionQueue> const& queue)
{
    auto device_config = get_stored_device_config(device.get_device_info().unique_id);

    if (device_config.is_set())
        return std::make_shared<DefaultDevice>(
            device_config.value(),
            queue,
            device,
            key_mapper,
            [this](Device *d){device_changed(d);});
    else
        return std::make_shared<DefaultDevice>(
            create_new_device_id(),
            queue,
            device,
            key_mapper,
            [this](Device *d){device_changed(d);});
}
