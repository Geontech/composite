/*
 * Copyright (C) 2024 Geon Technologies, LLC
 *
 * This file is part of composite.
 *
 * composite is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * composite is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */
 
#pragma once

#include "composite/component.hpp"

#include <dlfcn.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <vector>

namespace composite {

auto close_func(void* p) -> void;

using component_handles_type = std::vector<std::unique_ptr<void, decltype(&close_func)>>;

auto make_component(const nlohmann::json& comp_json, component_handles_type& handles) -> std::shared_ptr<composite::component>;

} // namespace composite