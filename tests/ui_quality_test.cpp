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

// Scripted GetCursorInfo/ShowCursor callbacks only. Never change the desktop's
// actual cursor, visibility count or position from this test executable.
struct FakeCursor {
    int counter = 0;
    unsigned queries = 0, shows = 0, hides = 0;
    bool sizesCorrect = true, inputsClear = true;
    BOOL results[2]{TRUE, TRUE};
    DWORD flags[2]{CURSOR_SHOWING, CURSOR_SHOWING};
    HCURSOR handles[2];

    explicit FakeCursor(HCURSOR expected) noexcept : handles{expected, expected} {}
    BOOL Query(CURSORINFO* info) noexcept {
        const unsigned reply = queries++ ? 1 : 0;
        sizesCorrect = sizesCorrect && info->cbSize == sizeof(*info);
        inputsClear = inputsClear && !info->flags && !info->hCursor &&
                      !info->ptScreenPos.x && !info->ptScreenPos.y;
        info->flags = flags[reply];
        info->hCursor = handles[reply];
        info->ptScreenPos = {123, 456};
        SetLastError(0x9150 + queries);
        return results[reply]; // Failed queries may still leave dirty output.
    }
    int Adjust(BOOL show) noexcept {
        if (show) ++shows; else ++hides;
        counter += show ? 1 : -1;
        SetLastError(0x9160);
        return counter;
    }
    lakefix::CursorVisibilitySample Ensure(unsigned& credits) noexcept {
        return lakefix::EnsureCursorVisibility(credits,
            [&](CURSORINFO* info) noexcept -> BOOL { return Query(info); },
            [&](BOOL show) noexcept { return Adjust(show); });
    }
    bool Confirm(const lakefix::CursorVisibilitySample& sample, HCURSOR expected) noexcept {
        return lakefix::ConfirmSystemCursor(sample, expected,
            [&](CURSORINFO* info) noexcept -> BOOL { return Query(info); });
    }
    void Release(unsigned& credits) noexcept {
        lakefix::ReleaseCursorVisibility(credits, [&](BOOL show) noexcept { return Adjust(show); });
    }
};

