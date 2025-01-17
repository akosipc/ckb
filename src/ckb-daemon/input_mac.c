#include "device.h"
#include "input.h"

#ifdef OS_MAC

// Numpad keys have an extra flag
#define IS_NUMPAD(scancode) ((scancode) >= kVK_ANSI_KeypadDecimal && (scancode) <= kVK_ANSI_Keypad9 && (scancode) != kVK_ANSI_KeypadClear && (scancode) != kVK_ANSI_KeypadEnter)

pthread_mutex_t _euid_guard = PTHREAD_MUTEX_INITIALIZER;

// Event helpers
static void postevent(io_connect_t event, UInt32 type, NXEventData* ev, IOOptionBits flags, IOOptionBits options, int silence_errors){
    // Hack #1: IOHIDPostEvent will fail with kIOReturnNotPrivileged if the event doesn't originate from the UID that owns /dev/console
    // You'd think being root would be good enough. You'd be wrong. ckb-daemon needs to run as root for other reasons though
    // (namely, being able to seize the physical IOHIDDevices) so what we do instead is change our EUID to the appropriate owner,
    // post the event, and then change it right back.
    // Yeah...
    uid_t uid = 0;
    gid_t gid = 0;
    struct stat file;
    if(!stat("/dev/console", &file)){
        uid = file.st_uid;
        gid = file.st_gid;
    }
    euid_guard_start;
    if(uid != 0)
        seteuid(uid);
    if(gid != 0)
        setegid(gid);

    IOGPoint location = {0, 0};
    if((options & kIOHIDSetRelativeCursorPosition) && type != NX_MOUSEMOVED){
        // Hack #2: IOHIDPostEvent will not accept relative mouse coordinates for any event other than NX_MOUSEMOVED
        // So we need to get the current absolute coordinates from CoreGraphics and then modify those...
        CGEventRef cge = CGEventCreate(nil);
        CGPoint loc = CGEventGetLocation(cge);
        CFRelease(cge);
        location.x = floor(loc.x + ev->mouseMove.dx);
        location.y = floor(loc.y + ev->mouseMove.dy);
        options = (options & ~kIOHIDSetRelativeCursorPosition) | kIOHIDSetCursorPosition;
    }
    kern_return_t res = IOHIDPostEvent(event, type, location, ev, kNXEventDataVersion, flags | NX_NONCOALSESCEDMASK, options);
    if(res != kIOReturnSuccess && !silence_errors)
        ckb_warn("Post event failed: %x\n", res);

    if(uid != 0)
        seteuid(0);
    if(gid != 0)
        setegid(0);
    euid_guard_stop;
}

// Keypress
#define aux_key_data(scancode, down, is_repeat) ((scancode) << 16 | ((down) ? 0x0a00 : 0x0b00) | !!(is_repeat))
static void postevent_kp(io_connect_t event, int kbflags, int scancode, int down, int is_flags, int is_repeat){
    NXEventData kp;
    memset(&kp, 0, sizeof(kp));
    UInt32 type;
    IOOptionBits flags = kbflags;
    IOOptionBits options = 0;
    if(scancode == KEY_CAPSLOCK){
        // Caps lock emits NX_FLAGSCHANGED when pressed, but also NX_SYSDEFINED on both press and release
        kp.compound.subType = NX_SUBTYPE_AUX_CONTROL_BUTTONS;
        kp.compound.misc.L[0] = aux_key_data(NX_KEYTYPE_CAPS_LOCK, down, is_repeat);
        postevent(event, NX_SYSDEFINED, &kp, flags, options, 1);
        if(!down)
            return;
        memset(&kp, 0, sizeof(kp));
    }
    if(IS_MEDIA(scancode)){
        kp.compound.subType = (scancode != KEY_POWER ? NX_SUBTYPE_AUX_CONTROL_BUTTONS : NX_SUBTYPE_POWER_KEY);
        kp.compound.misc.L[0] = aux_key_data(scancode - KEY_MEDIA, down, is_repeat);
        type = NX_SYSDEFINED;
    } else {
        if(is_flags){
            // Modifier (shift, ctrl, etc)
            type = NX_FLAGSCHANGED;
            options = kIOHIDSetGlobalEventFlags;
        } else
            // Standard key
            type = down ? NX_KEYDOWN : NX_KEYUP;
        kp.key.repeat = is_repeat;
        kp.key.keyCode = scancode;
        kp.key.origCharSet = kp.key.charSet = NX_ASCIISET;
        if(IS_NUMPAD(scancode))
            flags |= NX_NUMERICPADMASK;
        else if(scancode == kVK_Help)
            flags |= NX_HELPMASK;
    }
    postevent(event, type, &kp, flags, options, !down || is_repeat);
    // Don't print errors on key up or key repeat ^
}

