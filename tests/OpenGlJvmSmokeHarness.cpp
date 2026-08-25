#include <windows.h>

#include <GL/gl.h>
#include <jni.h>

#include <process.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr int kClientWidth = 640;
constexpr int kClientHeight = 360;

struct Arguments {
    std::wstring jvmPath;
    std::wstring agentPath;
    std::wstring pipeName;
    std::wstring token;
    bool splitThreads = false;
};

bool isHexToken(const std::wstring_view token) noexcept
{
    return token.size() == 32U &&
           std::all_of(token.begin(), token.end(), [](const wchar_t character) {
               return (character >= L'0' && character <= L'9') ||
                      (character >= L'a' && character <= L'f') ||
                      (character >= L'A' && character <= L'F');
           });
}

bool parseArguments(const int argc, wchar_t** const argv, Arguments& result)
{
    if (argc != 9 && argc != 11) {
        return false;
    }

    for (int index = 1; index + 1 < argc; index += 2) {
        const std::wstring_view key(argv[index]);
        const std::wstring value(argv[index + 1]);
        if (key == L"--jvm") {
            result.jvmPath = value;
        } else if (key == L"--agent") {
            result.agentPath = value;
        } else if (key == L"--pipe") {
            result.pipeName = value;
        } else if (key == L"--token") {
            result.token = value;
        } else if (key == L"--thread-mode") {
            if (value == L"split") {
                result.splitThreads = true;
            } else if (value != L"unified") {
                return false;
            }
        } else {
            return false;
        }
    }

    constexpr std::wstring_view pipePrefix = L"\\\\.\\pipe\\";
    return !result.jvmPath.empty() && !result.agentPath.empty() &&
           result.pipeName.starts_with(pipePrefix) &&
           isHexToken(result.token);
}

std::string wideToUtf8(const std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string converted(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            converted.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return converted;
}

std::wstring parentPath(const std::wstring& path)
{
    const std::size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0U, separator);
}

std::wstring lastErrorText(const DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD size = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0U, reinterpret_cast<wchar_t*>(&buffer), 0U, nullptr);
    std::wstring text = size != 0U && buffer != nullptr
        ? std::wstring(buffer, static_cast<std::size_t>(size))
        : L"Win32 error " + std::to_wstring(error);
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                              text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

struct WindowState {
    std::atomic<bool> running{true};
    bool splitOwnership = false;
};

constexpr UINT kDestroySplitWindowMessage = WM_APP + 0x41U;

LRESULT CALLBACK smokeWindowProcedure(HWND window,
                                      const UINT message,
                                      const WPARAM wParam,
                                      const LPARAM lParam)
{
    auto* state = reinterpret_cast<WindowState*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<WindowState*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CLOSE:
        if (state != nullptr && state->splitOwnership) {
            // The render thread owns the HDC/HGLRC. Ask it to stop first; it
            // posts kDestroySplitWindowMessage only after releasing GL state.
            state->running.store(false, std::memory_order_release);
            return 0;
        }
        ::DestroyWindow(window);
        return 0;
    case kDestroySplitWindowMessage:
        ::DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
            state->running.store(false, std::memory_order_release);
        }
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

class OpenGlWindow final {
public:
    OpenGlWindow() = default;
    OpenGlWindow(const OpenGlWindow&) = delete;
    OpenGlWindow& operator=(const OpenGlWindow&) = delete;

    ~OpenGlWindow()
    {
        destroy();
    }

