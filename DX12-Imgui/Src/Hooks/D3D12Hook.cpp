#include "D3D12Hook.h"
#include "Gui.h"
#include <kiero/kiero.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <iostream>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <thread>
#include <dxgidebug.h>

#pragma comment(lib, "dxguid.lib")

typedef HRESULT(__stdcall* PresentFunc)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef void(__stdcall* ExecuteCommandListsFunc)(ID3D12CommandQueue* pCommandQueue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);
typedef HRESULT(__stdcall* ResizeBuffersFunc)(IDXGISwapChain3* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
typedef HRESULT(__stdcall* SignalFunc)(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value);

PresentFunc oPresent = nullptr;
ExecuteCommandListsFunc oExecuteCommandLists = nullptr;
ResizeBuffersFunc oResizeBuffers = nullptr;
SignalFunc oSignal = nullptr;

static constexpr int NUM_FRAMES_IN_FLIGHT = 3;

struct FrameContext {
    ID3D12CommandAllocator* CommandAllocator = nullptr;
    UINT64 FenceValue = 0;
    ID3D12Resource* MainRenderTargetResource = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE MainRenderTargetDescriptor = {};
};

HWND g_Window = nullptr;
WNDPROC g_OriginalWndProc = nullptr;

ID3D12Device* g_D3DDevice = nullptr;
ID3D12DescriptorHeap* g_RtvDescHeap = nullptr;
ID3D12DescriptorHeap* g_SrvDescHeap = nullptr;
ID3D12CommandQueue* g_CommandQueue = nullptr;
ID3D12GraphicsCommandList* g_CommandList = nullptr;
ID3D12Fence* g_Fence = nullptr;
IDXGISwapChain3* g_SwapChain = nullptr;

FrameContext* g_FrameContexts = nullptr;
int g_BackBufferCount = -1;
UINT g_FrameIndex = 0;
UINT g_FenceValue = 0;
UINT64 g_LastSignaledFenceValue = 0;

HANDLE g_FenceEvent = nullptr;
HANDLE g_SwapChainWaitableObject = nullptr;

bool g_SwapChainOccluded = false;
bool g_ShouldRender = true;
UINT g_ResizeWidth = 0;
UINT g_ResizeHeight = 0;

void CreateRenderTargets() {
    if (!g_SwapChain || !g_D3DDevice || !g_FrameContexts || g_BackBufferCount <= 0) {
        LOG_ERROR("Cannot create render targets - DirectX objects not initialized");
        return;
    }

    for (UINT i = 0; i < g_BackBufferCount; i++) {
        ID3D12Resource* backBuffer = nullptr;
        HRESULT hr = g_SwapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to get back buffer %d: 0x%X", i, hr);
            continue;
        }

        g_D3DDevice->CreateRenderTargetView(backBuffer, nullptr,
            g_FrameContexts[i].MainRenderTargetDescriptor);
        g_FrameContexts[i].MainRenderTargetResource = backBuffer;
    }
}

void CleanupRenderTargets() {
    if (!g_FrameContexts || g_BackBufferCount <= 0) return;

    for (UINT i = 0; i < g_BackBufferCount; i++) {
        if (g_FrameContexts[i].MainRenderTargetResource) {
            g_FrameContexts[i].MainRenderTargetResource->Release();
            g_FrameContexts[i].MainRenderTargetResource = nullptr;
        }
    }
}

void WaitForGPU() {
    if (!g_FrameContexts || !g_Fence || !g_FenceEvent) return;

    FrameContext& frameCtx = g_FrameContexts[g_FrameIndex % NUM_FRAMES_IN_FLIGHT];
    UINT64 fenceValue = frameCtx.FenceValue;

    if (fenceValue == 0) return;

    frameCtx.FenceValue = 0;
    if (g_Fence->GetCompletedValue() >= fenceValue) return;

    g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
    WaitForSingleObject(g_FenceEvent, INFINITE);
}

