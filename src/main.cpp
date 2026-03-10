/**
 * Interactive Brokers Trading Application - Main Entry Point
 *
 * A cross-platform trading application built with C++20, ImGui (Vulkan backend),
 * and IB Gateway API integration.
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <cmath>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "implot.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "ui/windows/ChartWindow.h"
#include "ui/windows/NewsWindow.h"
#include "ui/windows/TradingWindow.h"
#include "ui/windows/ScannerWindow.h"
#include "ui/windows/PortfolioWindow.h"

// Global window instances (created on login, destroyed on disconnect)
static ui::ChartWindow*    g_ChartWindow    = nullptr;
static ui::NewsWindow*     g_NewsWindow     = nullptr;
static ui::TradingWindow*  g_TradingWindow  = nullptr;
static ui::ScannerWindow*  g_ScannerWindow  = nullptr;
static ui::PortfolioWindow* g_PortfolioWindow = nullptr;

// ============================================================================
// Connection / Login state
// ============================================================================
enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error,
};

enum class ApiType {
    TWS = 0,
    Gateway,
};

struct LoginState {
    // UI fields
    char     username[64]  = "";
    char     password[64]  = "";
    char     host[128]     = "127.0.0.1";
    int      port          = 7497;       // updated automatically
    int      clientId      = 1;
    bool     isLive        = false;
    ApiType  apiType       = ApiType::TWS;

    // Runtime state
    ConnectionState state  = ConnectionState::Disconnected;
    std::string     errorMsg;
    std::string     connectedAs;

    // Simulated connection timer
    std::chrono::steady_clock::time_point connectStartTime;

    void UpdatePort() {
        if (apiType == ApiType::TWS) {
            port = isLive ? 7496 : 7497;
        } else {
            port = isLive ? 4001 : 4002;
        }
    }
};

static LoginState g_Login;
static GLFWwindow* g_AppWindow = nullptr; // set in main, used by UI

// ============================================================================
// Vulkan globals (managed via ImGui helper structures)
// ============================================================================
static VkAllocationCallbacks*   g_Allocator      = nullptr;
static VkInstance               g_Instance       = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device         = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily    = (uint32_t)-1;
static VkQueue                  g_Queue          = VK_NULL_HANDLE;
static VkPipelineCache          g_PipelineCache  = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t                 g_MinImageCount    = 2;
static bool                     g_SwapChainRebuild = false;

// ============================================================================
// Error helpers
// ============================================================================
static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0) abort();
}

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& props, const char* ext) {
    for (const VkExtensionProperties& p : props)
        if (strcmp(p.extensionName, ext) == 0)
            return true;
    return false;
}

// ============================================================================
// Vulkan setup
// ============================================================================
static void SetupVulkan(ImVector<const char*> instance_extensions) {
    VkResult err;

    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t props_count = 0;
        ImVector<VkExtensionProperties> props;
        vkEnumerateInstanceExtensionProperties(nullptr, &props_count, nullptr);
        props.resize((int)props_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &props_count, props.Data);
        check_vk_result(err);

        if (IsExtensionAvailable(props, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(props, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif
        create_info.enabledExtensionCount   = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);
    }

    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    {
        ImVector<const char*> device_exts;
        device_exts.push_back("VK_KHR_swapchain");

        uint32_t props_count = 0;
        ImVector<VkExtensionProperties> props;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &props_count, nullptr);
        props.resize((int)props_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &props_count, props.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(props, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_exts.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
        const float prio[] = {1.0f};
        VkDeviceQueueCreateInfo qi[1] = {};
        qi[0].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi[0].queueFamilyIndex = g_QueueFamily;
        qi[0].queueCount       = 1;
        qi[0].pQueuePriorities = prio;

        VkDeviceCreateInfo ci = {};
        ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount    = IM_ARRAYSIZE(qi);
        ci.pQueueCreateInfos       = qi;
        ci.enabledExtensionCount   = (uint32_t)device_exts.Size;
        ci.ppEnabledExtensionNames = device_exts.Data;
        err = vkCreateDevice(g_PhysicalDevice, &ci, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    {
        VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE},
        };
        VkDescriptorPoolCreateInfo pi = {};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets       = 0;
        for (auto& ps : pool_sizes) pi.maxSets += ps.descriptorCount;
        pi.poolSizeCount = IM_ARRAYSIZE(pool_sizes);
        pi.pPoolSizes    = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pi, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int w, int h) {
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, surface, &res);
    if (res != VK_TRUE) { fprintf(stderr, "No WSI support\n"); exit(-1); }

    const VkFormat fmts[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,   VK_FORMAT_R8G8B8_UNORM,
    };
    wd->Surface       = surface;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice, surface, fmts, IM_ARRAYSIZE(fmts), VK_COLORSPACE_SRGB_NONLINEAR_KHR);

    VkPresentModeKHR pm[] = {VK_PRESENT_MODE_FIFO_KHR};
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, surface, pm, IM_ARRAYSIZE(pm));

    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, w, h, g_MinImageCount, 0);
}

static void CleanupVulkan() {
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow(ImGui_ImplVulkanH_Window* wd) {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, wd, g_Allocator);
    vkDestroySurfaceKHR(g_Instance, wd->Surface, g_Allocator);
    wd->Surface = VK_NULL_HANDLE;
}

// ============================================================================
// Frame rendering
// ============================================================================
static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkSemaphore img_sem  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore rend_sem = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;

    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX,
                                          img_sem, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (err != VK_SUBOPTIMAL_KHR) check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX); check_vk_result(err);
    err = vkResetFences(g_Device, 1, &fd->Fence);                         check_vk_result(err);
    err = vkResetCommandPool(g_Device, fd->CommandPool, 0);                check_vk_result(err);

    VkCommandBufferBeginInfo bi = {};
    bi.sType  = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    err = vkBeginCommandBuffer(fd->CommandBuffer, &bi); check_vk_result(err);

    VkRenderPassBeginInfo ri = {};
    ri.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    ri.renderPass               = wd->RenderPass;
    ri.framebuffer              = fd->Framebuffer;
    ri.renderArea.extent.width  = (uint32_t)wd->Width;
    ri.renderArea.extent.height = (uint32_t)wd->Height;
    ri.clearValueCount          = 1;
    ri.pClearValues             = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &ri, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &img_sem;
    si.pWaitDstStageMask    = &wait_stage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &fd->CommandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &rend_sem;
    err = vkEndCommandBuffer(fd->CommandBuffer);  check_vk_result(err);
    err = vkQueueSubmit(g_Queue, 1, &si, fd->Fence); check_vk_result(err);
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild) return;
    VkSemaphore rend_sem = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR pi = {};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &rend_sem;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &wd->Swapchain;
    pi.pImageIndices      = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &pi);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (err != VK_SUBOPTIMAL_KHR) check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

// ============================================================================
// Login simulation helpers
// ============================================================================

// Called once per frame while Connecting — simulates a 1.5 s handshake delay.
// In a real app, replace with actual IBKR EClient::eConnect() call.
static void TickConnecting() {
    using namespace std::chrono;
    auto elapsed = duration_cast<milliseconds>(
        steady_clock::now() - g_Login.connectStartTime).count();

    if (elapsed >= 1500) {
        // Simulate success: in a real app check EClient::isConnected()
        if (g_Login.username[0] != '\0') {
            g_Login.state       = ConnectionState::Connected;
            g_Login.connectedAs = std::string(g_Login.username) +
                                  (g_Login.isLive ? " [LIVE]" : " [PAPER]");
            printf("[ibkr] Connected to %s:%d as %s\n",
                   g_Login.host, g_Login.port, g_Login.connectedAs.c_str());
            // Initialise trading windows
            delete g_ChartWindow;
            g_ChartWindow = new ui::ChartWindow();
            delete g_NewsWindow;
            g_NewsWindow    = new ui::NewsWindow();
            delete g_TradingWindow;
            g_TradingWindow = new ui::TradingWindow();
            delete g_ScannerWindow;
            g_ScannerWindow = new ui::ScannerWindow();
            // Wire scanner → chart: double-click a row to view its chart
            g_ScannerWindow->OnSymbolSelected = [](const std::string& sym) {
                if (g_ChartWindow) g_ChartWindow->SetSymbol(sym);
            };
            delete g_PortfolioWindow;
            g_PortfolioWindow = new ui::PortfolioWindow();
        } else {
            g_Login.state    = ConnectionState::Error;
            g_Login.errorMsg = "Username cannot be empty.";
        }
    }
}

static void StartConnect() {
    g_Login.state            = ConnectionState::Connecting;
    g_Login.errorMsg.clear();
    g_Login.connectStartTime = std::chrono::steady_clock::now();
    printf("[ibkr] Connecting to %s:%d  clientId=%d  account=%s\n",
           g_Login.host, g_Login.port, g_Login.clientId,
           g_Login.isLive ? "LIVE" : "PAPER");
}

static void Disconnect() {
    g_Login.state       = ConnectionState::Disconnected;
    g_Login.connectedAs.clear();
    g_Login.errorMsg.clear();
    delete g_ChartWindow;
    g_ChartWindow = nullptr;
    delete g_NewsWindow;
    g_NewsWindow    = nullptr;
    delete g_TradingWindow;
    g_TradingWindow = nullptr;
    delete g_ScannerWindow;
    g_ScannerWindow = nullptr;
    delete g_PortfolioWindow;
    g_PortfolioWindow = nullptr;
    printf("[ibkr] Disconnected.\n");
}

// ============================================================================
// Mandatory Login Window
// Renders a centered, non-closeable dialog that blocks the trading UI.
// ============================================================================
static void RenderLoginWindow() {
    // Dim the entire screen with a dark overlay
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##overlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav        | ImGuiWindowFlags_NoMove   |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End();
    ImGui::PopStyleVar(2);

    // Center the login dialog
    const ImVec2 loginSize = {440.0f, 360.0f};
    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(loginSize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);

    ImGuiWindowFlags dlgFlags =
        ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse   | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 10.0f));

    if (ImGui::Begin("Interactive Brokers Login", nullptr, dlgFlags)) {

        // ---- Header ----
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.1f, 1.0f));
        float textW = ImGui::CalcTextSize("Interactive Brokers").x;
        ImGui::SetCursorPosX((loginSize.x - textW) * 0.5f);
        ImGui::Text("Interactive Brokers");
        ImGui::PopStyleColor();

        float subW = ImGui::CalcTextSize("Trading Application").x;
        ImGui::SetCursorPosX((loginSize.x - subW) * 0.5f);
        ImGui::TextDisabled("Trading Application");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool isConnecting = (g_Login.state == ConnectionState::Connecting);

        // ---- Account Type ----
        ImGui::Text("Account Type");
        ImGui::SameLine(140);

        if (isConnecting) ImGui::BeginDisabled();

        bool changedType = false;
        if (ImGui::RadioButton("Paper", !g_Login.isLive)) { g_Login.isLive = false; changedType = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Live",   g_Login.isLive)) { g_Login.isLive = true;  changedType = true; }

        // ---- API Type ----
        ImGui::Text("API");
        ImGui::SameLine(140);
        int apiIdx = (int)g_Login.apiType;
        if (ImGui::RadioButton("TWS", &apiIdx, 0))     { g_Login.apiType = ApiType::TWS;     changedType = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("IB Gateway", &apiIdx, 1)) { g_Login.apiType = ApiType::Gateway; changedType = true; }
        if (changedType) g_Login.UpdatePort();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ---- Credentials ----
        ImGui::Text("Username");
        ImGui::SameLine(140);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##user", g_Login.username, sizeof(g_Login.username));

        ImGui::Text("Password");
        ImGui::SameLine(140);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##pass", g_Login.password, sizeof(g_Login.password),
                         ImGuiInputTextFlags_Password);

        // ---- Connection params ----
        ImGui::Spacing();
        ImGui::Text("Host");
        ImGui::SameLine(140);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##host", g_Login.host, sizeof(g_Login.host));

        ImGui::Text("Port");
        ImGui::SameLine(140);
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##port", &g_Login.port, 0);
        ImGui::SameLine();
        ImGui::TextDisabled(g_Login.isLive ? "(live)" : "(paper)");

        ImGui::Text("Client ID");
        ImGui::SameLine(140);
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##cid", &g_Login.clientId, 1);

        if (isConnecting) ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ---- Status ----
        if (g_Login.state == ConnectionState::Error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("Error: %s", g_Login.errorMsg.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        if (isConnecting) {
            // Animated dots while connecting
            using namespace std::chrono;
            int dots = (int)(duration_cast<milliseconds>(
                steady_clock::now() - g_Login.connectStartTime).count() / 400) % 4;
            char buf[32];
            snprintf(buf, sizeof(buf), "Connecting%.*s", dots, "...");
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
            float w = ImGui::CalcTextSize(buf).x;
            ImGui::SetCursorPosX((loginSize.x - w) * 0.5f);
            ImGui::Text("%s", buf);
            ImGui::PopStyleColor();
        }

        // ---- Connect button ----
        if (!isConnecting) {
            ImVec4 btnColor = g_Login.isLive
                ? ImVec4(0.7f, 0.1f, 0.1f, 1.0f)   // red for live
                : ImVec4(0.1f, 0.5f, 0.2f, 1.0f);  // green for paper
            ImVec4 btnHover = g_Login.isLive
                ? ImVec4(0.9f, 0.2f, 0.2f, 1.0f)
                : ImVec4(0.2f, 0.7f, 0.3f, 1.0f);
            ImVec4 btnActive = g_Login.isLive
                ? ImVec4(0.5f, 0.05f, 0.05f, 1.0f)
                : ImVec4(0.08f, 0.35f, 0.15f, 1.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  btnActive);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            const char* btnLabel = g_Login.isLive ? "Connect to Live Account"
                                                  : "Connect to Paper Account";
            float btnW = loginSize.x - 48.0f; // full width minus padding
            if (ImGui::Button(btnLabel, ImVec2(btnW, 32.0f)))
                StartConnect();

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
        }

        // ---- Warning for live ----
        if (g_Login.isLive && !isConnecting) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));
            ImGui::TextWrapped("WARNING: Live account uses real money. Orders will be executed in the market.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // Tick the connecting state machine each frame
    if (g_Login.state == ConnectionState::Connecting)
        TickConnecting();
}

// ============================================================================
// Trading UI (only shown when connected)
// ============================================================================
static void RenderTradingUI() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDecoration  | ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;

    if (ImGui::Begin("##TradingHost", nullptr, hostFlags)) {

        // ---- Menu bar ----
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Disconnect")) Disconnect();
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(g_AppWindow, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Demo Window", nullptr, nullptr, false);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Windows")) {
                ImGui::MenuItem("Chart",     nullptr, nullptr, false);
                ImGui::MenuItem("Order Entry", nullptr, nullptr, false);
                ImGui::MenuItem("Portfolio", nullptr, nullptr, false);
                ImGui::MenuItem("News",      nullptr, nullptr, false);
                ImGui::EndMenu();
            }

            // Status indicator on the right side of the menu bar
            const std::string& who = g_Login.connectedAs;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x
                            - ImGui::CalcTextSize(who.c_str()).x - 12.0f);
            ImGui::PushStyleColor(ImGuiCol_Text,
                g_Login.isLive ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                               : ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
            ImGui::Text("%s", who.c_str());
            ImGui::PopStyleColor();

            ImGui::EndMenuBar();
        }

        // Dockspace
        ImGuiID dockId = ImGui::GetID("TradingDock");
        ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    }
    ImGui::End();

    // ---- Chart window (Task #4) ----
    if (g_ChartWindow) g_ChartWindow->Render();

    // ---- News window (Task #5) ----
    if (g_NewsWindow)    g_NewsWindow->Render();

    // ---- Trading window (Task #6) ----
    if (g_TradingWindow) g_TradingWindow->Render();

    // ---- Scanner window (Task #7) ----
    if (g_ScannerWindow) g_ScannerWindow->Render();

    // ---- Portfolio window (Task #8) ----
    if (g_PortfolioWindow) g_PortfolioWindow->Render();

    if (ImGui::Begin("Order Entry - AAPL")) {
        static int  orderType      = 0;
        static char quantity[32]   = "100";
        static char limitPrice[32] = "";

        ImGui::Text("Symbol: AAPL");
        if (ImGui::BeginCombo("Order Type", orderType == 0 ? "Market" : "Limit")) {
            if (ImGui::Selectable("Market", orderType == 0)) orderType = 0;
            if (ImGui::Selectable("Limit",  orderType == 1)) orderType = 1;
            ImGui::EndCombo();
        }
        if (orderType == 1)
            ImGui::InputText("Limit Price", limitPrice, sizeof(limitPrice));
        ImGui::InputText("Quantity", quantity, sizeof(quantity));

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.2f, 1.0f));
        if (ImGui::Button("BUY",  ImVec2(80, 0)))
            printf("BUY  %s @ %s\n", quantity, orderType == 1 ? limitPrice : "MARKET");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("SELL", ImVec2(80, 0)))
            printf("SELL %s @ %s\n", quantity, orderType == 1 ? limitPrice : "MARKET");
        ImGui::PopStyleColor();
    }
    ImGui::End();

    if (ImGui::Begin("Portfolio Summary")) {
        static float equity = 125000.0f;
        static float dayPnl =   1250.5f;
        ImGui::Text("Total Equity: $%.2f", equity);
        ImVec4 pnlCol = dayPnl >= 0
            ? ImVec4(0.2f, 0.8f, 0.3f, 1.0f)
            : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
        ImGui::SameLine();
        ImGui::TextColored(pnlCol, "  Day P&L: %+.2f", dayPnl);
        if (ImGui::Button("Refresh")) { /* TODO: fetch from IB Gateway */ }
    }
    ImGui::End();

    if (ImGui::Begin("News Feed")) {
        const char* headlines[] = {
            "Market opens higher on strong earnings",
            "Fed signals potential rate cuts ahead",
            "Tech stocks rally amid AI optimism",
            "Oil prices stabilize after recent volatility",
        };
        for (const char* h : headlines)
            if (ImGui::Selectable(h))
                printf("News: %s\n", h);
    }
    ImGui::End();
}

