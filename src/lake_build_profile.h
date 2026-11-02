#pragma once

// Compatibility evidence: docs/ANALYSIS.md. This is NOT the product version.
// Do not expand these identities/RVAs without a separate investigation.
#define LAKE_BUILD007_EXE_SHA256 "e9c9d291f9c20732904dd6b943737537c242508e546cda2afb1f72dfc2374713"
#define LAKE_BUILD007_CONTOUR_SHA256 "bb7d4126a83b775968d49ce52f885b588561838d97ffe062ec46a8208b94c283"

namespace lakefix::build007 {
constexpr unsigned long layoutDimensions = 0xc8aa34;
constexpr unsigned long layoutReturn = 0x369005;
constexpr unsigned long cursorGetterReturn = 0x36fc43;
constexpr unsigned long cursorSetterReturn = 0x36fc55;
} // namespace lakefix::build007
