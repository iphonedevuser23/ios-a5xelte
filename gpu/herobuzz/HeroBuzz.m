// hero2lte HeroBuzz: iOS's haptics on the S7's vibration motor.
// iOS would play its haptics on the audio plug-in's "Actuator", but mediaserverd never starts that I/O here
// (upstream notes, 2026-11-14). So the public feedback generators and AudioServices' vibrate sounds are caught in
// every UIKit process and turned into a Darwin notification; SpringBoard (not sandboxed) writes the buzz to
// /dev/hakios-haptic (the kernel runs the motor ~30 ms after each loud write).
#import <UIKit/UIKit.h>
#import <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <notify.h>
#include <objc/runtime.h>
#include <unistd.h>
#include <stdarg.h>

#define NOTE "com.herolte.buzz."
static BOOL inSpringBoard;
static FILE *buzzlog;
static void blog(const char *fmt, ...) {
    if (!buzzlog) return;
    va_list a; va_start(a, fmt); vfprintf(buzzlog, fmt, a); va_end(a); fputc('\n', buzzlog); fflush(buzzlog);
}
static __thread int in_public;   /* a public generator call is under way: its private play is not buzzed again */

static void buzz_now(int ms, int16_t level, int pulses) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        static int fd = -2;
        if (fd == -2) fd = open("/dev/hakios-haptic", O_WRONLY);
        if (fd < 0) return;
        int16_t samples[128];
        for (int i = 0; i < 128; i++) samples[i] = (i & 1) ? level : -level;
        for (int p = 0; p < pulses; p++) {
            for (int t = 0; t < ms; t += 20) {
                (void)write(fd, samples, sizeof samples);
                usleep(t + 20 < ms ? 20000 : 1000);
            }
            if (p + 1 < pulses) usleep(90000);
        }
    });
}

static void play(const char *kind) {
    if (inSpringBoard) blog("play %s", kind);
    if (!inSpringBoard) {
        char name[64];
        snprintf(name, sizeof name, NOTE "%s", kind);
        notify_post(name);
        return;
    }
    if (!strcmp(kind, "select")) buzz_now(20, 9000, 1);
    else if (!strcmp(kind, "light")) buzz_now(25, 12000, 1);
    else if (!strcmp(kind, "medium")) buzz_now(35, 20000, 1);
    else if (!strcmp(kind, "heavy")) buzz_now(45, 30000, 1);
    else if (!strcmp(kind, "success")) buzz_now(30, 20000, 2);
    else if (!strcmp(kind, "warning")) buzz_now(35, 24000, 2);
    else if (!strcmp(kind, "error")) buzz_now(35, 28000, 3);
    else if (!strcmp(kind, "vibrate")) buzz_now(400, 32000, 1);
}

static IMP orig_impact, orig_impact_i, orig_select, orig_note;
static void my_impact(UIImpactFeedbackGenerator *self, SEL _cmd) {
    NSInteger style = UIImpactFeedbackStyleMedium;
    Ivar iv = class_getInstanceVariable([UIImpactFeedbackGenerator class], "_style");
    if (iv) {
        ptrdiff_t off = ivar_getOffset(iv);
        style = *(NSInteger *)((char *)(__bridge void *)self + off);
    }
    play(style == UIImpactFeedbackStyleHeavy ? "heavy" : style == UIImpactFeedbackStyleMedium ? "medium" : "light");
    in_public++; ((void (*)(id, SEL))orig_impact)(self, _cmd); in_public--;
}
static void my_impact_i(UIImpactFeedbackGenerator *self, SEL _cmd, CGFloat intensity) {
    play(intensity > 0.7 ? "heavy" : intensity > 0.35 ? "medium" : "light");
    in_public++; ((void (*)(id, SEL, CGFloat))orig_impact_i)(self, _cmd, intensity); in_public--;
}
static void my_select(id self, SEL _cmd) {
    play("select");
    in_public++; ((void (*)(id, SEL))orig_select)(self, _cmd); in_public--;
}
static void my_note(id self, SEL _cmd, UINotificationFeedbackType type) {
    play(type == UINotificationFeedbackTypeSuccess ? "success" : type == UINotificationFeedbackTypeWarning ? "warning" : "error");
    in_public++; ((void (*)(id, SEL, UINotificationFeedbackType))orig_note)(self, _cmd, type); in_public--;
}

static void (*orig_play)(SystemSoundID);
static void my_play(SystemSoundID s) {
    if (s == kSystemSoundID_Vibrate) play("vibrate");
    else if (s == 1519) play("light");
    else if (s == 1520) play("medium");
    else if (s == 1521) play("error");
    orig_play(s);
}
static void (*orig_alert)(SystemSoundID);
static void my_alert(SystemSoundID s) {
    play("vibrate");
    orig_alert(s);
}
static void (*orig_play_c)(SystemSoundID, void (^)(void));
static void my_play_c(SystemSoundID s, void (^c)(void)) {
    if (s == kSystemSoundID_Vibrate) play("vibrate");
    orig_play_c(s, c);
}