    bool create()
    {
        m_instance = ::GetModuleHandleW(nullptr);
        if (m_instance == nullptr) {
            return fail(L"GetModuleHandleW", ::GetLastError());
        }

        m_className = L"McOverlayOpenGlJvmSmoke-" +
                      std::to_wstring(::GetCurrentProcessId());
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &smokeWindowProcedure;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = m_className.c_str();
        m_class = ::RegisterClassExW(&windowClass);
        if (m_class == 0U) {
            return fail(L"RegisterClassExW", ::GetLastError());
        }

        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        constexpr DWORD extendedStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        RECT outer{0, 0, kClientWidth, kClientHeight};
        if (::AdjustWindowRectEx(&outer, style, FALSE, extendedStyle) == FALSE) {
            return fail(L"AdjustWindowRectEx", ::GetLastError());
        }

        // IsWindowVisible must be true for the real renderer path, but the
        // harness must never distract the desktop user. A shown tool window
        // parked far outside the virtual screen satisfies both constraints.
        m_window = ::CreateWindowExW(
            extendedStyle, m_className.c_str(), L"McOverlay private OpenGL smoke",
            style, -32000, -32000, outer.right - outer.left, outer.bottom - outer.top,
            nullptr, nullptr, m_instance, &m_state);
        if (m_window == nullptr) {
            return fail(L"CreateWindowExW", ::GetLastError());
        }

        m_deviceContext = ::GetDC(m_window);
        if (m_deviceContext == nullptr) {
            return fail(L"GetDC", ::GetLastError());
        }

        PIXELFORMATDESCRIPTOR descriptor{};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1U;
        descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32U;
        descriptor.cDepthBits = 24U;
        descriptor.cStencilBits = 8U;
        descriptor.iLayerType = PFD_MAIN_PLANE;
        const int pixelFormat = ::ChoosePixelFormat(m_deviceContext, &descriptor);
        if (pixelFormat == 0) {
            return fail(L"ChoosePixelFormat", ::GetLastError());
        }
        if (::SetPixelFormat(m_deviceContext, pixelFormat, &descriptor) == FALSE) {
            return fail(L"SetPixelFormat", ::GetLastError());
        }

        m_context = ::wglCreateContext(m_deviceContext);
        if (m_context == nullptr) {
            return fail(L"wglCreateContext", ::GetLastError());
        }
        if (::wglMakeCurrent(m_deviceContext, m_context) == FALSE) {
            return fail(L"wglMakeCurrent", ::GetLastError());
        }

        ::ShowWindow(m_window, SW_SHOWNOACTIVATE);
        ::UpdateWindow(m_window);
        RECT client{};
        if (::IsWindowVisible(m_window) == FALSE ||
            ::GetClientRect(m_window, &client) == FALSE ||
            client.right - client.left != kClientWidth ||
            client.bottom - client.top != kClientHeight) {
            std::wcerr << L"OpenGL smoke window failed its visibility/client-size invariant.\n";
            return false;
        }
        return true;
    }

    bool runFrames()
    {
        MSG message{};
        while (m_state.running.load(std::memory_order_acquire)) {
            while (::PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE) {
                if (message.message == WM_QUIT) {
                    m_state.running.store(false, std::memory_order_release);
                    break;
                }
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            if (!m_state.running.load(std::memory_order_acquire)) {
                break;
            }

            ::glViewport(0, 0, kClientWidth, kClientHeight);
            ::glClearColor(0.075F, 0.067F, 0.090F, 1.0F);
            ::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (::SwapBuffers(m_deviceContext) == FALSE) {
                return fail(L"SwapBuffers", ::GetLastError());
            }
            // The test is deliberately continuous without spinning at an
            // unbounded rate. Every detach deadline still observes many final
            // frames in which the render-thread cleanup can drain.
            ::Sleep(4U);
        }
        return true;
    }

    [[nodiscard]] HWND window() const noexcept
    {
        return m_window;
    }

    [[nodiscard]] DWORD windowThreadId() const noexcept
    {
        return ::GetCurrentThreadId();
    }

    [[nodiscard]] DWORD renderThreadId() const noexcept
    {
        return ::GetCurrentThreadId();
    }

    [[nodiscard]] static constexpr const char* modeName() noexcept
    {
        return "unified";
    }

private:
    bool fail(const wchar_t* const operation, const DWORD error)
    {
        std::wcerr << operation << L" failed: " << lastErrorText(error) << L"\n";
        return false;
    }

    void destroy() noexcept
    {
        if (::wglGetCurrentContext() == m_context) {
            ::wglMakeCurrent(nullptr, nullptr);
        }
        if (m_context != nullptr) {
            ::wglDeleteContext(m_context);
            m_context = nullptr;
        }
        if (m_deviceContext != nullptr && m_window != nullptr) {
            ::ReleaseDC(m_window, m_deviceContext);
            m_deviceContext = nullptr;
        }
        if (m_window != nullptr && ::IsWindow(m_window) != FALSE) {
            ::DestroyWindow(m_window);
        }
        m_window = nullptr;
        if (m_class != 0U && m_instance != nullptr) {
            ::UnregisterClassW(m_className.c_str(), m_instance);
            m_class = 0U;
        }
    }

    HINSTANCE m_instance = nullptr;
    ATOM m_class = 0U;
    std::wstring m_className;
    HWND m_window = nullptr;
    HDC m_deviceContext = nullptr;
    HGLRC m_context = nullptr;
    WindowState m_state;
};

// Lunar and a number of LWJGL wrappers do not necessarily create/pump the
// HWND on the thread which owns the OpenGL context. This surface deliberately
// reproduces that topology: m_windowThread owns CreateWindow/DispatchMessage,
// while the JVM main thread owns SetPixelFormat, HGLRC, and SwapBuffers.
class SplitThreadOpenGlWindow final {
public:
    SplitThreadOpenGlWindow() = default;
    SplitThreadOpenGlWindow(const SplitThreadOpenGlWindow&) = delete;
    SplitThreadOpenGlWindow& operator=(const SplitThreadOpenGlWindow&) = delete;

