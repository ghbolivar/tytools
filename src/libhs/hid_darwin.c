/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Niels Martignène <niels.martignene@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "common_priv.h"
#include <CoreFoundation/CFRunLoop.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include "device_priv.h"
#include "hid.h"
#include "list.h"
#include "platform.h"

// Used for HID devices, see serial_posix.c for serial devices
struct _hs_hid_darwin {
    io_service_t service;
    union {
        IOHIDDeviceRef hid_ref;
    };

    uint8_t *read_buf;
    size_t read_size;

    pthread_mutex_t mutex;
    bool mutex_init;
    int poll_pipe[2];
    int thread_ret;

    _hs_list_head reports;
    unsigned int allocated_reports;
    _hs_list_head free_reports;

    pthread_t read_thread;
    pthread_cond_t cond;
    bool cond_init;

    CFRunLoopRef thread_loop;
    CFRunLoopSourceRef shutdown_source;
    bool device_removed;
};

static void fire_device_event(hs_port *port)
{
    char buf = '.';
    write(port->u.hid->poll_pipe[1], &buf, 1);
}

static void reset_device_event(hs_port *port)
{
    char buf;
    read(port->u.hid->poll_pipe[0], &buf, 1);
}

static void hid_removal_callback(void *ctx, IOReturn result, void *sender)
{
    _HS_UNUSED(result);
    _HS_UNUSED(sender);

    hs_port *port = ctx;

    pthread_mutex_lock(&port->u.hid->mutex);
    port->u.hid->device_removed = true;
    CFRunLoopSourceSignal(port->u.hid->shutdown_source);
    pthread_mutex_unlock(&port->u.hid->mutex);

    fire_device_event(port);
}

struct hid_report {
    _hs_list_head list;

    size_t size;
    uint8_t data[];
};

static void hid_report_callback(void *ctx, IOReturn result, void *sender,
                                IOHIDReportType report_type, uint32_t report_id,
                                uint8_t *report_data, CFIndex report_size)
{
    _HS_UNUSED(result);
    _HS_UNUSED(sender);

    if (report_type != kIOHIDReportTypeInput)
        return;

    hs_port *port = ctx;

    struct hid_report *report;
    bool fire;
    int r;

    pthread_mutex_lock(&port->u.hid->mutex);

    fire = _hs_list_is_empty(&port->u.hid->reports);

    report = _hs_list_get_first(&port->u.hid->free_reports, struct hid_report, list);
    if (report) {
        _hs_list_remove(&report->list);
    } else {
        if (port->u.hid->allocated_reports == 64) {
            r = 0;
            goto cleanup;
        }

        // Don't forget the leading report ID
        report = calloc(1, sizeof(struct hid_report) + port->u.hid->read_size + 1);
        if (!report) {
            r = hs_error(HS_ERROR_MEMORY, NULL);
            goto cleanup;
        }
        port->u.hid->allocated_reports++;
    }

    // You never know, even if port->u.hid->red_size is supposed to be the maximum input report size
    if (report_size > (CFIndex)port->u.hid->read_size)
        report_size = (CFIndex)port->u.hid->read_size;

    report->data[0] = (uint8_t)report_id;
    memcpy(report->data + 1, report_data, report_size);
    report->size = (size_t)report_size + 1;

    _hs_list_add_tail(&port->u.hid->reports, &report->list);

    r = 0;
cleanup:
    if (r < 0)
        port->u.hid->thread_ret = r;
    pthread_mutex_unlock(&port->u.hid->mutex);
    if (fire)
        fire_device_event(port);
}

