#
# Copyright (C) 2024 Geon Technologies, LLC
#
# This file is part of composite.
#
# composite is free software: you can redistribute it and/or modify it under the
# terms of the GNU Lesser General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version.
#
# composite is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
# more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see http://www.gnu.org/licenses/.
#
 
# Executable
add_executable(composite-cli
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)
# Includes
target_include_directories(composite-cli
    PRIVATE ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_BINARY_DIR}/include
)
# Linkage
target_link_libraries(composite-cli
    PRIVATE
    argparse
    fmt::fmt-header-only
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    dl
)