void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding = ImVec2(10, 10);
    style.WindowRounding = 5.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.FramePadding = ImVec2(5, 5);
    style.FrameRounding = 4.0f;
    style.ChildRounding = 3.0f;
    style.ItemSpacing = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(6, 6);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 15.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = 3.0f;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.AntiAliasedLines = true;
    style.CurveTessellationTol = 1.25f;

    const ImVec4 RICH_BLACK = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
    const ImVec4 GOLD = ImVec4(1.0f, 0.84f, 0.0f, 1.0f);
    const ImVec4 GOLD_HOVER = ImVec4(0.98f, 0.70f, 0.00f, 1.00f);
    const ImVec4 WHITE = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    const ImVec4 GRAY_DARK = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    const ImVec4 GRAY_MID = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    const ImVec4 GRAY_LIGHT = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);

    style.Colors[ImGuiCol_Text] = WHITE;
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
    style.Colors[ImGuiCol_WindowBg] = RICH_BLACK;
    style.Colors[ImGuiCol_ChildBg] = RICH_BLACK;
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.10f, 0.98f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.40f, 0.40f, 0.45f, 0.60f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    style.Colors[ImGuiCol_FrameBg] = GRAY_DARK;
    style.Colors[ImGuiCol_FrameBgHovered] = GRAY_MID;
    style.Colors[ImGuiCol_FrameBgActive] = GRAY_LIGHT;

    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.08f, 0.08f, 0.75f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);

    style.Colors[ImGuiCol_ScrollbarBg] = RICH_BLACK;
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    style.Colors[ImGuiCol_CheckMark] = GOLD;
    style.Colors[ImGuiCol_SliderGrab] = GOLD;
    style.Colors[ImGuiCol_SliderGrabActive] = GOLD_HOVER;

    style.Colors[ImGuiCol_Button] = GRAY_DARK;
    style.Colors[ImGuiCol_ButtonHovered] = GRAY_MID;
    style.Colors[ImGuiCol_ButtonActive] = GRAY_LIGHT;

    style.Colors[ImGuiCol_Header] = GOLD;
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.98f, 0.70f, 0.00f, 0.80f);
    style.Colors[ImGuiCol_HeaderActive] = GOLD_HOVER;

    style.Colors[ImGuiCol_ResizeGrip] = GOLD;
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.98f, 0.70f, 0.00f, 0.75f);
    style.Colors[ImGuiCol_ResizeGripActive] = GOLD_HOVER;

    style.Colors[ImGuiCol_Tab] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    style.Colors[ImGuiCol_TabHovered] = GRAY_DARK;
    style.Colors[ImGuiCol_TabActive] = GRAY_LIGHT;
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);

    style.Colors[ImGuiCol_PlotLines] = GOLD;
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_PlotHistogram] = GOLD;
    style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.98f, 0.70f, 0.00f, 0.35f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(0.98f, 0.70f, 0.00f, 0.95f);
    style.Colors[ImGuiCol_NavHighlight] = GOLD;
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.18f, 0.18f, 0.18f, 0.35f);
}

void InitializeImGui() {
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    SetupImGuiStyle();

    ImGui_ImplWin32_Init(g_Window);
    ImGui_ImplDX12_Init(g_D3DDevice, NUM_FRAMES_IN_FLIGHT,
        DXGI_FORMAT_R8G8B8A8_UNORM, g_SrvDescHeap,
        g_SrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
        g_SrvDescHeap->GetGPUDescriptorHandleForHeapStart());

    ImGui_ImplDX12_CreateDeviceObjects();
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT APIENTRY WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam)) {
        return true;
    }
    return CallWindowProc(g_OriginalWndProc, hwnd, uMsg, wParam, lParam);
}