// ============================================================================
// Top-level UI dispatcher
// ============================================================================
static void RenderMainUI() {
    if (g_Login.state != ConnectionState::Connected)
        RenderLoginWindow();
    else
        RenderTradingUI();
}

// ============================================================================
// Entry point
// ============================================================================
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    std::cout << "========================================\n"
              << "Interactive Brokers Trading Application\n"
              << "Version: 1.0.0\n"
              << "Build: " << __DATE__ << " " << __TIME__ << "\n"
              << "========================================\n";

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) { std::cerr << "Failed to initialize GLFW\n"; return 1; }
    if (!glfwVulkanSupported()) { std::cerr << "Vulkan not supported\n"; glfwTerminate(); return 1; }

    // Collect required Vulkan extensions from GLFW
    ImVector<const char*> extensions;
    uint32_t ext_count = 0;
    const char** glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    for (uint32_t i = 0; i < ext_count; i++) extensions.push_back(glfw_exts[i]);
    SetupVulkan(extensions);

    // Create window
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    g_AppWindow = glfwCreateWindow(1920, 1080, "IBKR Trading App", nullptr, nullptr);
    if (!g_AppWindow) {
        std::cerr << "Failed to create window\n";
        CleanupVulkan(); glfwTerminate(); return 1;
    }

    // Create surface + swapchain
    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, g_AppWindow, g_Allocator, &surface);
    check_vk_result(err);

    int fb_w, fb_h;
    glfwGetFramebufferSize(g_AppWindow, &fb_w, &fb_h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, fb_w, fb_h);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();
    {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding              = 0.0f;
        s.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForVulkan(g_AppWindow, true);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance              = g_Instance;
    init_info.PhysicalDevice        = g_PhysicalDevice;
    init_info.Device                = g_Device;
    init_info.QueueFamily           = g_QueueFamily;
    init_info.Queue                 = g_Queue;
    init_info.PipelineCache         = g_PipelineCache;
    init_info.DescriptorPool        = g_DescriptorPool;
    init_info.MinImageCount         = g_MinImageCount;
    init_info.ImageCount            = wd->ImageCount;
    init_info.Allocator             = g_Allocator;
    init_info.CheckVkResultFn       = check_vk_result;
    init_info.PipelineInfoMain.RenderPass  = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass     = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&init_info);

    // Initialize login defaults
    g_Login.UpdatePort();

    ImVec4 clear_color = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    printf("Application running. Close window to exit.\n");

    // ---- Main loop ----
    while (!glfwWindowShouldClose(g_AppWindow)) {
        glfwPollEvents();

        // Resize swapchain if needed
        int cur_w, cur_h;
        glfwGetFramebufferSize(g_AppWindow, &cur_w, &cur_h);
        if (cur_w > 0 && cur_h > 0 &&
            (g_SwapChainRebuild || wd->Width != cur_w || wd->Height != cur_h))
        {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(
                g_Instance, g_PhysicalDevice, g_Device, wd,
                g_QueueFamily, g_Allocator, cur_w, cur_h, g_MinImageCount, 0);
            wd->FrameIndex     = 0;
            g_SwapChainRebuild = false;
        }
        if (glfwGetWindowAttrib(g_AppWindow, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        RenderMainUI();

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool minimized = (draw_data->DisplaySize.x <= 0.0f ||
                                 draw_data->DisplaySize.y <= 0.0f);

        wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
        wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
        wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
        wd->ClearValue.color.float32[3] = clear_color.w;

        if (!minimized) FrameRender(wd, draw_data);

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        if (!minimized) FramePresent(wd);
    }

    // Cleanup
    err = vkDeviceWaitIdle(g_Device);
    check_vk_result(err);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    CleanupVulkanWindow(wd);
    CleanupVulkan();

    delete g_ChartWindow;
    g_ChartWindow = nullptr;
    delete g_NewsWindow;
    g_NewsWindow    = nullptr;
    delete g_TradingWindow;
    g_TradingWindow = nullptr;
    delete g_ScannerWindow;
    g_ScannerWindow = nullptr;
    delete g_PortfolioWindow;
    g_PortfolioWindow = nullptr;

    glfwDestroyWindow(g_AppWindow);
    glfwTerminate();

    std::cout << "Application terminated successfully.\n";
    return 0;
}
