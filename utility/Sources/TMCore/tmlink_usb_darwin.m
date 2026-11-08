/*
 * tmlink_usb_darwin.m - USB transport for macOS via IOUSBHost (10.15+).
 *
 * Finds the first Epson (VID 0x04B8) interface of class 7 (printer) or
 * 0xFF (vendor, TMUSB mode), opens it exclusively and uses its bulk OUT/IN
 * pipes.  Exclusive open fails while CUPS is printing - that is intended:
 * the utility must never share the pipe with a print job (Epson's Windows
 * utility corrupts prints when it does).
 */
#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOUSBHost/IOUSBHost.h>

#include "tmcore.h"

@interface TMUSBLink : NSObject
@property(strong) IOUSBHostInterface *intf;
@property(strong) IOUSBHostPipe *outPipe;
@property(strong) IOUSBHostPipe *inPipe;
@end
@implementation TMUSBLink
@end

static io_service_t find_interface(NSNumber *ifclass)
{
    CFMutableDictionaryRef match =
        [IOUSBHostInterface createMatchingDictionaryWithVendorID:@(0x04B8)
                                                       productID:nil
                                                       bcdDevice:nil
                                                 interfaceNumber:nil
                                              configurationValue:nil
                                                  interfaceClass:ifclass
                                               interfaceSubclass:nil
                                               interfaceProtocol:nil
                                                           speed:nil
                                                  productIDArray:nil];
    if (!match)
        return IO_OBJECT_NULL;
    io_iterator_t it = IO_OBJECT_NULL;
    /* MACH_PORT_NULL = default main port on every macOS version */
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, match, &it) != KERN_SUCCESS)
        return IO_OBJECT_NULL;
    io_service_t svc = IOIteratorNext(it);
    IOObjectRelease(it);
    return svc;
}

static int usb_write(void *ctx, const uint8_t *buf, size_t n)
{
    @autoreleasepool {
        TMUSBLink *u = (__bridge TMUSBLink *)ctx;
        NSMutableData *d = [NSMutableData dataWithBytes:buf length:n];
        NSUInteger done = 0;
        NSError *err = nil;
        if (![u.outPipe sendIORequestWithData:d bytesTransferred:&done
                            completionTimeout:10.0 error:&err] || done != n)
            return -1;
        return 0;
    }
}

static int usb_read(void *ctx, uint8_t *buf, size_t cap, int timeout_ms)
{
    @autoreleasepool {
        TMUSBLink *u = (__bridge TMUSBLink *)ctx;
        if (!u.inPipe)
            return -1;
        /* Full-speed bulk max packet is 64; ask for a multiple of it. */
        size_t want = cap < 64 ? 64 : (cap / 64) * 64;
        NSMutableData *d = [NSMutableData dataWithLength:want];
        NSUInteger got = 0;
        NSError *err = nil;
        NSTimeInterval t = timeout_ms <= 0 ? 0.05 : timeout_ms / 1000.0;
        BOOL ok = [u.inPipe sendIORequestWithData:d bytesTransferred:&got
                                completionTimeout:t error:&err];
        if (!ok) {
            if (err.code == kIOReturnTimeout || err.code == kIOReturnAborted)
                return 0;
            return -1;
        }
        if (got > cap)
            got = cap;
        memcpy(buf, d.bytes, got);
        return (int)got;
    }
}

static void usb_close(void *ctx)
{
    @autoreleasepool {
        TMUSBLink *u = (__bridge_transfer TMUSBLink *)ctx;
        [u.intf destroy];
        u.intf = nil;
    }
}

int tm_link_usb(tm_link *l)
{
    @autoreleasepool {
        io_service_t svc = find_interface(@7);
        if (svc == IO_OBJECT_NULL)
            svc = find_interface(@0xFF);
        if (svc == IO_OBJECT_NULL)
            return TM_ERR_NODEV;

        NSError *err = nil;
        IOUSBHostInterface *intf =
            [[IOUSBHostInterface alloc] initWithIOService:svc
                                                  options:IOUSBHostObjectInitOptionsNone
                                                    queue:nil
                                                    error:&err
                                          interestHandler:nil];
        IOObjectRelease(svc);
        if (!intf) {
            NSLog(@"tmcore: cannot open printer interface (busy printing?): %@", err);
            return TM_ERR_IO;
        }

        TMUSBLink *u = [TMUSBLink new];
        u.intf = intf;
        const IOUSBConfigurationDescriptor *cfg = intf.configurationDescriptor;
        const IOUSBInterfaceDescriptor *ifd = intf.interfaceDescriptor;
        const IOUSBEndpointDescriptor *ep = NULL;
        while ((ep = IOUSBGetNextEndpointDescriptor(cfg, ifd, (const IOUSBDescriptorHeader *)ep))) {
            if ((ep->bmAttributes & 0x03) != 0x02)          /* bulk only */
                continue;
            IOUSBHostPipe *p = [intf copyPipeWithAddress:ep->bEndpointAddress error:&err];
            if (!p)
                continue;
            if (ep->bEndpointAddress & 0x80) {
                if (!u.inPipe) u.inPipe = p;
            } else {
                if (!u.outPipe) u.outPipe = p;
            }
        }
        if (!u.outPipe) {
            [intf destroy];
            return TM_ERR_IO;
        }

        memset(l, 0, sizeof *l);
        l->ctx = (__bridge_retained void *)u;
        l->write = usb_write;
        l->read = usb_read;
        l->close = usb_close;
        return TM_OK;
    }
}
#endif