bool InitializeD3D12Objects(IDXGISwapChain3* swapChain) {
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D12Device), (void**)&g_D3DDevice))) {
        LOG_ERROR("Failed to get D3D12 device from swap chain");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);
    g_Window = desc.OutputWindow;
    g_BackBufferCount = desc.BufferCount;
    g_FrameContexts = new FrameContext[g_BackBufferCount];

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = g_BackBufferCount;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 1;

    if (FAILED(g_D3DDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvDescHeap)))) {
        LOG_ERROR("Failed to create RTV descriptor heap");
        return false;
    }

    SIZE_T rtvDescriptorSize = g_D3DDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvDescHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < g_BackBufferCount; i++) {
        g_FrameContexts[i].MainRenderTargetDescriptor = rtvHandle;
        rtvHandle.ptr += rtvDescriptorSize;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(g_D3DDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_SrvDescHeap)))) {
        LOG_ERROR("Failed to create SRV descriptor heap");
        return false;
    }

    ID3D12CommandAllocator* allocator;
    if (FAILED(g_D3DDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
        LOG_ERROR("Failed to create command allocator");
        return false;
    }

    for (UINT i = 0; i < g_BackBufferCount; i++) {
        g_FrameContexts[i].CommandAllocator = allocator;
    }

    if (FAILED(g_D3DDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
        nullptr, IID_PPV_ARGS(&g_CommandList))) || FAILED(g_CommandList->Close())) {
        LOG_ERROR("Failed to create command list");
        return false;
    }

    g_FenceEvent = CreateEvent(nullptr, false, false, nullptr);
    if (!g_FenceEvent) {
        LOG_ERROR("Failed to create fence event");
        return false;
    }

    g_SwapChain = swapChain;
    g_SwapChainWaitableObject = swapChain->GetFrameLatencyWaitableObject();

    return true;
}

HRESULT __fastcall HookedPresent(IDXGISwapChain3* swapChain, UINT syncInterval, UINT flags) {
    static bool initialized = false;

    if (!initialized) {
        if (InitializeD3D12Objects(swapChain)) {
            CreateRenderTargets();
            g_OriginalWndProc = (WNDPROC)SetWindowLongPtr(g_Window, GWLP_WNDPROC, (LONG_PTR)WindowProc);
            InitializeImGui();
            initialized = true;
        }
        else {
            return oPresent(swapChain, syncInterval, flags);
        }
    }

    if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
        WaitForGPU();
        CleanupRenderTargets();
        g_SwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
        g_ResizeWidth = g_ResizeHeight = 0;
        CreateRenderTargets();
    }

    if (!g_CommandQueue) {
        LOG_ERROR("Command queue not available");
        return oPresent(swapChain, syncInterval, flags);
    }

    if (GetAsyncKeyState(VK_INSERT) & 1) {
        Gui::show_gui = !Gui::show_gui;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::GetIO().MouseDrawCursor = Gui::show_gui;
    Gui::Render();

    FrameContext& frameCtx = g_FrameContexts[swapChain->GetCurrentBackBufferIndex()];
    frameCtx.CommandAllocator->Reset();

    UINT backBufferIdx = g_SwapChain->GetCurrentBackBufferIndex();

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = g_FrameContexts[backBufferIdx].MainRenderTargetResource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    g_CommandList->Reset(frameCtx.CommandAllocator, nullptr);
    g_CommandList->ResourceBarrier(1, &barrier);
    g_CommandList->OMSetRenderTargets(1, &g_FrameContexts[backBufferIdx].MainRenderTargetDescriptor, FALSE, nullptr);
    g_CommandList->SetDescriptorHeaps(1, &g_SrvDescHeap);

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_CommandList);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_CommandList->ResourceBarrier(1, &barrier);
    g_CommandList->Close();

    g_CommandQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList* const*>(&g_CommandList));

    return oPresent(swapChain, syncInterval, flags);
}

void __fastcall HookedExecuteCommandLists(ID3D12CommandQueue* commandQueue, UINT numCommandLists,
    ID3D12CommandList* const* commandLists) {
    if (!g_CommandQueue) {
        g_CommandQueue = commandQueue;
    }
    oExecuteCommandLists(commandQueue, numCommandLists, commandLists);
}

HRESULT __fastcall HookedResizeBuffers(IDXGISwapChain3* swapChain, UINT bufferCount, UINT width,
    UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags) {

    if (!g_D3DDevice || !g_SwapChain) {
        LOG_ERROR("Cannot resize - DirectX objects not initialized");
        return oResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
    }

    if (g_D3DDevice) {
        ImGui_ImplDX12_InvalidateDeviceObjects();
    }

    CleanupRenderTargets();
    g_BackBufferCount = bufferCount;

    HRESULT result = oResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags);

    if (SUCCEEDED(result)) {
        CreateRenderTargets();
        if (g_D3DDevice) {
            ImGui_ImplDX12_CreateDeviceObjects();
        }
    }
    else {
        LOG_ERROR("ResizeBuffers failed: 0x%X", result);
    }

    return result;
}