    ~SplitThreadOpenGlWindow()
    {
        destroy();
    }

    bool create()
    {
        m_instance = ::GetModuleHandleW(nullptr);
        if (m_instance == nullptr) {
            return fail(L"GetModuleHandleW", ::GetLastError());
        }
        m_state.splitOwnership = true;
        m_className = L"McOverlayOpenGlJvmSplitSmoke-" +
                      std::to_wstring(::GetCurrentProcessId());
        m_readyEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_readyEvent == nullptr) {
            return fail(L"CreateEventW", ::GetLastError());
        }

        unsigned threadId = 0U;
        m_windowThread = reinterpret_cast<HANDLE>(::_beginthreadex(
            nullptr, 0U, &SplitThreadOpenGlWindow::windowThreadEntry,
            this, 0U, &threadId));
        if (m_windowThread == nullptr) {
            return fail(L"_beginthreadex", static_cast<DWORD>(errno));
        }
        if (::WaitForSingleObject(m_readyEvent, 5000U) != WAIT_OBJECT_0) {
            std::wcerr << L"Timed out while creating the split-owner WGL window.\n";
            return false;
        }
        if (!m_windowReady.load(std::memory_order_acquire)) {
            std::wcerr << L"Split-owner window thread failed: "
                       << lastErrorText(m_windowError.load(std::memory_order_acquire))
                       << L"\n";
            return false;
        }

        const HWND window = m_window.load(std::memory_order_acquire);
        m_deviceContext = ::GetDC(window);
        if (m_deviceContext == nullptr) {
            return fail(L"GetDC", ::GetLastError());
        }

        PIXELFORMATDESCRIPTOR descriptor{};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1U;
        descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32U;
        descriptor.cDepthBits = 24U;
        descriptor.cStencilBits = 8U;
        descriptor.iLayerType = PFD_MAIN_PLANE;
        const int pixelFormat = ::ChoosePixelFormat(m_deviceContext, &descriptor);
        if (pixelFormat == 0) {
            return fail(L"ChoosePixelFormat", ::GetLastError());
        }
        if (::SetPixelFormat(m_deviceContext, pixelFormat, &descriptor) == FALSE) {
            return fail(L"SetPixelFormat", ::GetLastError());
        }
        m_context = ::wglCreateContext(m_deviceContext);
        if (m_context == nullptr) {
            return fail(L"wglCreateContext", ::GetLastError());
        }
        if (::wglMakeCurrent(m_deviceContext, m_context) == FALSE) {
            return fail(L"wglMakeCurrent", ::GetLastError());
        }

        m_renderThreadId = ::GetCurrentThreadId();
        if (m_renderThreadId == windowThreadId()) {
            std::wcerr << L"Split-owner invariant failed: HWND and HGLRC share a thread.\n";
            return false;
        }
        return true;
    }

