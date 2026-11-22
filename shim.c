/*
 * shim.c - lets Focusrite's Scarlett MixControl 1.10.6 find 1st-gen Scarlett
 *          interfaces on modern macOS.
 *
 * Why this is needed
 * ------------------
 * MixControl was written for Apple's old in-kernel USB audio driver
 * (IOAudioFamily), which published an "AppleUSBAudioEngine" object for each
 * USB audio device. Modern macOS runs USB audio in a user-space daemon
 * (usbaudiod) and no longer creates those objects, so MixControl never
 * notices the Scarlett and shows "NO HARDWARE CONNECTED". Talking to the
 * device over USB still works fine - only the discovery step is broken.
 *
 * What this does
 * --------------
 * This library is loaded into MixControl and fixes the two discovery steps:
 *
 *  1. MixControl asks macOS to notify it when an "AppleUSBAudioEngine" with a
 *     Focusrite vendor/product ID appears. We change that request to watch
 *     for the device's USB audio-control interface instead, which sits in the
 *     same place under the USB device.
 *
 *  2. MixControl then reads "IOAudioEngineGlobalUniqueID" from under the USB
 *     device to find the matching CoreAudio device. We answer with the UID of
 *     that CoreAudio device - the same value the old driver used to publish.
 *
 * Everything else is passed straight through to macOS untouched, and only
 * Focusrite devices (USB vendor ID 0x1235) are affected.
 *
 * Diagnostics are written to ~/Library/Logs/ScarlettMixControlShim.log.
 * Set SCARLETT_SHIM_DEBUG=1 to also log every registry property lookup.
 */

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FOCUSRITE_VENDOR_ID 0x1235
#define SHIM_VERSION "1.0.0"

static FILE *g_log;
static int g_debug;

static void shim_log(const char *fmt, ...) {
    if (!g_log) return;
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(g_log, "%s [%d] ", ts, getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

__attribute__((constructor)) static void shim_init(void) {
    const char *home = getenv("HOME");
    if (home) {
        char path[1024];
        snprintf(path, sizeof path, "%s/Library/Logs/ScarlettMixControlShim.log", home);
        g_log = fopen(path, "a");
    }
    const char *dbg = getenv("SCARLETT_SHIM_DEBUG");
    g_debug = dbg && *dbg && strcmp(dbg, "0") != 0;
    shim_log("shim %s loaded (debug=%d)", SHIM_VERSION, g_debug);
}

static int cfnum_to_u32(CFTypeRef v, uint32_t *out) {
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return 0;
    int64_t n = 0;
    if (!CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &n)) return 0;
    *out = (uint32_t)n;
    return 1;
}

/* Walk up the IOService plane from `entry` until we reach the USB device.
 * Returns 1 and fills vendor/location if the owning USB device is found. */
static int find_usb_device(io_registry_entry_t entry, uint32_t *vendor, uint32_t *location) {
    io_registry_entry_t cur = entry;
    IOObjectRetain(cur);
    for (int depth = 0; depth < 16 && cur; depth++) {
        if (IOObjectConformsTo(cur, "IOUSBHostDevice") || IOObjectConformsTo(cur, "IOUSBDevice")) {
            CFTypeRef vid = IORegistryEntryCreateCFProperty(cur, CFSTR("idVendor"), kCFAllocatorDefault, 0);
            CFTypeRef loc = IORegistryEntryCreateCFProperty(cur, CFSTR("locationID"), kCFAllocatorDefault, 0);
            int ok = cfnum_to_u32(vid, vendor) && cfnum_to_u32(loc, location);
            if (vid) CFRelease(vid);
            if (loc) CFRelease(loc);
            IOObjectRelease(cur);
            return ok;
        }
        io_registry_entry_t parent = 0;
        kern_return_t kr = IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent);
        IOObjectRelease(cur);
        cur = (kr == KERN_SUCCESS) ? parent : 0;
    }
    if (cur) IOObjectRelease(cur);
    return 0;
}

static CFStringRef copy_device_string(AudioObjectID dev, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef s = NULL;
    UInt32 size = sizeof(CFStringRef);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &s) != noErr) return NULL;
    return s;
}

