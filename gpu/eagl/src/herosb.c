/*
 * libherosb.dylib, in SpringBoard (boot-arg herosb=1, bsd/dev/herolte_gl.c): PosterBoard has no poster providers on this
 * root, so the wallpaper windows stay black. The picture of the drop zone goes into them, and behind the home screen.
 * DROP_DIR wallpaper.off turns it off.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DROP_DIR "/private/var/herolte/"
#define VIEW_TAG 0x48455230

typedef struct { double x, y, w, h; } rect_t;

/* Local stub: linking libdrm for herodrm_log added a @rpath/libdrm.2.dylib dependency with no LC_RPATH,
 * so libherosb never loaded. SpringBoard gets no HERO_LOG_STDERR pump anyway, so stderr is fine here. */
static void
herodrm_log(const char *format, ...)
{
   va_list arguments;
   va_start(arguments, format);
   vfprintf(stderr, format, arguments);
   va_end(arguments);
   fputc('\n', stderr);
}

static id
nsstring(const char *text)
{
   return ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
      sel_registerName("stringWithUTF8String:"), text);
}

static id
send0(id target, const char *selector)
{
   return ((id (*)(id, SEL))objc_msgSend)(target, sel_registerName(selector));
}

static id picture;
static bool gestures_on;
static unsigned rounds;

static void
give_picture(id window, bool behind_all)
{
   if (((id (*)(id, SEL, long))objc_msgSend)(window, sel_registerName("viewWithTag:"), VIEW_TAG))
      return;
   id view = ((id (*)(id, SEL, id))objc_msgSend)(send0((id)objc_getClass("UIImageView"), "alloc"),
      sel_registerName("initWithImage:"), picture);
   rect_t bounds = ((rect_t (*)(id, SEL))objc_msgSend)(window, sel_registerName("bounds"));
   ((void (*)(id, SEL, rect_t))objc_msgSend)(view, sel_registerName("setFrame:"), bounds);
   ((void (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("setContentMode:"), 2);          /* aspect fill */
   ((void (*)(id, SEL, unsigned long))objc_msgSend)(view, sel_registerName("setAutoresizingMask:"), 18);
   ((void (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("setTag:"), VIEW_TAG);
   ((void (*)(id, SEL, BOOL))objc_msgSend)(view, sel_registerName("setUserInteractionEnabled:"), NO);
   if (behind_all)
      ((void (*)(id, SEL, id, long))objc_msgSend)(window, sel_registerName("insertSubview:atIndex:"), view, 0);
   else
      ((void (*)(id, SEL, id))objc_msgSend)(window, sel_registerName("addSubview:"), view);
   herodrm_log("herosb: picture in %s, %.0f x %.0f", object_getClassName(window), bounds.w, bounds.h);
}

static void
look_at_windows(void *unused)
{
   rounds++;
   Class windows_class = objc_getClass("UIWindow");
   SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
   id windows = windows_class && class_respondsToSelector(object_getClass((id)windows_class), all) ?
      ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)windows_class, all, YES, NO) : nil;
   if (!picture && objc_getClass("UIImage")) {
      picture = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("UIImage"), sel_registerName("imageWithContentsOfFile:"),
         nsstring(DROP_DIR "wallpaper.png"));
      if (picture)
         send0(picture, "retain");
   }
   size_t count = windows ? ((size_t (*)(id, SEL))objc_msgSend)(windows, sel_registerName("count")) : 0;
   if (rounds == 1 || rounds == 6)
      herodrm_log("herosb: round %u, %zu windows, picture %d, gestures %d", rounds, count, picture != nil, gestures_on);
   for (size_t i = 0; picture && i < count; i++) {
      id window = ((id (*)(id, SEL, size_t))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);
      const char *name = object_getClassName(window);
      if (rounds == 6)
         herodrm_log("herosb: window %s", name);
      if (strstr(name, "Wallpaper"))
         give_picture(window, false);
      else if (strcmp(name, "SBHomeScreenWindow") == 0)
         give_picture(window, true);
   }
   if (rounds < 60)
      dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, look_at_windows);
}

/*
 * The ADT says "home button" (home-button-type 1), which is the iPhone 7/8 layout the hero2's physical home button
 * matches, and the default here. Telling SpringBoard type 2 switches it to the iPhone-X gesture layout (no home
 * button, swipe-up home); DROP_DIR gestures.on opts into that. The status bar provider is left stock: forcing
 * _UIStatusBarVisualProvider_Phone made Control Center call regionIdentifiersForPartWithIdentifier: on it and throw
 * NSInvalidArgumentException, crashing SpringBoard whenever Control Center opened.
 */