    bool runFrames()
    {
        while (m_state.running.load(std::memory_order_acquire)) {
            ::glViewport(0, 0, kClientWidth, kClientHeight);
            ::glClearColor(0.075F, 0.067F, 0.090F, 1.0F);
            ::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (::SwapBuffers(m_deviceContext) == FALSE) {
                return fail(L"SwapBuffers", ::GetLastError());
            }
            ::Sleep(4U);
        }
        return true;
    }

    [[nodiscard]] HWND window() const noexcept
    {
        return m_window.load(std::memory_order_acquire);
    }

    [[nodiscard]] DWORD windowThreadId() const noexcept
    {
        return m_windowThreadId.load(std::memory_order_acquire);
    }

    [[nodiscard]] DWORD renderThreadId() const noexcept
    {
        return m_renderThreadId;
    }

    [[nodiscard]] static constexpr const char* modeName() noexcept
    {
        return "split";
    }

private:
    static unsigned __stdcall windowThreadEntry(void* const opaque) noexcept
    {
        static_cast<SplitThreadOpenGlWindow*>(opaque)->windowThreadMain();
        return 0U;
    }

    void signalWindowFailure(const DWORD error) noexcept
    {
        m_windowError.store(error, std::memory_order_release);
        m_windowReady.store(false, std::memory_order_release);
        ::SetEvent(m_readyEvent);
    }

    void windowThreadMain() noexcept
    {
        m_windowThreadId.store(::GetCurrentThreadId(), std::memory_order_release);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &smokeWindowProcedure;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = m_className.c_str();
        m_class = ::RegisterClassExW(&windowClass);
        if (m_class == 0U) {
            signalWindowFailure(::GetLastError());
            return;
        }

        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        constexpr DWORD extendedStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        RECT outer{0, 0, kClientWidth, kClientHeight};
        if (::AdjustWindowRectEx(&outer, style, FALSE, extendedStyle) == FALSE) {
            signalWindowFailure(::GetLastError());
            ::UnregisterClassW(m_className.c_str(), m_instance);
            m_class = 0U;
            return;
        }

        const HWND window = ::CreateWindowExW(
            extendedStyle, m_className.c_str(), L"McOverlay private split-thread smoke",
            style, -32000, -32000, outer.right - outer.left, outer.bottom - outer.top,
            nullptr, nullptr, m_instance, &m_state);
        if (window == nullptr) {
            signalWindowFailure(::GetLastError());
            ::UnregisterClassW(m_className.c_str(), m_instance);
            m_class = 0U;
            return;
        }
        m_window.store(window, std::memory_order_release);
        ::ShowWindow(window, SW_SHOWNOACTIVATE);
        ::UpdateWindow(window);
        RECT client{};
        if (::IsWindowVisible(window) == FALSE ||
            ::GetClientRect(window, &client) == FALSE ||
            client.right - client.left != kClientWidth ||
            client.bottom - client.top != kClientHeight) {
            const DWORD error = ::GetLastError() != ERROR_SUCCESS
                ? ::GetLastError() : ERROR_INVALID_WINDOW_HANDLE;
            signalWindowFailure(error);
            ::DestroyWindow(window);
            m_window.store(nullptr, std::memory_order_release);
            ::UnregisterClassW(m_className.c_str(), m_instance);
            m_class = 0U;
            return;
        }

        m_windowReady.store(true, std::memory_order_release);
        ::SetEvent(m_readyEvent);
        MSG message{};
        while (true) {
            const BOOL result = ::GetMessageW(&message, nullptr, 0U, 0U);
            if (result <= 0) {
                break;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (::IsWindow(window) != FALSE) {
            ::DestroyWindow(window);
        }
        m_window.store(nullptr, std::memory_order_release);
        ::UnregisterClassW(m_className.c_str(), m_instance);
        m_class = 0U;
    }

    bool fail(const wchar_t* const operation, const DWORD error)
    {
        std::wcerr << operation << L" failed: " << lastErrorText(error) << L"\n";
        return false;
    }

    void destroy() noexcept
    {
        m_state.running.store(false, std::memory_order_release);
        if (::wglGetCurrentContext() == m_context) {
            ::wglMakeCurrent(nullptr, nullptr);
        }
        if (m_context != nullptr) {
            ::wglDeleteContext(m_context);
            m_context = nullptr;
        }

        const HWND window = m_window.load(std::memory_order_acquire);
        if (m_deviceContext != nullptr && window != nullptr) {
            ::ReleaseDC(window, m_deviceContext);
            m_deviceContext = nullptr;
        }
        if (window != nullptr && ::IsWindow(window) != FALSE) {
            if (::PostMessageW(window, kDestroySplitWindowMessage, 0U, 0) == FALSE) {
                ::PostThreadMessageW(windowThreadId(), WM_QUIT, 0U, 0);
            }
        }
        if (m_windowThread != nullptr) {
            // Never return while this thread can still dereference `this`.
            // The controller enforces its own process-wide deadline and may
            // terminate only this private child if USER32 is genuinely hung.
            ::WaitForSingleObject(m_windowThread, INFINITE);
            ::CloseHandle(m_windowThread);
            m_windowThread = nullptr;
        }
        if (m_readyEvent != nullptr) {
            ::CloseHandle(m_readyEvent);
            m_readyEvent = nullptr;
        }
    }

    HINSTANCE m_instance = nullptr;
    ATOM m_class = 0U;
    std::wstring m_className;
    std::atomic<HWND> m_window{nullptr};
    std::atomic<DWORD> m_windowThreadId{0U};
    DWORD m_renderThreadId = 0U;
    HANDLE m_readyEvent = nullptr;
    HANDLE m_windowThread = nullptr;
    std::atomic<bool> m_windowReady{false};
    std::atomic<DWORD> m_windowError{ERROR_SUCCESS};
    HDC m_deviceContext = nullptr;
    HGLRC m_context = nullptr;
    WindowState m_state;
};

template <typename Function>
Function loadFunction(const HMODULE module, const char* const name) noexcept
{
    const FARPROC address = ::GetProcAddress(module, name);
    static_assert(sizeof(Function) == sizeof(address));
    Function function = nullptr;
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

} // namespace

int wmain(const int argc, wchar_t** const argv)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                   SEM_NOOPENFILEERRORBOX);

