/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
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
