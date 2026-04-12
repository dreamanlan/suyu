
#include "GpuCaptureManager.h"

#if defined(__OHOS__)

#include "renderdoc_app.h"
#include <dlfcn.h>
#include <vulkan/vulkan.h>

// Huawei Squid extensions to RENDERDOC_CaptureOption (starting from 1000).
// These values are recognized by libsquid.so's SetCaptureOptionU32/F32 implementation.
enum SquidCaptureOptionExt
{
    eSquidOption_VkCrossGpuCapture   = 1000,  // bool: enable cross-GPU capture
    eSquidOption_TrimmingBeginEID    = 1001,  // uint32: trimming begin event ID
    eSquidOption_TrimmingEndEID      = 1002,  // uint32: trimming end event ID
    eSquidOption_OverrideDebugLayers = 1003,  // bool: override debug layers
};

struct GpuCaptureManager::HuaweiSquidImpl
{
    // Method 2 (primary): libsquid.so with RenderDoc-compatible API (GLES & Vulkan)
    void*                squidModule = nullptr;
    RENDERDOC_API_1_1_2* squidApi    = nullptr;

    // Method 1 (fallback): Vulkan debug-utils label (Vulkan only)
    VkDevice device = VK_NULL_HANDLE;
    VkQueue  queue  = VK_NULL_HANDLE;
    PFN_vkQueueInsertDebugUtilsLabelEXT pfnInsertLabel = nullptr;

    bool HasSquidApi() const { return squidApi != nullptr; }
    bool HasVulkanLabel() const { return pfnInsertLabel != nullptr && queue != VK_NULL_HANDLE; }
};

bool GpuCaptureManager::Init(GpuCaptureBackend backend, const std::string& modulePath)
{
    Shutdown();

    m_backend = backend;

    if (backend == GpuCaptureBackend::HuaweiSquid)
    {
        m_squid = new HuaweiSquidImpl();

        // Try loading libsquid.so (RenderDoc-compatible API, works for both GLES and Vulkan).
        // Use RTLD_NOLOAD first: the squid layer may already be injected by the profiler tool.
        const char* libName = modulePath.empty() ? "libsquid.so" : modulePath.c_str();
        void* mod = dlopen(libName, RTLD_NOW | RTLD_NOLOAD);
        if (!mod)
            mod = dlopen(libName, RTLD_NOW);

        if (mod)
        {
            auto GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
            if (GetAPI)
            {
                int ret = GetAPI(eRENDERDOC_API_Version_1_1_2, (void**)&m_squid->squidApi);
                if (ret != 1)
                    m_squid->squidApi = nullptr;
            }

            if (m_squid->squidApi)
            {
                m_squid->squidModule = mod;
            }
            else
            {
                // GetAPI failed; don't hold the module, fall through to Vulkan label fallback.
                dlclose(mod);
            }
        }

        // Even if libsquid.so failed, we mark available = true.
        // Vulkan label fallback will be set up later via SetVulkanHandles().
        // If neither path works, Start/EndFrameCapture will simply return false.
        m_available = true;

        // Apply deferred Vulkan handles if SetVulkanHandles() was called before Init().
        if (m_pendingVkDevice || m_pendingVkQueue)
        {
            SetVulkanHandles(m_pendingVkDevice, m_pendingVkQueue);
            m_pendingVkDevice = nullptr;
            m_pendingVkQueue  = nullptr;
        }
        return true;
    }

    m_available = false;
    return false;
}

void GpuCaptureManager::Shutdown()
{
    if (m_squid)
    {
        if (m_squid->squidModule)
        {
            dlclose(m_squid->squidModule);
            m_squid->squidModule = nullptr;
        }
        delete m_squid;
        m_squid = nullptr;
    }

    m_pendingVkDevice = nullptr;
    m_pendingVkQueue  = nullptr;
    m_available = false;
    m_backend = GpuCaptureBackend::None;
}

