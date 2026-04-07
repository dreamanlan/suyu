#include "GpuCaptureManager.h"
#include "renderdoc_app.h"

#if defined(_WIN32) || defined(_WIN64)
    #define GPUCAP_PLATFORM_WINDOWS 1
#else
    #define GPUCAP_PLATFORM_WINDOWS 0
#endif

#if defined(__ANDROID__)
    #define GPUCAP_PLATFORM_ANDROID 1
#else
    #define GPUCAP_PLATFORM_ANDROID 0
#endif

#if GPUCAP_PLATFORM_WINDOWS
    #include <windows.h>
#elif GPUCAP_PLATFORM_ANDROID
    #include <dlfcn.h>
#endif

#if GPUCAP_PLATFORM_WINDOWS || GPUCAP_PLATFORM_ANDROID

struct GpuCaptureManager::RenderDocImpl
{
    void*               module    = nullptr; // HMODULE on Windows, void* on Android
    bool                ownModule = false;
    RENDERDOC_API_1_5_0* api      = nullptr;
};

// Note: Metal backend initialization will be provided in a separate .mm file on Apple platforms.
// For non-Apple platforms we implement RenderDoc backend here.

bool GpuCaptureManager::Init(GpuCaptureBackend backend, const std::string& modulePath)
{
    Shutdown(); // clean up any previous backend

    m_backend = backend;

    if (backend == GpuCaptureBackend::RenderDoc)
    {
        m_rd = new RenderDocImpl();

    #if GPUCAP_PLATFORM_WINDOWS

        HMODULE mod = nullptr;
        bool own = false;

        if (!modulePath.empty())
        {
            mod = LoadLibraryA(modulePath.c_str());
            own = (mod != nullptr);
        }
        else
        {
            // Try to get already injected renderdoc.dll
            mod = GetModuleHandleA("renderdoc.dll");
            own = false;
        }

        if (!mod)
        {
            delete m_rd;
            m_rd = nullptr;
            m_available = false;
            return false;
        }

        m_rd->module    = mod;
        m_rd->ownModule = own;

        auto GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
        if (!GetAPI)
        {
            if (own)
                FreeLibrary(mod);
            delete m_rd;
            m_rd = nullptr;
            m_available = false;
            return false;
        }

        int ret = GetAPI(eRENDERDOC_API_Version_1_5_0, (void**)&m_rd->api);
        if (ret != 1 || !m_rd->api)
        {
            if (own)
                FreeLibrary(mod);
            delete m_rd;
            m_rd = nullptr;
            m_available = false;
            return false;
        }

    #elif GPUCAP_PLATFORM_ANDROID

        void* mod = nullptr;
        bool own = false;

        if (!modulePath.empty())
        {
            mod = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD);
            if (!mod)
                mod = dlopen(modulePath.c_str(), RTLD_NOW);
            own = (mod != nullptr);
        }
        else
        {
            // Try common RenderDoc layer/library names on Android
            const char* candidates[] = {
                "libVkLayer_GLES_RenderDoc.so",
                "libVkLayer_RenderDoc.so",
                "librenderdoc.so"
            };

            for (const char* name : candidates)
            {
                mod = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
                if (!mod)
                    mod = dlopen(name, RTLD_NOW);
                if (mod)
                {
                    own = true;
                    break;
                }
            }
        }

        if (!mod)
        {
            delete m_rd;
            m_rd = nullptr;
            m_available = false;
            return false;
        }

        m_rd->module    = mod;
        m_rd->ownModule = own;

        auto GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
        if (!GetAPI)
        {
            if (own)
                dlclose(mod);
            delete m_rd;
            m_rd = nullptr;
            m_available = false;
            return false;
        }

        int ret = GetAPI(eRENDERDOC_API_Version_1_5_0, (void**)&m_rd->api);
        if (ret != 1 || !m_rd->api)
        {
            if (own)
                dlclose(mod);
            delete m_rd;
            m_rd = nullptr;
            m_available = false;
            return false;
        }

    #else
        // RenderDoc backend not supported on this platform
        delete m_rd;
        m_rd = nullptr;
        m_available = false;
        return false;
    #endif

        m_available = true;
        return true;
    }

    // If backend is not RenderDoc, this .cpp does nothing.
    // Metal backend will be implemented in a .mm file on Apple platforms.
    m_available = false;
    return false;
}

void GpuCaptureManager::Shutdown()
{
    // Release RenderDoc backend
    if (m_rd)
    {
    #if GPUCAP_PLATFORM_WINDOWS
        if (m_rd->module && m_rd->ownModule)
            FreeLibrary((HMODULE)m_rd->module);
    #elif GPUCAP_PLATFORM_ANDROID
        if (m_rd->module && m_rd->ownModule)
            dlclose(m_rd->module);
    #endif
        delete m_rd;
        m_rd = nullptr;
    }

    // Metal backend will be cleaned up in the Metal implementation file
    if (m_metal)
    {
        // This will be actually implemented in the Metal .mm
        // We just ensure pointer is nullptr after Shutdown.
    }

    m_available = false;
    m_backend   = GpuCaptureBackend::None;
}

void GpuCaptureManager::TriggerCapture()
{
    if (!m_available)
        return;

    if (m_backend == GpuCaptureBackend::RenderDoc && m_rd && m_rd->api)
    {
        m_rd->api->TriggerCapture();
    }
    // Metal backend will override this in the .mm file if needed
}

bool GpuCaptureManager::StartFrameCapture(DeviceHandle device, WindowHandle window)
{
    if (!m_available)
        return false;

    if (m_backend == GpuCaptureBackend::RenderDoc && m_rd && m_rd->api)
    {
        m_rd->api->StartFrameCapture(device, window);
        return true;
    }

    return false; // Metal backend will override this in .mm file
}

bool GpuCaptureManager::EndFrameCapture(DeviceHandle device, WindowHandle window)
{
    if (!m_available)
        return false;

    if (m_backend == GpuCaptureBackend::RenderDoc && m_rd && m_rd->api)
    {
        int ok = m_rd->api->EndFrameCapture(device, window);
        return ok != 0;
    }

    return false; // Metal backend will override this in .mm file
}

void GpuCaptureManager::SetRenderDocCaptureOptionU32(int opt, uint32_t val)
{
    if (!m_available)
        return;
    if (m_backend != GpuCaptureBackend::RenderDoc || !m_rd || !m_rd->api)
        return;

    m_rd->api->SetCaptureOptionU32((RENDERDOC_CaptureOption)opt, val);
}

void GpuCaptureManager::SetRenderDocCaptureOptionF32(int opt, float val)
{
    if (!m_available)
        return;
    if (m_backend != GpuCaptureBackend::RenderDoc || !m_rd || !m_rd->api)
        return;

    m_rd->api->SetCaptureOptionF32((RENDERDOC_CaptureOption)opt, val);
}

void GpuCaptureManager::SetRenderDocLogFilePathTemplate(const char* pathTemplate)
{
    if (!m_available)
        return;
    if (m_backend != GpuCaptureBackend::RenderDoc || !m_rd || !m_rd->api)
        return;

    m_rd->api->SetLogFilePathTemplate(pathTemplate);
}

#endif //GPUCAP_PLATFORM_WINDOWS || GPUCAP_PLATFORM_ANDROID