static long
says_gestures(id self, SEL cmd)
{
   return 2;
}

static BOOL
says_yes(id self, SEL cmd)
{
   return YES;
}

static void
ask_for_gestures(void)
{
   Class platform = objc_getClass("BSPlatform");
   Method type = platform ? class_getInstanceMethod(platform, sel_registerName("homeButtonType")) : NULL;
   if (type) {
      method_setImplementation(type, (IMP)says_gestures);
      gestures_on = true;
   }
   Class settings = objc_getClass("SBHomeGestureSettings");
   Method enabled = settings ? class_getInstanceMethod(settings, sel_registerName("isHomeGestureEnabled")) : NULL;
   if (enabled)
      method_setImplementation(enabled, (IMP)says_yes);
}

/*
 * SpringBoard's own frame rate caps (from Scion's inject_sb.c, Galaxy S9). Some of its animations ask for fewer frames
 * than the panel has: the crossfade of the page dots and the search pill after every page's slide is
 * CAFrameRateRangeMake(20, 30, 0), and CoreAnimation gives SpringBoard 30 a second for as long as it runs -- on the S9
 * half of all the refreshes missed while swiping. A range whose most is under 60 gets 60 as its most and preferred.
 * DROP_DIR fps.off leaves them as Apple set them.
 */
typedef struct { float minimum, maximum, preferred; } frame_rate_range_t;
static void (*fluid_set_range_was)(id, SEL, frame_rate_range_t, unsigned);
static void (*animation_set_range_was)(id, SEL, frame_rate_range_t, unsigned);
static unsigned ranges_raised;

static frame_rate_range_t
raise_range(frame_rate_range_t range)
{
   if (range.maximum > 0.0f && range.maximum < 60.0f) {
      range.maximum = 60.0f;
      range.preferred = 60.0f;
      ranges_raised++;
   }
   return range;
}

static void
fluid_set_range(id self, SEL cmd, frame_rate_range_t range, unsigned reason)
{
   fluid_set_range_was(self, cmd, raise_range(range), reason);
}

static void
animation_set_range(id self, SEL cmd, frame_rate_range_t range, unsigned reason)
{
   animation_set_range_was(self, cmd, raise_range(range), reason);
}

static void
raise_frame_rates(void)
{
   SEL set = sel_registerName("setFrameRateRange:highFrameRateReason:");
   Class fluid = objc_getClass("SBFFluidBehaviorSettings"), animation = objc_getClass("SBFAnimationSettings");
   Method m = fluid && !fluid_set_range_was ? class_getInstanceMethod(fluid, set) : NULL;
   if (m)
      fluid_set_range_was = (void (*)(id, SEL, frame_rate_range_t, unsigned))method_setImplementation(m, (IMP)fluid_set_range);
   m = animation && !animation_set_range_was ? class_getInstanceMethod(animation, set) : NULL;
   if (m)
      animation_set_range_was = (void (*)(id, SEL, frame_rate_range_t, unsigned))method_setImplementation(m, (IMP)animation_set_range);
   herodrm_log("herosb: frame rate caps raised to 60: fluid settings %s, animation settings %s",
      fluid_set_range_was ? "yes" : "NO", animation_set_range_was ? "yes" : "NO");
}

static void
report_ranges(void *unused)
{
   herodrm_log("herosb: %u frame rate ranges raised to 60 so far", ranges_raised);
}

__attribute__((constructor)) static void
herosb_start(void)
{
   /* Bootstrap ElleKit's tweak loader in SpringBoard so installed tweaks (Cylinder, etc.) load.
    * Normally ElleKit's pspawn hook does this; this port has no pspawn, so we load it directly.
    * DROP_DIR tweaks.off disables it if a tweak destabilizes SpringBoard. */
   if (access(DROP_DIR "tweaks.off", F_OK) != 0)
      dlopen("/var/jb/usr/lib/TweakLoader.dylib", RTLD_NOW);
   if (access(DROP_DIR "fps.off", F_OK) != 0) {
      raise_frame_rates();
      dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, report_ranges);
   }
   if (access(DROP_DIR "gestures.on", F_OK) == 0)   /* opt in to the iPhone-X gesture layout; default is 7/8 */
      ask_for_gestures();
   if (access(DROP_DIR "wallpaper.off", F_OK) == 0)
      return;
   dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, look_at_windows);
}