HRESULT __fastcall HookedSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    if (g_CommandQueue && queue == g_CommandQueue) {
        g_Fence = fence;
        g_FenceValue = value;
    }
    return oSignal(queue, fence, value);
}

bool InitD3D12Hook() {
    LOG_INFO("Initializing D3D12 hook...");

    HMODULE d3d12Module = nullptr;
    HMODULE dxgiModule = nullptr;

    while (true) {
        d3d12Module = GetModuleHandleA("d3d12.dll");
        dxgiModule = GetModuleHandleA("dxgi.dll");

        if (d3d12Module && dxgiModule) break;

        if (WaitForSingleObject(GetCurrentProcess(), 1000) != WAIT_TIMEOUT) {
            LOG_ERROR("Process terminated while waiting for DirectX modules");
            return false;
        }

        LOG_INFO("Waiting for DirectX modules...");
    }

    LOG_INFO("DirectX modules found, setting up hooks...");

    try {
        if (kiero::init(kiero::RenderType::D3D12) != kiero::Status::Success) {
            LOG_ERROR("Failed to initialize kiero");
            return false;
        }

        struct HookBinding {
            int index;
            void** original;
            void* hook;
            const char* name;
        };

        HookBinding hooks[] = {
            {54, (void**)&oExecuteCommandLists, (void*)HookedExecuteCommandLists, "ExecuteCommandLists"},
            {58, (void**)&oSignal, (void*)HookedSignal, "Signal"},
            {140, (void**)&oPresent, (void*)HookedPresent, "Present"},
            {145, (void**)&oResizeBuffers, (void*)HookedResizeBuffers, "ResizeBuffers"}
        };

        bool allHooksSuccessful = true;
        for (const auto& hook : hooks) {
            if (kiero::bind(hook.index, hook.original, hook.hook) != kiero::Status::Success) {
                LOG_ERROR("Failed to hook %s", hook.name);
                allHooksSuccessful = false;
            }
        }

        if (!allHooksSuccessful) {
            LOG_ERROR("Failed to create one or more hooks");
            kiero::shutdown();
            return false;
        }

        LOG_INFO("D3D12 hook initialized successfully");
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during hook initialization: %s", e.what());
        kiero::shutdown();
        return false;
    }
    catch (...) {
        LOG_ERROR("Unknown exception during hook initialization");
        kiero::shutdown();
        return false;
    }
}

void ReleaseD3D12Hook() {
    LOG_INFO("Releasing D3D12 hook...");

    kiero::shutdown();

    if (g_D3DDevice) {
        Gui::Shutdown();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (g_CommandQueue && g_Fence && g_FenceEvent) {
        WaitForGPU();
    }

    CleanupRenderTargets();

    if (g_FrameContexts) {
        for (UINT i = 0; i < g_BackBufferCount; i++) {
            if (g_FrameContexts[i].CommandAllocator) {
                g_FrameContexts[i].CommandAllocator->Release();
            }
        }
        delete[] g_FrameContexts;
        g_FrameContexts = nullptr;
    }

    auto SafeRelease = [](auto*& ptr) {
        if (ptr) {
            ptr->Release();
            ptr = nullptr;
        }
        };

    SafeRelease(g_CommandList);
    SafeRelease(g_CommandQueue);
    SafeRelease(g_RtvDescHeap);
    SafeRelease(g_SrvDescHeap);
    SafeRelease(g_Fence);

    if (g_FenceEvent) {
        CloseHandle(g_FenceEvent);
        g_FenceEvent = nullptr;
    }
    if (g_SwapChainWaitableObject) {
        CloseHandle(g_SwapChainWaitableObject);
        g_SwapChainWaitableObject = nullptr;
    }

    if (g_OriginalWndProc && g_Window) {
        SetWindowLongPtr(g_Window, GWLP_WNDPROC, (LONG_PTR)g_OriginalWndProc);
    }

    g_D3DDevice = nullptr;
    g_SwapChain = nullptr;
    g_Window = nullptr;
    g_OriginalWndProc = nullptr;
    g_BackBufferCount = -1;
    g_FrameIndex = 0;
    g_FenceValue = 0;

    LOG_INFO("D3D12 hook released successfully");
}