/* Find the CoreAudio device for the USB device at `location`.
 * USB audio device UIDs look like
 *   "AppleUSBAudioEngine:Focusrite:Scarlett 18i20 USB:100000:1,2"
 * where the 4th field is the USB locationID in hex. */
static CFStringRef copy_coreaudio_uid_for_location(uint32_t location) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr || size == 0)
        return NULL;
    AudioObjectID *ids = malloc(size);
    if (!ids) return NULL;
    CFStringRef result = NULL;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids) == noErr) {
        char needle[32];
        snprintf(needle, sizeof needle, ":%x:", location);
        for (UInt32 i = 0; i < size / sizeof *ids && !result; i++) {
            CFStringRef uid = copy_device_string(ids[i], kAudioDevicePropertyDeviceUID);
            if (!uid) continue;
            char buf[512];
            if (CFStringGetCString(uid, buf, sizeof buf, kCFStringEncodingUTF8) &&
                strncmp(buf, "AppleUSBAudioEngine:", 20) == 0 && strstr(buf, needle)) {
                result = uid;
                continue;
            }
            CFRelease(uid);
        }
    }
    free(ids);
    return result;
}

static CFTypeRef shim_IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key,
                                                      CFAllocatorRef allocator, IOOptionBits options) {
    /* Inside this library, calls to IORegistryEntryCreateCFProperty reach the real one. */
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, allocator, options);

    if (g_debug && key) {
        char k[128] = "";
        CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8);
        io_name_t cls = "";
        IOObjectGetClass(entry, cls);
        shim_log("lookup %s on %s -> %s", k, cls, value ? "found" : "missing");
    }

    if (value || !key || CFStringCompare(key, CFSTR("IOAudioEngineGlobalUniqueID"), 0) != kCFCompareEqualTo)
        return value;

    uint32_t vendor = 0, location = 0;
    if (!find_usb_device(entry, &vendor, &location) || vendor != FOCUSRITE_VENDOR_ID)
        return NULL;

    CFStringRef uid = copy_coreaudio_uid_for_location(location);
    char buf[512] = "(none)";
    if (uid) CFStringGetCString(uid, buf, sizeof buf, kCFStringEncodingUTF8);
    shim_log("IOAudioEngineGlobalUniqueID requested for Focusrite device at location 0x%x -> %s", location, buf);
    return uid; /* caller owns it, same as the real function */
}

/* ---- Debug-only tracing of MixControl's device discovery (SCARLETT_SHIM_DEBUG=1) ---- */

static void log_cf(const char *label, CFTypeRef obj) {
    if (!g_debug) return;
    char buf[1024] = "(null)";
    if (obj) {
        CFStringRef d = CFCopyDescription(obj);
        if (d) {
            CFStringGetCString(d, buf, sizeof buf, kCFStringEncodingUTF8);
            CFRelease(d);
        }
    }
    for (char *p = buf; *p; p++)
        if (*p == '\n') *p = ' ';
    shim_log("%s %s", label, buf);
}

static kern_return_t shim_IOServiceGetMatchingServices(mach_port_t port, CFDictionaryRef matching,
                                                       io_iterator_t *existing) {
    log_cf("IOServiceGetMatchingServices", matching);
    kern_return_t kr = IOServiceGetMatchingServices(port, matching, existing);
    if (g_debug) shim_log("  -> 0x%08x", kr);
    return kr;
}

/* MixControl waits for the old kernel driver's "AppleUSBAudioEngine" object to
 * appear for each supported vendor/product ID. That object no longer exists,
 * so for Focusrite devices we watch for the USB audio-control interface
 * (interface 0) instead. It sits directly below the USB device, exactly where
 * the audio engine used to be, so MixControl's walk up to the parent USB
 * device still lands in the right place. Returns a new dictionary, or NULL to
 * leave the request alone. */
