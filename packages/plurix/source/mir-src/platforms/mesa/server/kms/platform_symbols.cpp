/*
 * Copyright © 2015 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2 or 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Andreas Pokorny <andreas.pokorny@canonical.com>
 */

#define MIR_LOG_COMPONENT "mesa-kms"
#include "mir/log.h"

#include "platform.h"
#include "gbm_platform.h"
#include "display_helpers.h"
#include "mir/options/program_option.h"
#include "mir/options/option.h"
#include "mir/udev/wrapper.h"
#include "mir/module_deleter.h"
#include "mir/assert_module_entry_point.h"
#include "mir/libname.h"
#include "mir/console_services.h"
#include "one_shot_device_observer.h"
#include "mir/raii.h"
#include "mir/graphics/egl_error.h"
#include "mir/graphics/gl_config.h"

#include <EGL/egl.h>
#include MIR_SERVER_GL_H
#include "egl_helper.h"
#include <fcntl.h>
#include <sys/ioctl.h>

namespace mg = mir::graphics;
namespace mgm = mg::mesa;
namespace mgc = mir::graphics::common;
namespace mo = mir::options;

namespace
{
char const* bypass_option_name{"bypass"};
char const* host_socket{"host-socket"};

}

mir::UniqueModulePtr<mg::Platform> create_host_platform(
    std::shared_ptr<mo::Option> const& options,
    std::shared_ptr<mir::EmergencyCleanupRegistry> const& emergency_cleanup_registry,
    std::shared_ptr<mir::ConsoleServices> const& console,
    std::shared_ptr<mg::DisplayReport> const& report,
    std::shared_ptr<mir::logging::Logger> const& /*logger*/)
{
    mir::assert_entry_point_signature<mg::CreateHostPlatform>(&create_host_platform);
    // ensure mesa finds the mesa mir-platform symbols

    auto bypass_option = mgm::BypassOption::allowed;
    if (!options->get<bool>(bypass_option_name))
        bypass_option = mgm::BypassOption::prohibited;

    return mir::make_module_ptr<mgm::Platform>(
        report, console, *emergency_cleanup_registry, bypass_option);
}

void add_graphics_platform_options(boost::program_options::options_description& config)
{
    mir::assert_entry_point_signature<mg::AddPlatformOptions>(&add_graphics_platform_options);
    config.add_options()
        (bypass_option_name,
         boost::program_options::value<bool>()->default_value(true),
         "[platform-specific] utilize the bypass optimization for fullscreen surfaces.");
}

namespace
{
class MinimalGLConfig : public mir::graphics::GLConfig
{
public:
    int depth_buffer_bits() const override
    {
        return 0;
    }

    int stencil_buffer_bits() const override
    {
        return 0;
    }
};
}