// Mouse button
static void postevent_mb(io_connect_t event, int button, int down){
    NXEventData mb;
    memset(&mb, 0, sizeof(mb));
    mb.compound.subType = NX_SUBTYPE_AUX_MOUSE_BUTTONS;
    mb.compound.misc.L[0] = (1 << button);
    mb.compound.misc.L[1] = down ? (1 << button) : 0;
    postevent(event, NX_SYSDEFINED, &mb, 0, 0, !down);
    // Mouse presses actually generate two events, one with a bitfield of buttons, one with a button number
    memset(&mb, 0, sizeof(mb));
    UInt32 type;
    mb.mouse.buttonNumber = button;
    switch(button){
    case 0:
        type = down ? NX_LMOUSEDOWN : NX_LMOUSEUP;
        break;
    case 1:
        type = down ? NX_RMOUSEDOWN : NX_RMOUSEUP;
        break;
    default:
        type = down ? NX_OMOUSEDOWN : NX_OMOUSEUP;
    }
    if(down)
        mb.mouse.pressure = 255;
    postevent(event, type, &mb, 0, 0, 1);
}

// input_mac_mouse.c
extern void wheel_accel(io_connect_t event, int* deltaAxis1, SInt32* fixedDeltaAxis1, SInt32* pointDeltaAxis1);
extern void mouse_accel(io_connect_t event, int* x, int* y);

// Mouse wheel
static void postevent_wheel(io_connect_t event, int use_accel, int value){
    NXEventData mm;
    memset(&mm, 0, sizeof(mm));
    if(use_accel){
        wheel_accel(event, &value, &mm.scrollWheel.fixedDeltaAxis1, &mm.scrollWheel.pointDeltaAxis1);
        mm.scrollWheel.deltaAxis1 = value;
    } else {
        // If acceleration is disabled, use a fixed delta of 3
        mm.scrollWheel.deltaAxis1 = value * 3;
    }
    postevent(event, NX_SCROLLWHEELMOVED, &mm, 0, 0, 0);
}

// Mouse axis
static void postevent_mm(io_connect_t event, int x, int y, int use_accel, uchar buttons){
    NXEventData mm;
    memset(&mm, 0, sizeof(mm));
    UInt32 type = NX_MOUSEMOVED;
    if(use_accel)
        mouse_accel(event, &x, &y);
    mm.mouseMove.dx = x;
    mm.mouseMove.dy = y;
    if(buttons != 0){
        // If a button is pressed, the event type changes
        if(buttons & 1)
            type = NX_LMOUSEDRAGGED;
        else if(buttons & 2)
            type = NX_RMOUSEDRAGGED;
        else
            type = NX_OMOUSEDRAGGED;
        // Pick the button index based on the lowest-numbered button
        int button = 0;
        while(!(buttons & 1)){
            button++;
            buttons >>= 1;
        }
        mm.mouse.pressure = 255;
        mm.mouse.buttonNumber = button;
    }
    postevent(event, type, &mm, 0, kIOHIDSetRelativeCursorPosition, 1);
}

// Key repeat delay helper (result in ns)
long repeattime(io_connect_t event, int first){
    long delay = 0;
    IOByteCount actualSize = 0;
    if(IOHIDGetParameter(event, first ? CFSTR(kIOHIDInitialKeyRepeatKey) : CFSTR(kIOHIDKeyRepeatKey), sizeof(long), &delay, &actualSize) != KERN_SUCCESS || actualSize == 0)
        return -1;
    return delay;
}

// Send keyup events for every scancode in the keymap
void clearkeys(usbdevice* kb){
    for(int key = 0; key < N_KEYS_INPUT; key++){
        int scan = keymap[key].scan;
        if((scan & SCAN_SILENT) || scan == BTN_WHEELUP || scan == BTN_WHEELDOWN || IS_MEDIA(scan))
            continue;
        postevent_kp(kb->event, 0, scan, 0, 0, 0);
    }
}