static void *hid_read_thread(void *ptr)
{
    hs_port *port = ptr;
    CFRunLoopSourceContext shutdown_ctx = {0};
    int r;

    pthread_mutex_lock(&port->u.hid->mutex);

    port->u.hid->thread_loop = CFRunLoopGetCurrent();

    shutdown_ctx.info = port->u.hid->thread_loop;
    shutdown_ctx.perform = (void (*)(void *))CFRunLoopStop;
    /* close_hid_device() could be called before the loop is running, while this thread is between
       pthread_barrier_wait() and CFRunLoopRun(). That's the purpose of the shutdown source. */
    port->u.hid->shutdown_source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &shutdown_ctx);
    if (!port->u.hid->shutdown_source) {
        r = hs_error(HS_ERROR_SYSTEM, "CFRunLoopSourceCreate() failed");
        goto error;
    }

    CFRunLoopAddSource(port->u.hid->thread_loop, port->u.hid->shutdown_source, kCFRunLoopCommonModes);
    IOHIDDeviceScheduleWithRunLoop(port->u.hid->hid_ref, port->u.hid->thread_loop, kCFRunLoopCommonModes);

    // This thread is ready, open_hid_device() can carry on
    port->u.hid->thread_ret = 1;
    pthread_cond_signal(&port->u.hid->cond);
    pthread_mutex_unlock(&port->u.hid->mutex);

    CFRunLoopRun();

    IOHIDDeviceUnscheduleFromRunLoop(port->u.hid->hid_ref, port->u.hid->thread_loop,
                                     kCFRunLoopCommonModes);

    pthread_mutex_lock(&port->u.hid->mutex);
    port->u.hid->thread_loop = NULL;
    pthread_mutex_unlock(&port->u.hid->mutex);

    return NULL;

error:
    port->u.hid->thread_ret = r;
    pthread_cond_signal(&port->u.hid->cond);
    pthread_mutex_unlock(&port->u.hid->mutex);
    return NULL;
}

static bool get_hid_device_property_number(IOHIDDeviceRef ref, CFStringRef prop,
                                           CFNumberType type, void *rn)
{
    CFTypeRef data = IOHIDDeviceGetProperty(ref, prop);
    if (!data || CFGetTypeID(data) != CFNumberGetTypeID())
        return false;

    return CFNumberGetValue(data, type, rn);
}

