#pragma once

#include <string>
#include <cstdint>

enum class GpuCaptureBackend
{
    None,
    RenderDoc,   // Windows / Android
    MetalXcode   // macOS / iOS Metal
};

class GpuCaptureManager
{
public:
    using DeviceHandle = void*;  // RenderDoc: graphics device/context pointer; Metal: id<MTLCommandQueue> or id<MTLDevice>
    using WindowHandle = void*;  // RenderDoc: HWND / ANativeWindow*; Metal: usually unused, pass nullptr

    static GpuCaptureManager& Instance()
    {
        static GpuCaptureManager inst;
        return inst;
    }

    // Initialize the capture backend.
    // - backend:
    //     RenderDoc  -> Windows / Android with RenderDoc in-app API
    //     MetalXcode -> macOS / iOS with Metal MTLCaptureManager
    // - modulePath:
    //     RenderDoc: path to renderdoc.dll / .so, or empty for auto-detect.
    //     MetalXcode: ignored.
    bool Init(GpuCaptureBackend backend, const std::string& modulePath = std::string());

    // Shutdown and release all resources.
    void Shutdown();

    bool IsAvailable() const { return m_available; }
    GpuCaptureBackend GetBackend() const { return m_backend; }

    // Request a capture:
    // - RenderDoc: trigger next-frame capture (equivalent to pressing F12 in UI).
    // - MetalXcode: you can implement this as "capture next frame" via a flag.
    void TriggerCapture();

    // Explicitly start / end a capture range.
    // External code should call these at the beginning / end of the frame or region it wants to capture.
    bool StartFrameCapture(DeviceHandle device = nullptr, WindowHandle window = nullptr);
    bool EndFrameCapture(DeviceHandle device = nullptr, WindowHandle window = nullptr);

    // RenderDoc-only options (no effect on Metal backend).
    void SetRenderDocCaptureOptionU32(int opt, uint32_t val);
    void SetRenderDocCaptureOptionF32(int opt, float val);
    void SetRenderDocLogFilePathTemplate(const char* pathTemplate);

private:
    GpuCaptureManager() = default;
    ~GpuCaptureManager() = default;

    GpuCaptureManager(const GpuCaptureManager&) = delete;
    GpuCaptureManager& operator=(const GpuCaptureManager&) = delete;

private:
    GpuCaptureBackend m_backend   = GpuCaptureBackend::None;
    bool              m_available = false;

    // Implementation details for RenderDoc backend.
    struct RenderDocImpl;
    RenderDocImpl* m_rd = nullptr;

    // Implementation details for Metal backend.
    struct MetalImpl;
    MetalImpl* m_metal = nullptr;
};