mg::PlatformPriority probe_graphics_platform(
    std::shared_ptr<mir::ConsoleServices> const& console,
    mo::ProgramOption const& options)
{
    mir::assert_entry_point_signature<mg::PlatformProbe>(&probe_graphics_platform);

    auto nested = options.is_set(host_socket);

    auto udev = std::make_shared<mir::udev::Context>();

    mir::udev::Enumerator drm_devices{udev};
    drm_devices.match_subsystem("drm");
    drm_devices.match_sysname("card[0-9]*");
    drm_devices.scan_devices();

    if (drm_devices.begin() == drm_devices.end())
    {
        mir::log_info("Unsupported: No DRM devices detected");
        return mg::PlatformPriority::unsupported;
    }

    // We also require GBM EGL platform
    auto const* client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!client_extensions)
    {
        // Doesn't support EGL client extensions; Mesa does, so this is unlikely to be mesa.
        mir::log_info("Unsupported: EGL platform does not support client extensions.");
        return mg::PlatformPriority::unsupported;
    }
    if (strstr(client_extensions, "EGL_KHR_platform_gbm") == nullptr)
    {
        // Doesn't support the Khronos-standardised GBM platform…
        mir::log_info("EGL platform does not support EGL_KHR_platform_gbm extension");
        // …maybe we support the old pre-standardised Mesa GBM platform?
        if (strstr(client_extensions, "EGL_MESA_platform_gbm") == nullptr)
        {
            mir::log_info(
                "Unsupported: EGL platform supports neither EGL_KHR_platform_gbm nor EGL_MESA_platform_gbm");
            return mg::PlatformPriority::unsupported;
        }
    }

    // Check for suitability
    mir::Fd tmp_fd;
    for (auto& device : drm_devices)
    {
        auto const devnum = device.devnum();
        if (devnum == makedev(0, 0))
        {
            /* The display connectors attached to the card appear as subdevices
             * of the card[0-9] node.
             * These won't have a device node, so pass on anything that doesn't have
             * a /dev/dri/card* node
             */
            continue;
        }

        try
        {
            auto maximum_suitability = mg::PlatformPriority::best;

            // Rely on the console handing us a DRM master...
            auto const device_cleanup = console->acquire_device(
                major(devnum), minor(devnum),
                std::make_unique<mgc::OneShotDeviceObserver>(tmp_fd)).get();

            if (tmp_fd != mir::Fd::invalid)
            {
                // Check that the drm device is usable by setting the interface version we use (1.4)
                drmSetVersion sv;
                sv.drm_di_major = 1;
                sv.drm_di_minor = 4;
                sv.drm_dd_major = -1;     /* Don't care */
                sv.drm_dd_minor = -1;     /* Don't care */

                if (auto error = -drmSetInterfaceVersion(tmp_fd, &sv))
                {
                    throw std::system_error{
                        error,
                        std::system_category(),
                        std::string{"Failed to set DRM interface version on device "} + device.devnode()};
                }

                /* Check if modesetting is supported on this DRM node
                 * This must be done after drmSetInterfaceVersion() as, for Hysterical Raisins,
                 * drmGetBusid() will return nullptr unless drmSetInterfaceVersion() has already been called
                 */
                auto const busid = std::unique_ptr<char, decltype(&drmFreeBusid)>{
                    drmGetBusid(tmp_fd),
                    &drmFreeBusid
                };

                if (!busid)
                {
                    mir::log_warning(
                        "Failed to query BusID for device %s; cannot check if KMS is available",
                        device.devnode());
                    maximum_suitability = mg::PlatformPriority::supported;
                }
                else
                {
                    switch (auto err = -drmCheckModesettingSupported(busid.get()))
                    {
                    case 0: break;

                    case ENOSYS:
                        if (getenv("MIR_MESA_KMS_DISABLE_MODESET_PROBE") == nullptr)
                        {
                            throw std::runtime_error{std::string{"Device "}+device.devnode()+" does not support KMS"};
                        }

                        mir::log_debug("MIR_MESA_KMS_DISABLE_MODESET_PROBE is set");
                        // Falls through.
                    case EINVAL:
                        mir::log_warning(
                            "Failed to detect whether device %s supports KMS, continuing with lower confidence",
                            device.devnode());
                        maximum_suitability = mg::PlatformPriority::supported;
                        break;

                    default:
                        mir::log_warning("Unexpected error from drmCheckModesettingSupported(): %s (%i), "
                                         "but continuing anyway", strerror(err), err);
                        mir::log_warning("Please file a bug at "
                                         "https://github.com/MirServer/mir/issues containing this message");
                        maximum_suitability = mg::PlatformPriority::supported;
                    }
                }

                mgm::helpers::GBMHelper gbm_device{tmp_fd};
                mgm::helpers::EGLHelper egl{MinimalGLConfig()};

                egl.setup(gbm_device);

                egl.make_current();

                auto const renderer_string = reinterpret_cast<char const*>(glGetString(GL_RENDERER));
                if (!renderer_string)
                {
                    throw mg::gl_error(
                        "Probe failed to query GL renderer");
                }

                if (strncmp(
                    "llvmpipe",
                    renderer_string,
                    strlen("llvmpipe")) == 0)
                {
                     mir::log_info("Detected software renderer: %s", renderer_string);
                     // TODO:   Check if any *other* DRM devices support HW acceleration, and
                     //         use them instead.
                     maximum_suitability = mg::PlatformPriority::supported;
                }

                return maximum_suitability;
            }
        }
        catch (std::exception const& e)
        {
            mir::log_info("%s", e.what());
        }
    }

    if (nested)
        return mg::PlatformPriority::supported;

    /*
     * We haven't found any suitable devices. We've complained above about
     * the reason, so we don't have to pretend to be supported.
     */
    return mg::PlatformPriority::unsupported;
}

namespace
{
mir::ModuleProperties const description = {
    "mir:mesa-kms",
    MIR_VERSION_MAJOR,
    MIR_VERSION_MINOR,
    MIR_VERSION_MICRO,
    mir::libname()
};
}

mir::ModuleProperties const* describe_graphics_module()
{
    mir::assert_entry_point_signature<mg::DescribeModule>(&describe_graphics_module);
    return &description;
}

mir::UniqueModulePtr<mir::graphics::DisplayPlatform> create_display_platform(
    std::shared_ptr<mo::Option> const& options,
    std::shared_ptr<mir::EmergencyCleanupRegistry> const& emergency_cleanup_registry,
    std::shared_ptr<mir::ConsoleServices> const& console,
    std::shared_ptr<mg::DisplayReport> const& report,
    std::shared_ptr<mir::logging::Logger> const&)
{
    mir::assert_entry_point_signature<mg::CreateDisplayPlatform>(&create_display_platform);
    // ensure mesa finds the mesa mir-platform symbols

    auto bypass_option = mgm::BypassOption::allowed;
    if (!options->get<bool>(bypass_option_name))
        bypass_option = mgm::BypassOption::prohibited;

    return mir::make_module_ptr<mgm::Platform>(
        report, console, *emergency_cleanup_registry, bypass_option);
}

mir::UniqueModulePtr<mir::graphics::RenderingPlatform> create_rendering_platform(
    std::shared_ptr<mir::options::Option> const& options,
    std::shared_ptr<mir::graphics::PlatformAuthentication> const& platform_authentication)
{
    mir::assert_entry_point_signature<mg::CreateRenderingPlatform>(&create_rendering_platform);

    auto bypass_option = mgm::BypassOption::allowed;
    if (options->is_set(bypass_option_name) && !options->get<bool>(bypass_option_name))
        bypass_option = mgm::BypassOption::prohibited;
    return mir::make_module_ptr<mgm::GBMPlatform>(
        bypass_option, mgm::BufferImportMethod::gbm_native_pixmap, platform_authentication);
}
