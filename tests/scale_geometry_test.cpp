#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/scale_geometry.h"
#include <cstdio>
#include <limits>

// Standalone console tests: Windows types only, no windows, cursor movement,
// DirectDraw, GPU, injection or proprietary application code.
namespace {
int checks;
int failures;

void Check(bool condition, const char* name) {
    ++checks;
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

bool SameRect(const RECT& actual, const RECT& expected) {
    return actual.left == expected.left && actual.top == expected.top &&
           actual.right == expected.right && actual.bottom == expected.bottom;
}

bool SamePoint(POINT actual, POINT expected) {
    return actual.x == expected.x && actual.y == expected.y;
}

void CheckRect(RECT actual, RECT expected, const char* name) {
    Check(SameRect(actual, expected), name);
    if (!SameRect(actual, expected))
        printf("  got {%ld,%ld,%ld,%ld}, expected {%ld,%ld,%ld,%ld}\n",
               actual.left, actual.top, actual.right, actual.bottom,
               expected.left, expected.top, expected.right, expected.bottom);
}

void CheckPoint(POINT actual, POINT expected, const char* name) {
    Check(SamePoint(actual, expected), name);
    if (!SamePoint(actual, expected))
        printf("  got (%ld,%ld), expected (%ld,%ld)\n", actual.x, actual.y, expected.x, expected.y);
}

void TestFits() {
    using lakefix::FitViewport;
    CheckRect(FitViewport(1024, 576, 1536, 864), {0, 0, 1024, 576}, "1536x864 in 1024x576");
    CheckRect(FitViewport(1024, 576, 1088, 612), {0, 0, 1024, 576}, "reduced 1088x612 canvas preserves 16:9 viewport");
    CheckPoint(lakefix::ViewToSource({0, 0, 1024, 576}, {1088, 612}, {512, 288}), {544, 306},
               "reduced canvas center maps without old-canvas constants");
    CheckRect(FitViewport(640, 480, 1536, 864), {0, 60, 640, 420}, "1536x864 in 640x480 letterboxes");
    CheckRect(FitViewport(800, 600, 1536, 864), {0, 75, 800, 525}, "1536x864 in 800x600 letterboxes");
    CheckRect(FitViewport(400, 225, 1536, 864), {0, 0, 400, 225}, "1536x864 in 400x225");
    CheckRect(FitViewport(1, 1, 1536, 864), {0, 0, 1, 1}, "1x1 retains one pixel on both axes");
    CheckRect(FitViewport(641, 481, 1536, 864), {0, 60, 641, 420}, "odd dimensions floor size and center bars");
    CheckRect(FitViewport(400, 300, 1536, 864), {0, 37, 400, 262}, "400x300 has asymmetric one-pixel remainder");
    CheckRect(FitViewport(480, 640, 1536, 864), {0, 185, 480, 455}, "landscape source in portrait viewport");
    CheckRect(FitViewport(640, 480, 864, 1536), {185, 0, 455, 480}, "rotated source pillarboxes");
    CheckRect(FitViewport(576, 1024, 864, 1536), {0, 0, 576, 1024}, "rotated matching aspect ratio");
    CheckRect(FitViewport(2000, 1000, 100, 100), {500, 0, 1500, 1000}, "square source enlarges and pillarboxes");
    CheckRect(FitViewport(1, 1, 1, 1), {0, 0, 1, 1}, "unit source and viewport");

    const LONG low = (std::numeric_limits<LONG>::min)();
    const SIZE invalid[] = {{0, 1}, {1, 0}, {-1, 1}, {1, -1}, {0, 0}, {low, low}};
    for (const SIZE size : invalid) {
        CheckRect(FitViewport(size.cx, size.cy, 1536, 864), {}, "invalid viewport returns an empty rectangle");
        CheckRect(FitViewport(1024, 576, size.cx, size.cy), {}, "invalid source returns an empty rectangle");
    }
}

void TestHitTestingAndMapping() {
    using namespace lakefix;
    const SIZE source{1536, 864};
    const RECT viewport = FitViewport(640, 480, source.cx, source.cy);
    Check(ViewportContains(viewport, {0, 60}), "left/top edges are inclusive");
    Check(ViewportContains(viewport, {639, 419}), "last destination pixel is inside");
    Check(!ViewportContains(viewport, {640, 240}), "right edge is exclusive");
    Check(!ViewportContains(viewport, {320, 420}), "bottom edge is exclusive");
    Check(!ViewportContains(viewport, {-1, 240}), "negative x is outside");
    Check(!ViewportContains(viewport, {320, 59}), "top letterbox bar is outside");
    Check(!ViewportContains(viewport, {320, 479}), "bottom letterbox bar is outside");
    Check(!ViewportContains({}, {0, 0}), "empty viewport contains no pixels");
    Check(!ViewportContains({5, 5, 4, 4}, {4, 4}), "inverted viewport contains no pixels");

    CheckPoint(ViewToSource(viewport, source, {0, 60}), {0, 0}, "map top-left corner");
    CheckPoint(ViewToSource(viewport, source, {639, 60}), {1533, 0}, "map top-right using floor");
    CheckPoint(ViewToSource(viewport, source, {0, 419}), {0, 861}, "map bottom-left using floor");
    CheckPoint(ViewToSource(viewport, source, {639, 419}), {1533, 861}, "map bottom-right using floor");
    CheckPoint(ViewToSource(viewport, source, {320, 240}), {768, 432}, "map center");
    const POINT outside[] = {{640, 240}, {320, 420}, {-1, 240}, {320, 59}, {320, 479}};
    for (const POINT point : outside)
        CheckPoint(ViewToSource(viewport, source, point), {-1, -1}, "outside maps to signed (-1,-1)");

    CheckPoint(SourceToView(viewport, source, {0, 0}), {0, 60}, "reverse map top-left");
    CheckPoint(SourceToView(viewport, source, {1535, 863}), {639, 419}, "reverse map last source pixel");
    CheckPoint(SourceToView(viewport, source, {768, 432}), {320, 240}, "reverse map center");
    CheckPoint(ViewToSource({0, 0, 1, 1}, source, {0, 0}), {0, 0}, "unit viewport uses floor, not rounded centers");
    CheckPoint(SourceToView({0, 0, 1, 1}, source, {1535, 863}), {0, 0}, "all valid source pixels can collapse to one pixel");
    CheckPoint(ViewToSource({0, 0, 2, 2}, {1, 1}, {1, 1}), {0, 0}, "unit source enlarges safely");

    const SIZE invalidSizes[] = {{0, 864}, {1536, 0}, {-1, 864}, {1536, -1}};
    for (const SIZE size : invalidSizes) {
        CheckPoint(ViewToSource(viewport, size, {320, 240}), {-1, -1}, "invalid source cannot divide in view mapping");
        CheckPoint(SourceToView(viewport, size, {0, 0}), {-1, -1}, "invalid source cannot divide in reverse mapping");
    }
    const RECT invalidRects[] = {{}, {1, 1, 1, 2}, {1, 1, 2, 1}, {2, 2, 1, 1}};
    for (const RECT rectangle : invalidRects) {
        CheckPoint(ViewToSource(rectangle, source, {1, 1}), {-1, -1}, "invalid viewport cannot divide in view mapping");
        CheckPoint(SourceToView(rectangle, source, {0, 0}), {-1, -1}, "invalid viewport cannot divide in reverse mapping");
    }
    const POINT invalidSource[] = {{-1, 0}, {0, -1}, {1536, 0}, {0, 864}};
    for (const POINT point : invalidSource)
        CheckPoint(SourceToView(viewport, source, point), {-1, -1}, "source bounds are exclusive and reject negatives");

    const RECT translated{-40, -20, 60, 30};
    CheckPoint(ViewToSource(translated, {200, 100}, {-40, -20}), {0, 0}, "negative rectangle origins map safely");
    CheckPoint(SourceToView(translated, {200, 100}, {100, 50}), {10, 5}, "reverse mapping preserves translated origin");
}

void TestLargeCoordinates() {
    using namespace lakefix;
    const LONG high = (std::numeric_limits<LONG>::max)();
    const LONG low = (std::numeric_limits<LONG>::min)();
    CheckRect(FitViewport(high, high, high, high), {0, 0, high, high}, "LONG_MAX square fitting has no product overflow");
    CheckRect(FitViewport(high, high, 1536, 864), {0, 469762048, high, 1677721599}, "LONG_MAX landscape fitting floors safely");
    CheckRect(FitViewport(high, high, high, 1), {0, 1073741823, high, 1073741824}, "extremely wide source retains a one-pixel height");
    CheckRect(FitViewport(high, high, 1, high), {1073741823, 0, 1073741824, high}, "extremely tall source retains a one-pixel width");
    CheckRect(FitViewport(1, high, high, 1), {0, 1073741823, 1, 1073741824}, "extreme ratio cannot round fitted height to zero");
    CheckRect(FitViewport(high, 1, 1, high), {1073741823, 0, 1073741824, 1}, "extreme ratio cannot round fitted width to zero");

    const RECT spanning{low, low, high, high};
    Check(ViewportContains(spanning, {low, low}), "full LONG-span rectangle contains its inclusive edge");
    Check(!ViewportContains(spanning, {high, high}), "full LONG-span rectangle excludes its terminal edge");
    CheckPoint(ViewToSource(spanning, {high, high}, {low, low}), {0, 0}, "full-span mapping widens subtraction first");
    CheckPoint(ViewToSource(spanning, {high, high}, {high - 1, high - 1}), {high - 1, high - 1}, "full-span mapping product stays within int64");
    CheckPoint(ViewToSource(spanning, {high, high}, {0, 0}), {1073741823, 1073741823}, "full-span mapping crosses negative and positive coordinates");
    CheckPoint(SourceToView(spanning, {high, high}, {0, 0}), {low, low}, "full-span reverse mapping retains LONG_MIN");
    CheckPoint(SourceToView(spanning, {high, high}, {high - 1, high - 1}), {high - 3, high - 3}, "full-span reverse mapping cannot overflow edges");
    CheckPoint(SourceToView(spanning, {1, 1}, {0, 0}), {low, low}, "full-span unit source cannot divide by zero");
}

void TestRoundTrips() {
    struct Case {
        SIZE view;
        SIZE source;
        const char* name;
    };
    const Case cases[] = {
        {{1024, 576}, {1536, 864}, "matching landscape"},
        {{640, 480}, {1536, 864}, "letterboxed landscape"},
        {{800, 600}, {1536, 864}, "larger letterbox"},
        {{400, 225}, {1536, 864}, "small landscape"},
        {{1, 1}, {1536, 864}, "one-pixel viewport"},
        {{480, 640}, {1536, 864}, "portrait viewport"},
        {{640, 480}, {864, 1536}, "rotated source"},
        {{576, 1024}, {864, 1536}, "matching portrait"},
        {{203, 157}, {17, 31}, "non-integral enlargement"},
        {{157, 203}, {31, 17}, "rotated non-integral enlargement"},
        {{1, 31}, {31, 1}, "extreme small ratio"},
    };
    for (const Case& test : cases) {
        const RECT viewport = lakefix::FitViewport(test.view.cx, test.view.cy, test.source.cx, test.source.cy);
        const std::int64_t width = static_cast<std::int64_t>(viewport.right) - viewport.left;
        const std::int64_t height = static_cast<std::int64_t>(viewport.bottom) - viewport.top;
        bool sourceRoundTrip = width > 0 && height > 0;
        bool viewRoundTrip = sourceRoundTrip;
        // Sample a grid including all corners and the center. Floor mappings
        // can lose at most ceil(axis ratio), and never round a coordinate up.
        for (int row = 0; row <= 16 && sourceRoundTrip && viewRoundTrip; ++row) {
            for (int column = 0; column <= 16; ++column) {
                const POINT source{
                    static_cast<LONG>(static_cast<std::int64_t>(test.source.cx - 1) * column / 16),
                    static_cast<LONG>(static_cast<std::int64_t>(test.source.cy - 1) * row / 16)};
                const POINT view = lakefix::SourceToView(viewport, test.source, source);
                const POINT back = lakefix::ViewToSource(viewport, test.source, view);
                sourceRoundTrip = sourceRoundTrip && lakefix::ViewportContains(viewport, view) &&
                    back.x >= 0 && back.y >= 0 && back.x <= source.x && back.y <= source.y &&
                    source.x - back.x <= (test.source.cx + width - 1) / width &&
                    source.y - back.y <= (test.source.cy + height - 1) / height;

                const POINT original{
                    static_cast<LONG>(viewport.left + (width - 1) * column / 16),
                    static_cast<LONG>(viewport.top + (height - 1) * row / 16)};
                const POINT mapped = lakefix::ViewToSource(viewport, test.source, original);
                const POINT returned = lakefix::SourceToView(viewport, test.source, mapped);
                viewRoundTrip = viewRoundTrip && lakefix::ViewportContains(viewport, returned) &&
                    mapped.x >= 0 && mapped.x < test.source.cx && mapped.y >= 0 && mapped.y < test.source.cy &&
                    returned.x <= original.x && returned.y <= original.y &&
                    static_cast<std::int64_t>(original.x) - returned.x <= (width + test.source.cx - 1) / test.source.cx &&
                    static_cast<std::int64_t>(original.y) - returned.y <= (height + test.source.cy - 1) / test.source.cy;
            }
        }
        char name[160]{};
        std::snprintf(name, sizeof(name), "%s source/view/source bounded floor round trip", test.name);
        Check(sourceRoundTrip, name);
        std::snprintf(name, sizeof(name), "%s view/source/view bounded floor round trip", test.name);
        Check(viewRoundTrip, name);
    }
}
void TestCursorDeltas() {
    using lakefix::TranslateCursorDelta;
    const POINT real{101, 103}, before{151, 154};
    const SIZE view{1024, 576}, source{1536, 864};
    CheckPoint(TranslateCursorDelta(real, before, before, view, source), real,
               "zero native cursor offset never moves physical cursor");
    CheckPoint(TranslateCursorDelta({1000, 800}, {-1, -1}, {-1, -1}, view, source), {1000, 800},
               "outside sentinel round-trip preserves the actual cursor anchor");
    CheckPoint(TranslateCursorDelta({1000, 800}, {-1, -1}, {2, 2}, view, source), {1002, 802},
               "captured outside cursor offset scales from real anchor, not sentinel position");
    CheckPoint(TranslateCursorDelta(real, before, {154, 157}, view, source), {103, 105},
               "positive cursor offset scales relative to exact starting pixel");
    CheckPoint(TranslateCursorDelta(real, before, {148, 151}, view, source), {99, 101},
               "negative native cursor offset is symmetric");
    CheckPoint(TranslateCursorDelta(real, before, {152, 155}, view, source), real,
               "subpixel cursor offset does not introduce inverse-floor drift");
    CheckPoint(TranslateCursorDelta(real, before, {154, 157}, {3072, 1728}, source), {107, 109},
               "cursor delta supports enlarged viewports");
    CheckPoint(TranslateCursorDelta(real, before, {0, 0}, {0, 576}, source), real,
               "invalid cursor viewport leaves real position unchanged");
    CheckPoint(TranslateCursorDelta(real, before, {0, 0}, view, {1536, 0}), real,
               "invalid cursor canvas cannot divide by zero");
    const LONG low = (std::numeric_limits<LONG>::min)(), high = (std::numeric_limits<LONG>::max)();
    CheckPoint(TranslateCursorDelta({high, low}, {low, high}, {high, low}, {high, high}, {1, 1}),
               {high, low}, "extreme cursor deltas saturate without product overflow");
}

void TestCapturedMapping() {
    using lakefix::ViewToSourceCaptured;
    const RECT viewport{0, 75, 800, 525};
    const SIZE source{1088, 612};
    CheckPoint(ViewToSourceCaptured(viewport, source, {400, 74}), {544, -2}, "capture above keeps valid horizontal axis");
    CheckPoint(ViewToSourceCaptured(viewport, source, {-1, 300}), {-2, 306}, "capture left keeps valid vertical axis");
    CheckPoint(ViewToSourceCaptured(viewport, source, {800, 300}), {1088, 306}, "capture right extrapolates without clamp");
    CheckPoint(ViewToSourceCaptured(viewport, source, {400, 525}), {544, 612}, "capture below delivers continuous release coordinate");
    CheckPoint(ViewToSourceCaptured(viewport, source, {-7, -9}), {-10, -115}, "capture corner uses signed floor on both axes");
    CheckPoint(ViewToSourceCaptured(viewport, source, {400, 300}), {544, 306}, "capture inside agrees with ordinary map");
    CheckPoint(ViewToSourceCaptured({}, source, {400, 300}), {-1, -1}, "invalid capture geometry cannot divide by zero");
    const LONG low = (std::numeric_limits<LONG>::min)(), high = (std::numeric_limits<LONG>::max)();
    CheckPoint(ViewToSourceCaptured({0, 0, 1, 1}, {high, high}, {low, high}), {low, high}, "capture extrapolation saturates only at LONG bounds");
    // Demonstrates truncation, not a claim that Lake requires accumulating it.
    POINT repeated{100, 100};
    for (int i = 0; i < 17; ++i)
        repeated = lakefix::TranslateCursorDelta(repeated, {0, 0}, {1, 1}, {1024, 576}, source);
    CheckPoint(repeated, {100, 100}, "small-delta truncation is reproduced with default canvas ratio");
    CheckPoint(lakefix::TranslateCursorDelta({100, 100}, {0, 0}, {17, 17}, {1024, 576}, source), {116, 116},
               "aggregate delta differs; accumulation remains disabled pending callsite evidence");
}
} // namespace

static_assert(sizeof(LONG) == 4, "These tests exercise Win32 LONG bounds");
static_assert(noexcept(lakefix::FitViewport(1, 1, 1, 1)), "FitViewport must be noexcept");
static_assert(noexcept(lakefix::ViewportContains(RECT{}, POINT{})), "ViewportContains must be noexcept");
static_assert(noexcept(lakefix::ViewToSource(RECT{}, SIZE{}, POINT{})), "ViewToSource must be noexcept");
static_assert(noexcept(lakefix::SourceToView(RECT{}, SIZE{}, POINT{})), "SourceToView must be noexcept");

int wmain() {
    TestFits();
    TestHitTestingAndMapping();
    TestLargeCoordinates();
    TestRoundTrips();
    TestCursorDeltas();
    TestCapturedMapping();
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
