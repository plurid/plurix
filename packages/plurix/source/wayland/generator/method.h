/*
 * Copyright © 2018 Canonical Ltd.
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
 * Authored By: William Wold <william.wold@canonical.com>
 */

#ifndef MIR_WAYLAND_GENERATOR_METHOD_H
#define MIR_WAYLAND_GENERATOR_METHOD_H

#include "emitter.h"
#include "argument.h"

#include <experimental/optional>
#include <functional>
#include <unordered_map>

namespace xmlpp
{
class Element;
}

class Method
{
public:
    Method(xmlpp::Element const& node, std::string const& class_name, bool is_event);

    Emitter types_str() const;
    Emitter types_declare() const;
    Emitter types_init() const;
    Emitter wl_message_init() const;

    void populate_required_interfaces(std::set<std::string>& interfaces) const; // fills the set with interfaces used

protected:

    bool use_null_types() const;

    static int get_since_version(xmlpp::Element const& node);

    std::string const name;
    std::string const class_name;
    int const min_version;
    std::vector<Argument> arguments;
};

#endif // MIR_WAYLAND_GENERATOR_METHOD_H
