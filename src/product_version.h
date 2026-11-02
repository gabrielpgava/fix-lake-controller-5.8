#pragma once

// Product identity, independent of the proprietary compatibility profile.
// Build scripts force-include the generated stamp before compiling every own
// component. A manual build is explicitly untracked and cannot be packaged.
#define LAKE_FIX_VERSION "0.7.0"
#define LAKE_FIX_ABI_VERSION 2
#define LAKE_FIX_CONFIG_SCHEMA 1
#define LAKE_FIX_MAX_INI_BYTES 131072
#ifndef LAKE_FIX_BUILD_ID
#define LAKE_FIX_BUILD_ID "development-untracked"
#endif
#if defined(LAKE_FIX_TESTING)
#define LAKE_FIX_COMPONENT_BUILD_ID LAKE_FIX_BUILD_ID "-fixture"
#else
#define LAKE_FIX_COMPONENT_BUILD_ID LAKE_FIX_BUILD_ID
#endif

#define LAKE_FIX_WIDEN_IMPL(value) L##value
#define LAKE_FIX_WIDEN(value) LAKE_FIX_WIDEN_IMPL(value)
#define LAKE_FIX_VERSION_W LAKE_FIX_WIDEN(LAKE_FIX_VERSION)
#define LAKE_FIX_BUILD_ID_W LAKE_FIX_WIDEN(LAKE_FIX_BUILD_ID)
