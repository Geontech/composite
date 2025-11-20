/*
 * Copyright (C) 2025 Geon Technologies, LLC
 *
 * This file is part of composite.
 *
 * composite is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * composite is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#pragma once

/**
 * @file export.hpp
 * @brief Symbol visibility macros for shared library exports
 *
 * COMPOSITE_API marks symbols that should be visible across shared library boundaries.
 * This is critical for singletons like dpdk::manager to ensure all DSOs see the same instance.
 */

#if defined(_WIN32) || defined(_WIN64)
    // Windows DLL export/import
    #ifdef COMPOSITE_EXPORTS
        #define COMPOSITE_API __declspec(dllexport)
    #else
        #define COMPOSITE_API __declspec(dllimport)
    #endif
#else
    // GCC/Clang visibility attribute
    #define COMPOSITE_API __attribute__((visibility("default")))
#endif
