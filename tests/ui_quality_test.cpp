#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/ui_quality.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {
int checks;
int failures;
void Check(bool condition, const char* name) {
    ++checks;
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
void TestFonts() {
    for (const char* face : {"Arial", "aRiAl", "Courier New", "COURIER NEW"}) {
        LOGFONTA input{};
        input.lfHeight = -13;
        input.lfWidth = 2;
        input.lfWeight = FW_BOLD;
        input.lfItalic = 1;
        input.lfCharSet = ANSI_CHARSET;
        input.lfQuality = NONANTIALIASED_QUALITY;
        strcpy_s(input.lfFaceName, face);
        const LOGFONTA original = input;
        LOGFONTA output{};
        Check(lakefix::PrepareSmoothFont(input, output), "observed scalable face accepts grayscale AA");
        Check(std::memcmp(&input, &original, sizeof(input)) == 0, "caller LOGFONT is not modified");
        LOGFONTA expected = original;
        expected.lfQuality = ANTIALIASED_QUALITY;
        Check(std::memcmp(&output, &expected, sizeof(output)) == 0, "only quality changes; all requested metrics and face stay identical");
    }
    for (const char* face : {"Small Fonts", "Alphanumeric LCD", "Symbol", "Arial Narrow", ""}) {
        LOGFONTA input{}, output{};
        strcpy_s(input.lfFaceName, face);
        output.lfHeight = 123;
        Check(!lakefix::PrepareSmoothFont(input, output) && output.lfHeight == 123,
              "raster, LCD and unobserved faces pass through unchanged");
    }
    LOGFONTA input{}, output{};
    strcpy_s(input.lfFaceName, "Arial");
    input.lfQuality = ANTIALIASED_QUALITY;
    Check(!lakefix::PrepareSmoothFont(input, output), "already-antialiased request is not rewritten");
    memset(input.lfFaceName, 'A', sizeof(input.lfFaceName));
    Check(!lakefix::PrepareSmoothFont(input, output), "unterminated face is not mistaken for a supported font");
}
void TestCursorRectangles() {
    POINT rect[] = {{100, 200}, {120, 220}};
    Check(lakefix::HideSoftwareCursorRectangle(rect, 2) && rect[0].x == -16384 && rect[0].y == -16384 &&
          rect[1].x == -16364 && rect[1].y == -16364, "cursor paint becomes an off-canvas rectangle with the same dimensions");
    const LONG low = (std::numeric_limits<LONG>::min)(), high = (std::numeric_limits<LONG>::max)();
    const RECT invalid[] = {{0, 0, 0, 20}, {0, 0, 20, 0}, {20, 20, 0, 0}, {0, 0, 513, 10},
                            {0, 0, 10, 513}, {low, low, high, high}};
    for (const RECT& value : invalid) {
        POINT points[] = {{value.left, value.top}, {value.right, value.bottom}};
        const POINT before[] = {points[0], points[1]};
        Check(!lakefix::HideSoftwareCursorRectangle(points, 2) && std::memcmp(points, before, sizeof(points)) == 0,
              "invalid or unexpected cursor geometry is preserved without overflow");
    }
    POINT edge[] = {{high - 512, high - 512}, {high, high}};
    Check(lakefix::HideSoftwareCursorRectangle(edge, 2) && edge[1].x == -15872,
          "maximum supported cursor rectangle near LONG_MAX maps safely");
    Check(!lakefix::HideSoftwareCursorRectangle(nullptr, 2), "null cursor rectangle is rejected");
    Check(!lakefix::HideSoftwareCursorRectangle(rect, 1) && !lakefix::HideSoftwareCursorRectangle(rect, 3),
          "only a two-point rectangle can be changed");
    for (ULONG_PTR rva : {0x283fbUL, 0x2a453UL, 0x2a4e2UL})
        Check(lakefix::IsCursorPaintReturn(rva), "verified cursor-paint return address accepted");
    for (ULONG_PTR rva : {0UL, 0x283faUL, 0x283fcUL, 0x36fc43UL, 0x36fc55UL})
        Check(!lakefix::IsCursorPaintReturn(rva), "input/cursor-warp and unrelated callsites are never suppressed");
}
void TestCursorVisibilityBalance() {
    int counter = -3, calls = 0;
    unsigned credits = 0;
    const auto adjust = [&](BOOL show) noexcept { ++calls; return counter += show ? 1 : -1; };
    lakefix::AcquireCursorVisibility(credits, adjust);
    Check(counter == 0 && credits == 3 && calls == 3, "negative native visibility counter acquires only the required increments");
    lakefix::AcquireCursorVisibility(credits, adjust);
    Check(counter == 0 && credits == 3 && calls == 5, "an already-visible counter does not accumulate visibility credits");
    lakefix::ReleaseCursorVisibility(credits, adjust);
    Check(counter == -3 && credits == 0, "cleanup restores precisely our increments, not a guessed global cursor state");
    const int before = calls;
    lakefix::ReleaseCursorVisibility(credits, adjust);
    Check(calls == before && counter == -3, "repeated visibility cleanup is harmless");
    counter = 2;
    lakefix::AcquireCursorVisibility(credits, adjust);
    Check(counter == 2 && credits == 0, "positive preexisting visibility count is preserved");
    counter = -100;
    calls = 0;
    lakefix::AcquireCursorVisibility(credits, adjust);
    Check(counter == -84 && credits == 16 && calls == 16, "unexpectedly negative count has a strict bounded fallback");
    lakefix::AcquireCursorVisibility(credits, adjust);
    Check(calls == 16 && credits == 16, "visibility cap cannot cause an unbounded show loop");
    lakefix::ReleaseCursorVisibility(credits, adjust);
    Check(counter == -100 && credits == 0, "bounded fallback also restores its own increments");
}

void TestCursorVisibilityPolicy() {
    for (ULONG_PTR rva : {0x11d76UL, 0x11d84UL, 0x6465dUL, 0x6466aUL}) {
        Check(lakefix::IsCursorVisibilityReturnRva(rva) &&
              lakefix::ShouldVirtualizeCursorVisibility(true, true, true, rva),
              "only verified renderer normalization returns accept virtual ShowCursor counts");
    }
    for (ULONG_PTR rva : {0UL, 0x11d75UL, 0x11d77UL, 0x11d83UL, 0x11d85UL,
                          0x6465cUL, 0x6465eUL, 0x64669UL, 0x6466bUL, 0x283fbUL, 0x36fc43UL}) {
        Check(!lakefix::IsCursorVisibilityReturnRva(rva) &&
              !lakefix::ShouldVirtualizeCursorVisibility(true, true, true, rva),
              "neighboring, non-renderer and unrelated return sites keep real ShowCursor semantics");
    }
}

void TestVirtualCursorVisibility() {
    lakefix::VirtualCursorVisibility logical;
    int physical = 0, calls = 0, hides = 0;
    unsigned credits = 1; // Native -1 plus our one outstanding visibility credit.
    const auto real = [&](BOOL show) noexcept {
        ++calls;
        if (!show) ++hides;
        return physical += show ? 1 : -1;
    };
    const auto dispatch = [&](BOOL show, bool enabled, bool hasClient, bool owningThread, ULONG_PTR rva) noexcept {
        if (lakefix::ShouldVirtualizeCursorVisibility(enabled, hasClient, owningThread, rva))
            return logical.Adjust(show, credits, real);
        return real(show);
    };
    Check(dispatch(TRUE, false, true, true, 0x11d76) == 1 && calls == 1 && !logical.initialized,
          "SystemCursor off forwards once without probing or initializing the logical helper");
    Check(dispatch(FALSE, false, true, true, 0x11d84) == 0 && calls == 2 && !logical.initialized,
          "SystemCursor off preserves real hide results at known return sites");
    Check(dispatch(TRUE, true, false, true, 0x6465d) == 1 && calls == 3 && !logical.initialized,
          "no attached client keeps normalization calls native");
    Check(dispatch(FALSE, true, true, false, 0x6466a) == 0 && calls == 4 && !logical.initialized,
          "another thread keeps normalization calls native");
    Check(dispatch(TRUE, true, true, true, 0) == 1 && calls == 5 && !logical.initialized,
          "a caller outside the renderer never initializes logical visibility");
    Check(dispatch(FALSE, true, true, true, 0x11d85) == 0 && calls == 6 && !logical.initialized,
          "an unlisted renderer return still calls real ShowCursor");

    Check(dispatch(TRUE, true, true, true, 0x11d76) == 0 && logical.count == 0 &&
          logical.initialized && physical == 0 && calls == 8 && hides == 4 && credits == 1,
          "first virtual call balances its real probe, subtracts credits and returns the logical increment");
    Check(dispatch(FALSE, true, true, true, 0x11d84) == -1 && physical == 0 && calls == 8,
          "known native hide becomes logical -1 without hiding the OS cursor");

    const int callsBefore = calls, hidesBefore = hides;
    const ULONG_PTR pairs[][2] = {{0x11d76, 0x11d84}, {0x6465d, 0x6466a}};
    bool normalized = true;
    for (unsigned i = 0; i < 1000; ++i) {
        const auto& pair = pairs[i % 2];
        int shown = -1, hidden = 0;
        for (unsigned attempt = 0; attempt < 8 && shown < 0; ++attempt)
            shown = dispatch(TRUE, true, true, true, pair[0]);
        for (unsigned attempt = 0; attempt < 8 && hidden >= 0; ++attempt)
            hidden = dispatch(FALSE, true, true, true, pair[1]);
        normalized = normalized && shown == 0 && hidden == -1;
    }
    Check(normalized && logical.count == -1, "1000 frame/dialog normalizations terminate at logical -1");
    Check(calls == callsBefore && hides == hidesBefore && physical == 0 && credits == 1,
          "1000 normalizations neither call real ShowCursor nor consume visibility credits");
    credits = 16;
    Check(dispatch(TRUE, true, true, true, 0x6465d) == 0 &&
          dispatch(FALSE, true, true, true, 0x6466a) == -1 && calls == callsBefore && physical == 0,
          "later credit changes never trigger a second probe or reseed the logical counter");

    const int low = (std::numeric_limits<int>::min)(), high = (std::numeric_limits<int>::max)();
    lakefix::VirtualCursorVisibility maximum{true, high}, minimum{true, low};
    Check(maximum.Adjust(TRUE, 0, real) == high && maximum.count == high,
          "logical show at INT_MAX saturates without signed overflow");
    Check(maximum.Adjust(FALSE, 0, real) == high - 1,
          "logical hide can leave the upper boundary normally");
    Check(minimum.Adjust(FALSE, 0, real) == low && minimum.count == low,
          "logical hide at INT_MIN saturates without signed overflow");
    Check(minimum.Adjust(TRUE, 0, real) == low + 1 && calls == callsBefore,
          "logical show can leave the lower boundary without any real API calls");

    lakefix::VirtualCursorVisibility underflow;
    physical = low;
    Check(underflow.Adjust(FALSE, 16, real) == low && underflow.count == low &&
          physical == low && calls == callsBefore + 2,
          "seeding INT_MIN minus visibility credits clamps safely after a balanced probe");
    lakefix::VirtualCursorVisibility largeCredits;
    physical = 0;
    Check(largeCredits.Adjust(TRUE, (std::numeric_limits<unsigned>::max)(), real) == low + 1 &&
          physical == 0 && calls == callsBefore + 4,
          "unsigned credit subtraction uses wide arithmetic rather than wrapping the logical count");
}
} // namespace

int wmain() {
    TestFonts();
    TestCursorRectangles();
    TestCursorVisibilityBalance();
    TestCursorVisibilityPolicy();
    TestVirtualCursorVisibility();
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