void TestCursorVisibilitySamples() {
    const auto expected = reinterpret_cast<HCURSOR>(static_cast<UINT_PTR>(0x1234));
    const auto unexpected = reinterpret_cast<HCURSOR>(static_cast<UINT_PTR>(0x5678));
    {
        FakeCursor cursor(expected); unsigned credits = 0;
        SetLastError(0x9101);
        const auto sample = cursor.Ensure(credits);
        Check(sample.queried && !sample.adjusted && sample.info.flags == CURSOR_SHOWING &&
              sample.info.hCursor == expected && cursor.queries == 1 && !cursor.shows && !cursor.hides && !credits &&
              GetLastError() == 0x9101, "visible expected cursor uses one query without ShowCursor or LastError leakage");
        SetLastError(0x9101);
        Check(cursor.Confirm(sample, expected) && cursor.queries == 1 && !cursor.shows && !cursor.hides &&
              GetLastError() == 0x9101, "same-call visible arrow confirmation reuses only the unchanged visibility sample");
        Check(cursor.sizesCorrect && cursor.inputsClear && sample.info.cbSize == sizeof(CURSORINFO),
              "initial cursor query receives a sized, zero-initialized CURSORINFO");
        SetLastError(0x9101);
        Check(!cursor.Confirm(sample, nullptr) && cursor.queries == 1 && GetLastError() == 0x9101,
              "null expected cursor never suppresses the bitmap or issues a confirmation query");
    }
    {
        FakeCursor cursor(expected); unsigned credits = 0;
        cursor.counter = -3; cursor.flags[0] = 0;
        SetLastError(0x9102);
        const auto sample = cursor.Ensure(credits);
        Check(sample.queried && sample.adjusted && !sample.info.flags && cursor.queries == 1 &&
              cursor.counter == 0 && credits == 3 && cursor.shows == 3 && !cursor.hides && GetLastError() == 0x9102,
              "hidden cursor acquires bounded credits without replacing the original sample");
        SetLastError(0x9102);
        Check(cursor.Confirm(sample, expected) && cursor.queries == 2 && cursor.shows == 3 &&
              cursor.sizesCorrect && cursor.inputsClear && GetLastError() == 0x9102,
              "visibility recovery always confirms with a fresh correctly initialized query");
        cursor.Release(credits);
        Check(cursor.counter == -3 && !credits && cursor.shows == cursor.hides,
              "sample-based recovery returns exactly its three acquired visibility credits");
    }
    {
        FakeCursor cursor(expected); unsigned credits = 0;
        cursor.results[0] = FALSE; cursor.counter = -1;
        SetLastError(0x9103);
        const auto sample = cursor.Ensure(credits);
        Check(!sample.queried && !sample.adjusted && cursor.queries == 1 && !cursor.shows && !cursor.hides && !credits &&
              GetLastError() == 0x9103, "failed first query never trusts its output or adjusts visibility");
        SetLastError(0x9103);
        Check(cursor.Confirm(sample, expected) && cursor.queries == 2 && cursor.counter == -1 &&
              cursor.sizesCorrect && cursor.inputsClear && GetLastError() == 0x9103,
              "a failed first query can recover through successful fresh arrow confirmation");
    }
    {
        FakeCursor cursor(expected); unsigned credits = 0;
        cursor.flags[0] = 0; cursor.results[1] = FALSE; cursor.counter = -1;
        POINT bitmap[] = {{100, 200}, {120, 220}};
        const POINT original[] = {bitmap[0], bitmap[1]};
        SetLastError(0x9104);
        const auto sample = cursor.Ensure(credits);
        const bool confirmed = cursor.Confirm(sample, expected);
        if (confirmed) lakefix::HideSoftwareCursorRectangle(bitmap, 2);
        Check(!confirmed && cursor.queries == 2 && credits == 1 && cursor.counter == 0 &&
              std::memcmp(bitmap, original, sizeof(bitmap)) == 0 && GetLastError() == 0x9104,
              "failed confirmation keeps the bitmap even when dirty output claims the expected visible arrow");
        cursor.Release(credits);
        Check(!credits && cursor.counter == -1 && cursor.shows == cursor.hides,
              "failed confirmation still balances visibility credits during cleanup");
    }
    // CURSOR_SUPPRESSED is 2, not the hidden state 0 (also test older SDKs).
    for (DWORD state : {0UL, 2UL}) {
        FakeCursor cursor(expected); unsigned credits = 0;
        cursor.flags[0] = cursor.flags[1] = state;
        cursor.counter = state ? 0 : -1;
        POINT bitmap[] = {{1, 2}, {21, 22}};
        const POINT original[] = {bitmap[0], bitmap[1]};
        SetLastError(0x9105);
        const auto sample = cursor.Ensure(credits);
        const bool confirmed = cursor.Confirm(sample, expected);
        if (confirmed) lakefix::HideSoftwareCursorRectangle(bitmap, 2);
        Check(sample.queried && sample.adjusted && !confirmed && cursor.queries == 2 &&
              std::memcmp(bitmap, original, sizeof(bitmap)) == 0 && cursor.sizesCorrect && cursor.inputsClear &&
              GetLastError() == 0x9105, "both still-hidden and CURSOR_SUPPRESSED confirmations retain the bitmap fallback");
        if (state) Check(!credits && cursor.counter == 0 && cursor.shows == 1 && cursor.hides == 1,
                         "suppression with a nonnegative counter uses a balanced probe, not a retained visibility increment");
        cursor.Release(credits);
        Check(!credits && cursor.counter == (state ? 0 : -1) && cursor.shows == cursor.hides,
              "hidden and suppressed fallback paths restore their own counter changes only");
    }
    {
        FakeCursor cursor(expected); unsigned credits = 0;
        cursor.handles[0] = unexpected;
        SetLastError(0x9106);
        const auto sample = cursor.Ensure(credits);
        Check(sample.queried && !sample.adjusted && cursor.Confirm(sample, expected) && cursor.queries == 2 &&
              !cursor.shows && !cursor.hides && !credits && GetLastError() == 0x9106,
              "an unexpected visible handle needs fresh confirmation before the bitmap may be hidden");
    }
    {
        FakeCursor cursor(expected); unsigned credits = 0;
        cursor.handles[0] = cursor.handles[1] = unexpected;
        SetLastError(0x9107);
        const auto sample = cursor.Ensure(credits);
        Check(!cursor.Confirm(sample, expected) && cursor.queries == 2 && !cursor.shows && !cursor.hides &&
              !credits && GetLastError() == 0x9107, "a persistently unexpected visible handle cannot suppress the native bitmap");
    }
    {
        FakeCursor cursor(expected); unsigned credits = 16;
        cursor.counter = -84; cursor.flags[0] = cursor.flags[1] = 0;
        SetLastError(0x9108);
        const auto sample = cursor.Ensure(credits);
        Check(sample.queried && sample.adjusted && !cursor.Confirm(sample, expected) && cursor.queries == 2 &&
              credits == 16 && cursor.counter == -84 && !cursor.shows && !cursor.hides && GetLastError() == 0x9108,
              "reaching the 16-credit cap still requires fresh confirmation but never calls ShowCursor again");
        cursor.Release(credits);
        Check(!credits && cursor.counter == -100 && !cursor.shows && cursor.hides == 16,
              "capped confirmation leaves all existing credits available for exact cleanup");
    }
    {
        FakeCursor cursor(expected); unsigned credits = 0;
        SetLastError(0x9109);
        (void)cursor.Ensure(credits); // Ordinary WM_SETCURSOR ignores the sample.
        Check(cursor.queries == 1 && !cursor.shows && !cursor.hides && !credits && GetLastError() == 0x9109,
              "ordinary visible WM_SETCURSOR handling adds no redundant confirmation query");
        cursor.flags[1] = 0; cursor.counter = -2;
        SetLastError(0x9109);
        (void)cursor.Ensure(credits);
        Check(cursor.queries == 2 && cursor.counter == 0 && credits == 2 && cursor.shows == 2 &&
              GetLastError() == 0x9109, "ordinary hidden WM_SETCURSOR recovers visibility with only its single new query");
        cursor.Release(credits);
        Check(!credits && cursor.counter == -2 && cursor.shows == cursor.hides,
              "ordinary cursor handling retains balanced visibility cleanup");
    }
    {
        FakeCursor cursor(expected); unsigned credits = 0;
        SetLastError(0x9110);
        const auto first = cursor.Ensure(credits);
        Check(cursor.Confirm(first, expected) && cursor.queries == 1,
              "first event obtains its own visible cursor sample");
        cursor.flags[1] = 0; cursor.counter = -1;
        SetLastError(0x9110);
        const auto second = cursor.Ensure(credits);
        cursor.flags[1] = CURSOR_SHOWING;
        Check(second.adjusted && cursor.Confirm(second, expected) && cursor.queries == 3 && credits == 1,
              "next event observes newly hidden visibility rather than caching the previous event's result");
        cursor.handles[1] = unexpected;
        SetLastError(0x9110);
        const auto third = cursor.Ensure(credits);
        Check(!third.adjusted && !cursor.Confirm(third, expected) && cursor.queries == 5 &&
              cursor.sizesCorrect && cursor.inputsClear && GetLastError() == 0x9110,
              "later event observes a replaced cursor and cannot reuse a prior successful confirmation");
        cursor.Release(credits);
        Check(!credits && cursor.counter == -1 && cursor.shows == cursor.hides,
              "multiple independent event samples do not accumulate or lose visibility credits");
    }
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
    TestCursorVisibilitySamples();
    TestCursorVisibilityPolicy();
    TestVirtualCursorVisibility();
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