// Opens HID service. Returns kIOReturnSuccess on success.
static int open_iohid(io_connect_t* connection){
    io_iterator_t iter;
    io_service_t service;
    // Open master port (if not done yet)
    static mach_port_t master = 0;
    kern_return_t res;
    if(!master && (res = IOMasterPort(bootstrap_port, &master)) != kIOReturnSuccess){
        master = 0;
        ckb_err("Unable to open master port: 0x%08x\n", res);
        goto failure;
    }
    // Open the HID service
    if((res = IOServiceGetMatchingServices(master, IOServiceMatching(kIOHIDSystemClass), &iter)) != kIOReturnSuccess)
        goto failure;
    service = IOIteratorNext(iter);
    if(!service){
        res = kIOReturnNotOpen;
        goto failure_release_iter;
    }
    if((res = IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, connection)) != kIOReturnSuccess){
        *connection = 0;
        goto failure_release_iter;
    }
    // Finished; release objects and return success
    IOObjectRelease(service);
    failure_release_iter:
    IOObjectRelease(iter);
    failure:
    return res;
}

int os_inputopen(usbdevice* kb){
    // The IO service isn't always ready at startup, so if it's not, wait until it is
    IOReturn res;
    while((res = open_iohid(&kb->event)) != kIOReturnSuccess){
        if(res != kIOReturnNotOpen){
            // If this is a more serious error, at least print a warning
            ckb_err("Unable to open HID system: 0x%08x\n", res);
            sleep(1);
            continue;
        }
        usleep(10000);
    }
    clearkeys(kb);
    return 0;
}

void os_inputclose(usbdevice* kb){
    if(kb->event){
        clearkeys(kb);
        IOServiceClose(kb->event);
        kb->event = 0;
    }
}

void os_keypress(usbdevice* kb, int scancode, int down){
    if(scancode & SCAN_MOUSE){
        if(scancode == BTN_WHEELUP){
            if(down)
                postevent_wheel(kb->event, !!(kb->features & FEAT_MOUSEACCEL), 1);
            return;
        } else if(scancode == BTN_WHEELDOWN){
            if(down)
                postevent_wheel(kb->event, !!(kb->features & FEAT_MOUSEACCEL), -1);
            return;
        }
        int button = scancode & ~SCAN_MOUSE;
        // Reverse or collapse left/right buttons if the system preferences say so
        int mode;
        if(IOHIDGetMouseButtonMode(kb->event, &mode) == kIOReturnSuccess){
            if(mode == kIOHIDButtonMode_ReverseLeftRightClicks && button == 0)
                button = 1;
            else if(mode != kIOHIDButtonMode_EnableRightClick && button == 1)
                button = 0;
        }
        postevent_mb(kb->event, button, down);
        if(down)
            kb->mousestate |= (1 << button);
        else
            kb->mousestate &= ~(1 << button);
        return;
    }
    // Some boneheaded Apple engineers decided to reverse kVK_ANSI_Grave and kVK_ISO_Section on the 105-key layouts...
    if(!HAS_ANY_FEATURE(kb, FEAT_LMASK)){
        // If the layout hasn't been set yet, it can be auto-detected from certain keys
        if(scancode == KEY_BACKSLASH_ISO || scancode == KEY_102ND)
            kb->features |= FEAT_ISO;
        else if(scancode == KEY_BACKSLASH)
            kb->features |= FEAT_ANSI;
    }
    if(scancode == KEY_BACKSLASH_ISO)
        scancode = KEY_BACKSLASH;
    if(HAS_FEATURES(kb, FEAT_ISO)){
        // Compensate for key reversal
        if(scancode == KEY_GRAVE)
            scancode = KEY_102ND;
        else if(scancode == KEY_102ND)
            scancode = KEY_GRAVE;
    }
    // Check for modifier keys and update flags
    int isMod = 0;
    IOOptionBits mod = 0;
    if(scancode == KEY_CAPSLOCK){
        if(down)
            kb->modifiers ^= NX_ALPHASHIFTMASK;
        isMod = 1;
    }
    else if(scancode == KEY_LEFTSHIFT) mod = NX_DEVICELSHIFTKEYMASK;
    else if(scancode == KEY_RIGHTSHIFT) mod = NX_DEVICERSHIFTKEYMASK;
    else if(scancode == KEY_LEFTCTRL) mod = NX_DEVICELCTLKEYMASK;
    else if(scancode == KEY_RIGHTCTRL) mod = NX_DEVICERCTLKEYMASK;
    else if(scancode == KEY_LEFTMETA) mod = NX_DEVICELCMDKEYMASK;
    else if(scancode == KEY_RIGHTMETA) mod = NX_DEVICERCMDKEYMASK;
    else if(scancode == KEY_LEFTALT) mod = NX_DEVICELALTKEYMASK;
    else if(scancode == KEY_RIGHTALT) mod = NX_DEVICERALTKEYMASK;
    if(mod){
        // Update global modifiers
        if(down)
            mod |= kb->modifiers;
        else
            mod = kb->modifiers & ~mod;
        if((mod & NX_DEVICELSHIFTKEYMASK) || (mod & NX_DEVICERSHIFTKEYMASK)) mod |= NX_SHIFTMASK; else mod &= ~NX_SHIFTMASK;
        if((mod & NX_DEVICELCTLKEYMASK) || (mod & NX_DEVICERCTLKEYMASK)) mod |= NX_CONTROLMASK; else mod &= ~NX_CONTROLMASK;
        if((mod & NX_DEVICELCMDKEYMASK) || (mod & NX_DEVICERCMDKEYMASK)) mod |= NX_COMMANDMASK; else mod &= ~NX_COMMANDMASK;
        if((mod & NX_DEVICELALTKEYMASK) || (mod & NX_DEVICERALTKEYMASK)) mod |= NX_ALTERNATEMASK; else mod &= ~NX_ALTERNATEMASK;
        kb->modifiers = mod;
        kb->lastkeypress = KEY_NONE;
        isMod = 1;
    } else if(!isMod){
        // For any other key, trigger key repeat
        if(down){
            long repeat = repeattime(kb->event, 1);
            if(repeat > 0){
                kb->lastkeypress = scancode;
                clock_gettime(CLOCK_MONOTONIC, &kb->keyrepeat);
                timespec_add(&kb->keyrepeat, repeat);
            } else
                kb->lastkeypress = KEY_NONE;
        } else
            kb->lastkeypress = KEY_NONE;
    }

    postevent_kp(kb->event, kb->modifiers, scancode, down, isMod, 0);
}