    Arguments arguments;
    if (!parseArguments(argc, argv, arguments)) {
        std::wcerr << L"Usage: McOverlayOpenGlJvmSmoke --jvm <jvm.dll> "
                      L"--agent <McOverlayAgent.dll> --pipe <\\\\.\\pipe\\name> "
                      L"--token <32-hex-token> [--thread-mode unified|split]\n";
        return 2;
    }

    // The JDK bin directory contains java.dll and other invocation-time
    // dependencies, while the explicitly loaded module lives in bin/server.
    const std::wstring serverDirectory = parentPath(arguments.jvmPath);
    const std::wstring jdkBinDirectory = parentPath(serverDirectory);
    if (jdkBinDirectory.empty() || ::SetDllDirectoryW(jdkBinDirectory.c_str()) == FALSE) {
        std::wcerr << L"Could not configure the private JDK DLL directory.\n";
        return 3;
    }

    const HMODULE jvmModule = ::LoadLibraryW(arguments.jvmPath.c_str());
    if (jvmModule == nullptr) {
        std::wcerr << L"LoadLibraryW(jvm.dll) failed: "
                   << lastErrorText(::GetLastError()) << L"\n";
        return 4;
    }

    using CreateJavaVm = jint(JNICALL*)(JavaVM**, void**, void*);
    const CreateJavaVm createJavaVm =
        loadFunction<CreateJavaVm>(jvmModule, "JNI_CreateJavaVM");
    if (createJavaVm == nullptr) {
        std::wcerr << L"JNI_CreateJavaVM is not exported by the selected jvm.dll.\n";
        ::FreeLibrary(jvmModule);
        return 5;
    }

