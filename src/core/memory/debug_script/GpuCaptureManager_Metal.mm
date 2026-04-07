#import <TargetConditionals.h>
#if TARGET_OS_OSX || TARGET_OS_IPHONE

#import "GpuCaptureManager.h"
#import <Metal/Metal.h>
#import <Metal/MTLCaptureManager.h>

struct GpuCaptureManager::MetalImpl
{
    id<MTLDevice>       device    = nil;
    id<MTLCommandQueue> queue     = nil;
    bool                capturing = false;
};

// Override Init for Metal backend on Apple platforms
bool GpuCaptureManager::Init(GpuCaptureBackend backend, const std::string& modulePath)
{
    if (backend == GpuCaptureBackend::MetalXcode)
    {
        Shutdown(); // cleanup any previous backend

        m_backend = backend;
        m_metal   = new MetalImpl();

        // You can choose either to create device/queue here,
        // or let external code pass them via StartFrameCapture's DeviceHandle.
        // Example (optional):
        // m_metal->device = MTLCreateSystemDefaultDevice();
        // m_metal->queue  = [m_metal->device newCommandQueue];

        m_available = true;
        (void)modulePath;
        return true;
    }

    // For non-Metal backend, RenderDoc Init will handle it in the .cpp
    return false;
}

void GpuCaptureManager::Shutdown()
{
    // Release Metal backend
    if (m_metal)
    {
        m_metal->device    = nil;
        m_metal->queue     = nil;
        m_metal->capturing = false;
        delete m_metal;
        m_metal = nullptr;
    }

    // RenderDoc backend will be released in the RenderDoc .cpp file if used.
    if (m_rd)
    {
        // Here we just null the pointer; actual FreeLibrary/dlclose is in RenderDoc .cpp
        // It is safe because Shutdown() in RenderDoc file will be called when using that backend.
    }

    m_available = false;
    m_backend   = GpuCaptureBackend::None;
}

// TriggerCapture for Metal can be implemented as "capture next frame" if you like.
// Here we leave it empty by default and recommend using explicit Start/End.
void GpuCaptureManager::TriggerCapture()
{
    if (!m_available)
        return;

    if (m_backend == GpuCaptureBackend::MetalXcode)
    {
        // You can implement a "capture next frame" flag here.
        // For simplicity, we do nothing. External code should call
        // StartFrameCapture / EndFrameCapture explicitly.
    }
}

// Start a Metal capture using MTLCaptureManager.
// DeviceHandle is expected to be either:
// - id<MTLCommandQueue>, or
// - id<MTLDevice> (less recommended; queue is better).
bool GpuCaptureManager::StartFrameCapture(DeviceHandle device, WindowHandle window)
{
    if (!m_available || m_backend != GpuCaptureBackend::MetalXcode)
        return false;

    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> q = nil;

    if (device)
    {
        // Assume device is a command queue.
        // If you actually pass a device, you can adjust this logic.
        q = (__bridge id<MTLCommandQueue>)device;
    }

    if (!q && m_metal->queue)
        q = m_metal->queue;

    if (!dev && m_metal->device)
        dev = m_metal->device;

    if (!dev && q)
        dev = q.device;

    if (!dev && !q)
        return false;

    MTLCaptureManager* mgr = [MTLCaptureManager sharedCaptureManager];

    MTLCaptureDescriptor* desc = [[MTLCaptureDescriptor alloc] init];
    if (q)
        desc.captureObject = q;   // capture a specific queue
    else
        desc.captureObject = dev; // fallback to device

    NSError* error = nil;
    BOOL ok = [mgr startCaptureWithDescriptor:desc error:&error];
    if (!ok)
    {
        NSLog(@"[GpuCaptureManager] Failed to start Metal capture: %@", error);
        return false;
    }

    m_metal->capturing = true;
    (void)window;
    return true;
}

bool GpuCaptureManager::EndFrameCapture(DeviceHandle device, WindowHandle window)
{
    if (!m_available || m_backend != GpuCaptureBackend::MetalXcode)
        return false;

    if (!m_metal->capturing)
        return false;

    MTLCaptureManager* mgr = [MTLCaptureManager sharedCaptureManager];
    [mgr stopCapture];

    m_metal->capturing = false;
    (void)device;
    (void)window;
    return true;
}

// RenderDoc-only options: no-op for Metal backend.
void GpuCaptureManager::SetRenderDocCaptureOptionU32(int opt, uint32_t val)
{
    (void)opt;
    (void)val;
}

void GpuCaptureManager::SetRenderDocCaptureOptionF32(int opt, float val)
{
    (void)opt;
    (void)val;
}

void GpuCaptureManager::SetRenderDocLogFilePathTemplate(const char* pathTemplate)
{
    (void)pathTemplate;
}

#endif // TARGET_OS_OSX || TARGET_OS_IPHONE