void GpuCaptureManager::SetVulkanHandles(void* device, void* queue)
{
    // If Init() hasn't been called yet, cache the handles for deferred setup.
    if (!m_squid)
    {
        m_pendingVkDevice = device;
        m_pendingVkQueue  = queue;
        return;
    }

    if (m_backend != GpuCaptureBackend::HuaweiSquid)
        return;

    m_squid->device = (VkDevice)device;
    m_squid->queue = (VkQueue)queue;

    if (m_squid->device)
    {
        m_squid->pfnInsertLabel = (PFN_vkQueueInsertDebugUtilsLabelEXT)
            vkGetDeviceProcAddr(m_squid->device, "vkQueueInsertDebugUtilsLabelEXT");
    }
}

void GpuCaptureManager::TriggerCapture()
{
    if (!m_available || m_backend != GpuCaptureBackend::HuaweiSquid || !m_squid)
        return;

    // Primary: use libsquid.so RenderDoc-compatible API
    if (m_squid->HasSquidApi())
    {
        m_squid->squidApi->TriggerCapture();
        return;
    }

    // Fallback: Vulkan debug-utils label (single-frame begin + end inserted together;
    // the profiler tool will capture the next presented frame between these markers)
    if (m_squid->HasVulkanLabel())
    {
        VkDebugUtilsLabelEXT label = {};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = "capture-marker,begin_capture";
        m_squid->pfnInsertLabel(m_squid->queue, &label);

        label.pLabelName = "capture-marker,end_capture";
        m_squid->pfnInsertLabel(m_squid->queue, &label);
    }
}

bool GpuCaptureManager::StartFrameCapture(DeviceHandle device, WindowHandle window)
{
    if (!m_available || m_backend != GpuCaptureBackend::HuaweiSquid || !m_squid)
        return false;

    // Primary: use libsquid.so RenderDoc-compatible API
    if (m_squid->HasSquidApi())
    {
        m_squid->squidApi->StartFrameCapture(NULL, NULL);
        return true;
    }

    // Fallback: Vulkan debug-utils label
    if (m_squid->HasVulkanLabel())
    {
        VkDebugUtilsLabelEXT label = {};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = "capture-marker,begin_capture";
        m_squid->pfnInsertLabel(m_squid->queue, &label);
        return true;
    }

    (void)device;
    (void)window;
    return false;
}

bool GpuCaptureManager::EndFrameCapture(DeviceHandle device, WindowHandle window)
{
    if (!m_available || m_backend != GpuCaptureBackend::HuaweiSquid || !m_squid)
        return false;

    // Primary: use libsquid.so RenderDoc-compatible API
    if (m_squid->HasSquidApi())
    {
        int ok = m_squid->squidApi->EndFrameCapture(NULL, NULL);
        return ok != 0;
    }

    // Fallback: Vulkan debug-utils label
    if (m_squid->HasVulkanLabel())
    {
        VkDebugUtilsLabelEXT label = {};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = "capture-marker,end_capture";
        m_squid->pfnInsertLabel(m_squid->queue, &label);
        return true;
    }

    (void)device;
    (void)window;
    return false;
}

void GpuCaptureManager::SetRenderDocCaptureOptionU32(int opt, uint32_t val)
{
    if (!m_available || m_backend != GpuCaptureBackend::HuaweiSquid || !m_squid)
        return;
    if (!m_squid->HasSquidApi())
        return;

    m_squid->squidApi->SetCaptureOptionU32((RENDERDOC_CaptureOption)opt, val);
}

void GpuCaptureManager::SetRenderDocCaptureOptionF32(int opt, float val)
{
    if (!m_available || m_backend != GpuCaptureBackend::HuaweiSquid || !m_squid)
        return;
    if (!m_squid->HasSquidApi())
        return;

    m_squid->squidApi->SetCaptureOptionF32((RENDERDOC_CaptureOption)opt, val);
}

void GpuCaptureManager::SetRenderDocCaptureFilePathTemplate(const char* pathTemplate)
{
    if (!m_available || m_backend != GpuCaptureBackend::HuaweiSquid || !m_squid)
        return;
    if (!m_squid->HasSquidApi())
        return;

    m_squid->squidApi->SetCaptureFilePathTemplate(pathTemplate);
}

#endif // defined(__OHOS__)