/* UIKit's own controls (context menus, switches, pickers...) use private generators that never call the public
 * methods: every UIFeedbackGenerator method named *playFeedback* with (id) or (id, CGPoint) arguments buzzes too. */
static NSMutableDictionary *orig_priv;
static IMP priv_orig(id self, SEL s) {
    return (IMP)[orig_priv[NSStringFromSelector(s)] pointerValue];
}
static void priv_a(id self, SEL s, id fb) {
    if (!in_public) play("light");
    ((void (*)(id, SEL, id))priv_orig(self, s))(self, s, fb);
}
static void priv_b(id self, SEL s, id fb, CGPoint p) {
    if (!in_public) play("light");
    ((void (*)(id, SEL, id, CGPoint))priv_orig(self, s))(self, s, fb, p);
}
static void hook_private(Class c) {
    unsigned n = 0;
    Method *ms = class_copyMethodList(c, &n);
    for (unsigned i = 0; i < n; i++) {
        const char *name = sel_getName(method_getName(ms[i]));
        const char *types = method_getTypeEncoding(ms[i]);
        if (inSpringBoard && (strcasestr(name, "play") || strcasestr(name, "feedback"))) blog("method %s %s %s", class_getName(c), name, types);
        if (!strstr(name, "playFeedback") || orig_priv[@(name)]) continue;
        unsigned args = method_getNumberOfArguments(ms[i]);
        char a2[64] = "", a3[64] = "", rt[16] = "";
        method_getReturnType(ms[i], rt, sizeof rt);
        if (rt[0] != 'v') continue;
        if (args >= 3) method_getArgumentType(ms[i], 2, a2, sizeof a2);
        if (args >= 4) method_getArgumentType(ms[i], 3, a3, sizeof a3);
        IMP n2 = NULL;
        if (args == 3 && a2[0] == '@') n2 = (IMP)priv_a;
        else if (args == 4 && a2[0] == '@' && strncmp(a3, "{CGPoint", 8) == 0) n2 = (IMP)priv_b;
        if (!n2) continue;
        orig_priv[@(name)] = [NSValue valueWithPointer:(void *)method_setImplementation(ms[i], n2)];
        if (inSpringBoard) blog("hooked %s %s", class_getName(c), name);
    }
    free(ms);
}

static void swap(Class c, SEL s, IMP n, IMP *o) {
    Method m = class_getInstanceMethod(c, s);
    if (m) *o = method_setImplementation(m, n);
}

__attribute__((constructor)) static void init(void) {
    inSpringBoard = [[[NSBundle mainBundle] bundleIdentifier] isEqualToString:@"com.apple.springboard"];
    if (inSpringBoard) buzzlog = fopen("/var/mobile/Library/Caches/herobuzz.txt", "w");
    orig_priv = [NSMutableDictionary new];
    hook_private([UIFeedbackGenerator class]);
    Class eng = NSClassFromString(@"_UIFeedbackEngine");
    if (eng) hook_private(eng);
    swap([UIImpactFeedbackGenerator class], @selector(impactOccurred), (IMP)my_impact, &orig_impact);
    swap([UIImpactFeedbackGenerator class], @selector(impactOccurredWithIntensity:), (IMP)my_impact_i, &orig_impact_i);
    swap([UISelectionFeedbackGenerator class], @selector(selectionChanged), (IMP)my_select, &orig_select);
    swap([UINotificationFeedbackGenerator class], @selector(notificationOccurred:), (IMP)my_note, &orig_note);
    void (*hook)(void *, void *, void **) = (void (*)(void *, void *, void **))dlsym(RTLD_DEFAULT, "MSHookFunction");
    if (hook) {
        hook((void *)AudioServicesPlaySystemSound, (void *)my_play, (void **)&orig_play);
        hook((void *)AudioServicesPlayAlertSound, (void *)my_alert, (void **)&orig_alert);
        hook((void *)AudioServicesPlaySystemSoundWithCompletion, (void *)my_play_c, (void **)&orig_play_c);
    }
    if (inSpringBoard) {
        static const char *kinds[] = {"select", "light", "medium", "heavy", "success", "warning", "error", "vibrate"};
        for (unsigned i = 0; i < sizeof kinds / sizeof *kinds; i++) {
            char name[64];
            snprintf(name, sizeof name, NOTE "%s", kinds[i]);
            const char *k = kinds[i];
            int token;
            notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) { blog("from an app: %s", k); play(k); });
        }
        blog("HeroBuzz v2: SpringBoard listening, MSHookFunction %s, /dev/hakios-haptic %s", hook ? "found" : "missing", access("/dev/hakios-haptic", W_OK) == 0 ? "writable" : "NOT writable");
    }
}
