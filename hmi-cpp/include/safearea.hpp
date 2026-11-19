// Safe area of the HMI's window: the margins (drawable pixels) that the system covers, such as an iPhone's
// notch / Dynamic Island and home indicator. main.cpp's Output draws the Canvas inside them and fills them black.
//
// Only iOS has such margins: hmi-ios/src/platform.mm defines ksphmi_platform_safe_insets() from the window's
// UIKit safeAreaInsets, read at call time (they move with every rotation). Every other platform gets the inline
// default below, which returns zeros, so their output, link and behaviour do not change.
#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ksphmi {

/// Insets of the safe area from each edge of the window's drawable, in drawable pixels (0 = not covered).
struct SafeInsets {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool any() const { return left || top || right || bottom; }
    bool operator==(const SafeInsets& o) const {
        return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
    }
    bool operator!=(const SafeInsets& o) const { return !(*this == o); }
};

}  // namespace ksphmi

#if defined(__APPLE__) && TARGET_OS_IOS
/// hmi-ios/src/platform.mm: the SDL window's safeAreaInsets now, converted to drawable pixels (rounded up).
/// Main thread only (SDL_main's thread on iOS); zeros when there is no window yet.
extern "C" void ksphmi_platform_safe_insets(int* left, int* top, int* right, int* bottom);
#endif

namespace ksphmi {

/// The current safe-area insets of the HMI window (zeros on every platform but iOS).
inline SafeInsets safe_insets() {
    SafeInsets s;
#if defined(__APPLE__) && TARGET_OS_IOS
    ksphmi_platform_safe_insets(&s.left, &s.top, &s.right, &s.bottom);
#endif
    return s;
}

}  // namespace ksphmi