static CFMutableDictionaryRef copy_rewritten_matching(CFDictionaryRef matching) {
    if (!matching) return NULL;
    CFTypeRef cls = CFDictionaryGetValue(matching, CFSTR(kIOProviderClassKey));
    if (!cls || CFGetTypeID(cls) != CFStringGetTypeID() ||
        CFStringCompare(cls, CFSTR("AppleUSBAudioEngine"), 0) != kCFCompareEqualTo)
        return NULL;
    uint32_t vendor = 0;
    if (!cfnum_to_u32(CFDictionaryGetValue(matching, CFSTR("idVendor")), &vendor) || vendor != FOCUSRITE_VENDOR_ID)
        return NULL;

    /* Use IOPropertyMatch (plain property comparison) rather than top-level USB
     * keys: USB interface matching only accepts certain key combinations
     * (e.g. it would also demand bConfigurationValue). */
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(props, CFSTR("idVendor"), CFDictionaryGetValue(matching, CFSTR("idVendor")));
    CFTypeRef product = CFDictionaryGetValue(matching, CFSTR("idProduct"));
    if (product) CFDictionarySetValue(props, CFSTR("idProduct"), product);
    int zero = 0;
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &zero);
    CFDictionarySetValue(props, CFSTR("bInterfaceNumber"), n);
    CFRelease(n);

    CFMutableDictionaryRef m = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(m, CFSTR(kIOProviderClassKey), CFSTR("IOUSBHostInterface"));
    CFDictionarySetValue(m, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(props);
    return m;
}

static kern_return_t shim_IOServiceAddMatchingNotification(IONotificationPortRef port, const io_name_t type,
                                                           CFDictionaryRef matching,
                                                           IOServiceMatchingCallback callback, void *refCon,
                                                           io_iterator_t *notification) {
    CFMutableDictionaryRef rewritten = copy_rewritten_matching(matching);
    if (rewritten) {
        log_cf("rewriting AppleUSBAudioEngine notification:", matching);
        shim_log("watching for a Focusrite USB interface in place of the old audio engine (%s)", type);
        CFRelease(matching); /* the real call would have consumed it */
        matching = rewritten;
    } else if (g_debug) {
        shim_log("IOServiceAddMatchingNotification type=%s", type);
        log_cf("  matching", matching);
    }
    kern_return_t kr = IOServiceAddMatchingNotification(port, type, matching, callback, refCon, notification);
    if (g_debug || (rewritten && kr != KERN_SUCCESS)) shim_log("  -> 0x%08x", kr);
    return kr;
}

static io_object_t shim_IOIteratorNext(io_iterator_t iterator) {
    io_object_t o = IOIteratorNext(iterator);
    if (g_debug && o) {
        io_name_t cls = "";
        IOObjectGetClass(o, cls);
        if (strstr(cls, "USB")) shim_log("IOIteratorNext -> %s", cls);
    }
    return o;
}

static kern_return_t shim_IOCreatePlugInInterfaceForService(io_service_t service, CFUUIDRef pluginType,
                                                            CFUUIDRef interfaceType,
                                                            IOCFPlugInInterface ***theInterface,
                                                            SInt32 *theScore) {
    kern_return_t kr = IOCreatePlugInInterfaceForService(service, pluginType, interfaceType, theInterface, theScore);
    if (g_debug) {
        io_name_t cls = "";
        IOObjectGetClass(service, cls);
        shim_log("IOCreatePlugInInterfaceForService on %s -> 0x%08x", cls, kr);
    }
    return kr;
}

/* dyld interposing: these replace the real functions for the whole process. */
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement;
    const void *replacee;
} interposers[] = {
    {(const void *)shim_IORegistryEntryCreateCFProperty, (const void *)IORegistryEntryCreateCFProperty},
    {(const void *)shim_IOServiceGetMatchingServices, (const void *)IOServiceGetMatchingServices},
    {(const void *)shim_IOServiceAddMatchingNotification, (const void *)IOServiceAddMatchingNotification},
    {(const void *)shim_IOIteratorNext, (const void *)IOIteratorNext},
    {(const void *)shim_IOCreatePlugInInterfaceForService, (const void *)IOCreatePlugInInterfaceForService},
};