    std::array<char, 20> classPathOption{};
    constexpr std::string_view classPath = "-Djava.class.path=.";
    std::copy(classPath.begin(), classPath.end(), classPathOption.begin());
    std::array<char, 5> reducedSignalsOption{'-', 'X', 'r', 's', '\0'};
    std::array<JavaVMOption, 2> vmOptions{};
    vmOptions[0].optionString = classPathOption.data();
    vmOptions[1].optionString = reducedSignalsOption.data();
    JavaVMInitArgs vmArguments{};
    vmArguments.version = JNI_VERSION_1_8;
    vmArguments.nOptions = static_cast<jint>(vmOptions.size());
    vmArguments.options = vmOptions.data();
    vmArguments.ignoreUnrecognized = JNI_FALSE;

    JavaVM* vm = nullptr;
    JNIEnv* environment = nullptr;
    const jint vmResult = createJavaVm(
        &vm, reinterpret_cast<void**>(&environment), &vmArguments);
    if (vmResult != JNI_OK || vm == nullptr || environment == nullptr) {
        std::wcerr << L"JNI_CreateJavaVM failed with code " << vmResult << L".\n";
        ::FreeLibrary(jvmModule);
        return 6;
    }

    int exitCode = 0;
    HMODULE agentModule = nullptr;
    using AgentOnAttach = jint(JNICALL*)(JavaVM*, char*, void*);
    using AgentOnUnload = void(JNICALL*)(JavaVM*);
    AgentOnUnload agentOnUnload = nullptr;

    const auto runSurface = [&](auto& window) -> int {
        int surfaceExitCode = 0;
        if (!window.create()) {
            surfaceExitCode = 7;
        } else {
            agentModule = ::LoadLibraryW(arguments.agentPath.c_str());
            if (agentModule == nullptr) {
                std::wcerr << L"LoadLibraryW(McOverlayAgent.dll) failed: "
                           << lastErrorText(::GetLastError()) << L"\n";
                surfaceExitCode = 8;
            } else {
                const AgentOnAttach agentOnAttach =
                    loadFunction<AgentOnAttach>(agentModule, "Agent_OnAttach");
                agentOnUnload =
                    loadFunction<AgentOnUnload>(agentModule, "Agent_OnUnload");
                const std::string pipe = wideToUtf8(arguments.pipeName);
                std::string token = wideToUtf8(arguments.token);
                std::transform(token.begin(), token.end(), token.begin(), [](const char value) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
                });
                std::string options = "pipe=" + pipe + ";token=" + token + ";protocol=1";
                if (agentOnAttach == nullptr || agentOnUnload == nullptr || pipe.empty() ||
                    token.size() != 32U) {
                    std::wcerr << L"Agent exports or UTF-8 options are invalid.\n";
                    surfaceExitCode = 9;
                } else if (agentOnAttach(vm, options.data(), nullptr) != JNI_OK) {
                    std::wcerr << L"Agent_OnAttach rejected the private JVM.\n";
                    surfaceExitCode = 10;
                } else {
                    std::cout << "MC_OVERLAY_OPENGL_SMOKE_READY "
                              << ::GetCurrentProcessId() << " " << std::hex
                              << reinterpret_cast<std::uintptr_t>(window.window())
                              << std::dec << " " << window.windowThreadId()
                              << " " << window.renderThreadId() << " "
                              << window.modeName() << std::endl;
                    if (!window.runFrames()) {
                        surfaceExitCode = 11;
                    }
                }
            }
        }

        // The successful controller path sends DETACH_COMPLETE before WM_CLOSE.
        // On every error path this remains a safe last-resort bounded stop while
        // the window and HGLRC still exist.
        if (agentOnUnload != nullptr) {
            agentOnUnload(vm);
        }
        return surfaceExitCode;
    };

    if (arguments.splitThreads) {
        SplitThreadOpenGlWindow window;
        exitCode = runSurface(window);
    } else {
        OpenGlWindow window;
        exitCode = runSurface(window);
    }

    const jint destroyResult = vm->DestroyJavaVM();
    if (destroyResult != JNI_OK && exitCode == 0) {
        std::wcerr << L"DestroyJavaVM failed with code " << destroyResult << L".\n";
        exitCode = 12;
    }
    if (agentModule != nullptr) {
        ::FreeLibrary(agentModule);
    }
    ::FreeLibrary(jvmModule);
    ::SetDllDirectoryW(nullptr);
    return exitCode;
}