static int open_hid_device(hs_device *dev, hs_port_mode mode, hs_port **rport)
{
    hs_port *port;
    kern_return_t kret;
    int r;

    port = calloc(1, _HS_ALIGN_SIZE_FOR_TYPE(sizeof(*port), struct _hs_hid_darwin) +
                  sizeof(struct _hs_hid_darwin));
    if (!port) {
        r = hs_error(HS_ERROR_MEMORY, NULL);
        goto error;
    }
    port->dev = hs_device_ref(dev);
    port->mode = mode;
    port->u.hid = (struct _hs_hid_darwin *)((char *)port +
                                         _HS_ALIGN_SIZE_FOR_TYPE(sizeof(*port), struct _hs_hid_darwin));

    port->u.hid->poll_pipe[0] = -1;
    port->u.hid->poll_pipe[1] = -1;

    _hs_list_init(&port->u.hid->reports);
    _hs_list_init(&port->u.hid->free_reports);

    port->u.hid->service = IORegistryEntryFromPath(kIOMasterPortDefault, dev->path);
    if (!port->u.hid->service) {
        r = hs_error(HS_ERROR_NOT_FOUND, "Device '%s' not found", dev->path);
        goto error;
    }

    port->u.hid->hid_ref = IOHIDDeviceCreate(kCFAllocatorDefault, port->u.hid->service);
    if (!port->u.hid->hid_ref) {
        r = hs_error(HS_ERROR_NOT_FOUND, "Device '%s' not found", dev->path);
        goto error;
    }

    kret = IOHIDDeviceOpen(port->u.hid->hid_ref, 0);
    if (kret != kIOReturnSuccess) {
        r = hs_error(HS_ERROR_SYSTEM, "Failed to open HID device '%s'", dev->path);
        goto error;
    }

    IOHIDDeviceRegisterRemovalCallback(port->u.hid->hid_ref, hid_removal_callback, port);

    if (mode & HS_PORT_MODE_READ) {
        r = get_hid_device_property_number(port->u.hid->hid_ref, CFSTR(kIOHIDMaxInputReportSizeKey),
                                           kCFNumberSInt32Type, &port->u.hid->read_size);
        if (!r) {
            r = hs_error(HS_ERROR_SYSTEM, "HID device '%s' has no valid report size key", dev->path);
            goto error;
        }
        port->u.hid->read_buf = malloc(port->u.hid->read_size);
        if (!port->u.hid->read_buf) {
            r = hs_error(HS_ERROR_MEMORY, NULL);
            goto error;
        }

        IOHIDDeviceRegisterInputReportCallback(port->u.hid->hid_ref, port->u.hid->read_buf,
                                               (CFIndex)port->u.hid->read_size, hid_report_callback, port);

        r = pipe(port->u.hid->poll_pipe);
        if (r < 0) {
            r = hs_error(HS_ERROR_SYSTEM, "pipe() failed: %s", strerror(errno));
            goto error;
        }
        fcntl(port->u.hid->poll_pipe[0], F_SETFL,
                fcntl(port->u.hid->poll_pipe[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(port->u.hid->poll_pipe[1], F_SETFL,
                fcntl(port->u.hid->poll_pipe[1], F_GETFL, 0) | O_NONBLOCK);

        r = pthread_mutex_init(&port->u.hid->mutex, NULL);
        if (r < 0) {
            r = hs_error(HS_ERROR_SYSTEM, "pthread_mutex_init() failed: %s", strerror(r));
            goto error;
        }
        port->u.hid->mutex_init = true;

        r = pthread_cond_init(&port->u.hid->cond, NULL);
        if (r < 0) {
            r = hs_error(HS_ERROR_SYSTEM, "pthread_cond_init() failed: %s", strerror(r));
            goto error;
        }
        port->u.hid->cond_init = true;

        pthread_mutex_lock(&port->u.hid->mutex);

        r = pthread_create(&port->u.hid->read_thread, NULL, hid_read_thread, port);
        if (r) {
            r = hs_error(HS_ERROR_SYSTEM, "pthread_create() failed: %s", strerror(r));
            goto error;
        }

        /* Barriers are great for this, but OSX doesn't have those... And since it's the only
           place we would use them, it's probably not worth it to have a custom implementation. */
        while (!port->u.hid->thread_ret)
            pthread_cond_wait(&port->u.hid->cond, &port->u.hid->mutex);
        r = port->u.hid->thread_ret;
        port->u.hid->thread_ret = 0;
        pthread_mutex_unlock(&port->u.hid->mutex);
        if (r < 0)
            goto error;
    }

    *rport = port;
    return 0;

error:
    hs_port_close(port);
    return r;
}

static void close_hid_device(hs_port *port)
{
    if (port) {
        if (port->u.hid->shutdown_source) {
            pthread_mutex_lock(&port->u.hid->mutex);

            if (port->u.hid->thread_loop) {
                CFRunLoopSourceSignal(port->u.hid->shutdown_source);
                CFRunLoopWakeUp(port->u.hid->thread_loop);
            }

            pthread_mutex_unlock(&port->u.hid->mutex);
            pthread_join(port->u.hid->read_thread, NULL);

            CFRelease(port->u.hid->shutdown_source);
        }

        if (port->u.hid->cond_init)
            pthread_cond_destroy(&port->u.hid->cond);
        if (port->u.hid->mutex_init)
            pthread_mutex_destroy(&port->u.hid->mutex);

        _hs_list_splice(&port->u.hid->free_reports, &port->u.hid->reports);
        _hs_list_foreach(cur, &port->u.hid->free_reports) {
            struct hid_report *report = _hs_container_of(cur, struct hid_report, list);
            free(report);
        }

        close(port->u.hid->poll_pipe[0]);
        close(port->u.hid->poll_pipe[1]);

        free(port->u.hid->read_buf);

        if (port->u.hid->hid_ref) {
            IOHIDDeviceClose(port->u.hid->hid_ref, 0);
            CFRelease(port->u.hid->hid_ref);
        }
        if (port->u.hid->service)
            IOObjectRelease(port->u.hid->service);

        hs_device_unref(port->dev);
    }

    free(port);
}

static hs_descriptor get_hid_descriptor(const hs_port *port)
{
    return port->u.hid->poll_pipe[0];
}

const struct _hs_device_vtable _hs_darwin_hid_vtable = {
    .open = open_hid_device,
    .close = close_hid_device,

    .get_descriptor = get_hid_descriptor
};

ssize_t hs_hid_read(hs_port *port, uint8_t *buf, size_t size, int timeout)
{
    assert(port);
    assert(port->dev->type == HS_DEVICE_TYPE_HID);
    assert(port->mode & HS_PORT_MODE_READ);
    assert(buf);
    assert(size);

    struct hid_report *report;
    ssize_t r;

    if (port->u.hid->device_removed)
        return hs_error(HS_ERROR_IO, "Device '%s' was removed", port->dev->path);

    if (timeout) {
        struct pollfd pfd;
        uint64_t start;

        pfd.events = POLLIN;
        pfd.fd = port->u.hid->poll_pipe[0];

        start = hs_millis();
restart:
        r = poll(&pfd, 1, hs_adjust_timeout(timeout, start));
        if (r < 0) {
            if (errno == EINTR)
                goto restart;

            return hs_error(HS_ERROR_SYSTEM, "poll('%s') failed: %s", port->dev->path, strerror(errno));
        }
        if (!r)
            return 0;
    }

    pthread_mutex_lock(&port->u.hid->mutex);

    if (port->u.hid->thread_ret < 0) {
        r = port->u.hid->thread_ret;
        port->u.hid->thread_ret = 0;

        goto reset;
    }

    report = _hs_list_get_first(&port->u.hid->reports, struct hid_report, list);
    if (!report) {
        r = 0;
        goto cleanup;
    }

    if (size > report->size)
        size = report->size;
    memcpy(buf, report->data, size);
    r = (ssize_t)size;

    _hs_list_remove(&report->list);
    _hs_list_add(&port->u.hid->free_reports, &report->list);

reset:
    if (_hs_list_is_empty(&port->u.hid->reports))
        reset_device_event(port);

cleanup:
    pthread_mutex_unlock(&port->u.hid->mutex);
    return r;
}

static ssize_t send_report(hs_port *port, IOHIDReportType type, const uint8_t *buf, size_t size)
{
    uint8_t report;
    kern_return_t kret;

    if (port->u.hid->device_removed)
        return hs_error(HS_ERROR_IO, "Device '%s' was removed", port->dev->path);

    if (size < 2)
        return 0;

    report = buf[0];
    if (!report) {
        buf++;
        size--;
    }

    /* FIXME: find a way drop out of IOHIDDeviceSetReport() after a reasonable time, because
       IOHIDDeviceSetReportWithCallback() is broken. Perhaps we can open the device twice and
       close the write side to drop out of IOHIDDeviceSetReport() after a few seconds? Or maybe
       we can call IOHIDDeviceSetReport() in another thread and kill it, but I don't trust OSX
       to behave well in that case. The HID API does like to crash OSX for no reason. */
    kret = IOHIDDeviceSetReport(port->u.hid->hid_ref, type, report, buf, (CFIndex)size);
    if (kret != kIOReturnSuccess)
        return hs_error(HS_ERROR_IO, "I/O error while writing to '%s'", port->dev->path);

    return (ssize_t)size + !report;
}

ssize_t hs_hid_write(hs_port *port, const uint8_t *buf, size_t size)
{
    assert(port);
    assert(port->dev->type == HS_DEVICE_TYPE_HID);
    assert(port->mode & HS_PORT_MODE_WRITE);
    assert(buf);

    return send_report(port, kIOHIDReportTypeOutput, buf, size);
}

ssize_t hs_hid_get_feature_report(hs_port *port, uint8_t report_id, uint8_t *buf, size_t size)
{
    assert(port);
    assert(port->dev->type == HS_DEVICE_TYPE_HID);
    assert(port->mode & HS_PORT_MODE_READ);
    assert(buf);
    assert(size);

    CFIndex len;
    kern_return_t kret;

    if (port->u.hid->device_removed)
        return hs_error(HS_ERROR_IO, "Device '%s' was removed", port->dev->path);

    len = (CFIndex)size - 1;
    kret = IOHIDDeviceGetReport(port->u.hid->hid_ref, kIOHIDReportTypeFeature, report_id,
                                buf + 1, &len);
    if (kret != kIOReturnSuccess)
        return hs_error(HS_ERROR_IO, "IOHIDDeviceGetReport() failed on '%s'", port->dev->path);

    buf[0] = report_id;
    return (ssize_t)len;
}

ssize_t hs_hid_send_feature_report(hs_port *port, const uint8_t *buf, size_t size)
{
    assert(port);
    assert(port->dev->type == HS_DEVICE_TYPE_HID);
    assert(port->mode & HS_PORT_MODE_WRITE);
    assert(buf);

    return send_report(port, kIOHIDReportTypeFeature, buf, size);
}