// Retrigger the last-pressed key
void _keyretrigger(usbdevice* kb){
    int scancode = kb->lastkeypress;
    postevent_kp(kb->event, kb->modifiers, scancode, 1, 0, 1);
    // Set next key repeat time
    long repeat = repeattime(kb->event, 0);
    if(repeat > 0)
        timespec_add(&kb->keyrepeat, repeat);
    else
        kb->lastkeypress = KEY_NONE;
}

void keyretrigger(usbdevice* kb){
    // Repeat the key as many times as needed to catch up
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    while(!(kb->lastkeypress & (SCAN_SILENT | SCAN_MOUSE)) && timespec_ge(time, kb->keyrepeat))
        _keyretrigger(kb);
}

void os_mousemove(usbdevice* kb, int x, int y){
    postevent_mm(kb->event, x, y, !!(kb->features & FEAT_MOUSEACCEL), kb->mousestate);
}

void os_isync(usbdevice* kb){
    // OSX doesn't have any equivalent to the SYN_ events
}

void os_updateindicators(usbdevice* kb, int force){
    // Set NumLock on permanently
    char ileds = 1;
    // Set Caps Lock if enabled. Unlike Linux, OSX keyboards have independent caps lock states, so
    // we use the last-assigned value rather than fetching it from the system
    if(kb->modifiers & kCGEventFlagMaskAlphaShift)
        ileds |= 2;
    if(kb->active){
        usbmode* mode = kb->profile->currentmode;
        ileds = (ileds & ~mode->ioff) | mode->ion;
    }
    if(force || ileds != kb->ileds){
        kb->ileds = ileds;
        // Get a list of LED elements from handle 0
        long ledpage = kHIDPage_LEDs;
        const void* keys[] = { CFSTR(kIOHIDElementUsagePageKey) };
        const void* values[] = { CFNumberCreate(kCFAllocatorDefault, kCFNumberLongType, &ledpage) };
        CFDictionaryRef matching = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFRelease(values[0]);
        CFArrayRef leds;
        kern_return_t res = (*kb->handles[0])->copyMatchingElements(kb->handles[0], matching, &leds, 0);
        CFRelease(matching);
        if(res != kIOReturnSuccess)
            return;
        // Iterate through them and update the LEDs which have changed
        DELAY_SHORT(kb);
        CFIndex count = CFArrayGetCount(leds);
        for(CFIndex i = 0; i < count; i++){
            IOHIDElementRef led = (void*)CFArrayGetValueAtIndex(leds, i);
            uint32_t usage = IOHIDElementGetUsage(led);
            IOHIDValueRef value = IOHIDValueCreateWithIntegerValue(kCFAllocatorDefault, led, 0, !!(ileds & (1 << (usage - 1))));
            (*kb->handles[0])->setValue(kb->handles[0], led, value, 5000, 0, 0, 0);
            CFRelease(value);
        }
        CFRelease(leds);
    }
}

#endif
