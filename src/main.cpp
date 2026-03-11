/**
 * Interactive Brokers Trading Application — Main Entry Point
 *
 * Connects to a running IB Gateway or TWS via the C++ TWS API.
 * Authentication is handled by TWS/Gateway itself; we only need host/port/clientId.
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <cmath>
#include <unordered_map>

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

#include "core/services/IBKRClient.h"

// ============================================================================
// Global window instances
// ============================================================================
static ui::ChartWindow*    g_ChartWindow    = nullptr;
static ui::NewsWindow*     g_NewsWindow     = nullptr;
static ui::TradingWindow*  g_TradingWindow  = nullptr;
static ui::ScannerWindow*  g_ScannerWindow  = nullptr;
static ui::PortfolioWindow* g_PortfolioWindow = nullptr;

// IB API client (created on Connect, deleted on Disconnect)
static core::services::IBKRClient* g_IBClient = nullptr;

// Pending bar accumulation for the chart (fills as historicalData callbacks arrive)
static core::BarSeries g_pendingBars;

// Pending scanner results (fills as scannerData callbacks arrive)
static std::vector<core::ScanResult> g_pendingScanResults;

// tickerId → symbol mapping (for routing tick data to windows)
static std::unordered_map<int, std::string> g_tickerSymbols;

static int g_nextOrderId = 1;

// Request IDs (reserved ranges)
static constexpr int HIST_REQID   = 1;    // historical data for the chart
static constexpr int DEPTH_REQID  = 200;  // market depth
static constexpr int SCAN_REQID   = 300;  // scanner
static constexpr int MKT_REQID_BASE = 100; // market data per-symbol offset

// ============================================================================
// Connection / Login state
// ============================================================================
enum class ConnectionState { Disconnected, Connecting, Connected, Error };
enum class ApiType          { TWS = 0, Gateway };

struct LoginState {
    char    host[128]  = "127.0.0.1";
    int     port       = 7497;
    int     clientId   = 1;
    bool    isLive     = false;
    ApiType apiType    = ApiType::TWS;

    ConnectionState state    = ConnectionState::Disconnected;
    std::string     errorMsg;
    std::string     connectedAs;

    void UpdatePort() {
        if (apiType == ApiType::TWS)
            port = isLive ? 7496 : 7497;
        else
            port = isLive ? 4001 : 4002;
    }
};

static LoginState  g_Login;
static GLFWwindow* g_AppWindow = nullptr;

// ============================================================================
// Vulkan globals
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
// Helpers
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
        if (strcmp(p.extensionName, ext) == 0) return true;
    return false;
}

// ============================================================================
// Vulkan setup / teardown
// ============================================================================
static void SetupVulkan(ImVector<const char*> instance_extensions) {
    VkResult err;
    {
        VkInstanceCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t cnt = 0;
        ImVector<VkExtensionProperties> props;
        vkEnumerateInstanceExtensionProperties(nullptr, &cnt, nullptr);
        props.resize((int)cnt);
        vkEnumerateInstanceExtensionProperties(nullptr, &cnt, props.Data);

        if (IsExtensionAvailable(props, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(props, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif
        ci.enabledExtensionCount   = (uint32_t)instance_extensions.Size;
        ci.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&ci, g_Allocator, &g_Instance);
        check_vk_result(err);
    }

    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    {
        ImVector<const char*> dev_exts;
        dev_exts.push_back("VK_KHR_swapchain");
        uint32_t cnt2 = 0;
        ImVector<VkExtensionProperties> props2;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &cnt2, nullptr);
        props2.resize((int)cnt2);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &cnt2, props2.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(props2, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            dev_exts.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
        const float prio[] = {1.0f};
        VkDeviceQueueCreateInfo qi[1] = {};
        qi[0].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi[0].queueFamilyIndex = g_QueueFamily;
        qi[0].queueCount       = 1;
        qi[0].pQueuePriorities = prio;
        VkDeviceCreateInfo dci = {};
        dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount    = IM_ARRAYSIZE(qi);
        dci.pQueueCreateInfos       = qi;
        dci.enabledExtensionCount   = (uint32_t)dev_exts.Size;
        dci.ppEnabledExtensionNames = dev_exts.Data;
        err = vkCreateDevice(g_PhysicalDevice, &dci, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    {
        VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE},
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
        g_PhysicalDevice, surface, fmts, IM_ARRAYSIZE(fmts),
        VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    VkPresentModeKHR pm[] = {VK_PRESENT_MODE_FIFO_KHR};
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice, surface, pm, IM_ARRAYSIZE(pm));
    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily,
        g_Allocator, w, h, g_MinImageCount, 0);
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
    err = vkEndCommandBuffer(fd->CommandBuffer);   check_vk_result(err);
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
// Window lifecycle helpers
// ============================================================================
static void CreateTradingWindows() {
    delete g_ChartWindow;    g_ChartWindow    = new ui::ChartWindow();
    delete g_NewsWindow;     g_NewsWindow     = new ui::NewsWindow();
    delete g_TradingWindow;  g_TradingWindow  = new ui::TradingWindow();
    delete g_ScannerWindow;  g_ScannerWindow  = new ui::ScannerWindow();
    delete g_PortfolioWindow; g_PortfolioWindow = new ui::PortfolioWindow();

    // Scanner row double-click → switch chart symbol + request new data
    g_ScannerWindow->OnSymbolSelected = [](const std::string& sym) {
        if (g_ChartWindow) g_ChartWindow->SetSymbol(sym);
        if (g_IBClient) {
            g_IBClient->CancelHistoricalData(HIST_REQID);
            g_IBClient->CancelMarketData(MKT_REQID_BASE);
            g_IBClient->CancelMktDepth(DEPTH_REQID);
            g_pendingBars = core::BarSeries{};
            g_pendingBars.symbol    = sym;
            g_pendingBars.timeframe = core::Timeframe::D1;
            g_IBClient->ReqHistoricalData(HIST_REQID, sym);
            g_tickerSymbols[MKT_REQID_BASE] = sym;
            g_IBClient->ReqMarketData(MKT_REQID_BASE, sym);
            g_IBClient->ReqMktDepth(DEPTH_REQID, sym);
        }
    };

    // Wire order submission / cancellation to IB
    g_TradingWindow->SetNextOrderId(g_nextOrderId);
    g_TradingWindow->OnOrderSubmit = [](int orderId, const std::string& sym,
                                        const std::string& action,
                                        const std::string& orderType,
                                        double qty, double limitPrice) {
        if (g_IBClient)
            g_IBClient->PlaceOrder(orderId, sym, action, orderType, qty, limitPrice);
    };
    g_TradingWindow->OnOrderCancel = [](int orderId) {
        if (g_IBClient) g_IBClient->CancelOrder(orderId);
    };

    // Wire scanner to IB
    g_ScannerWindow->OnScanRequest = [](const std::string& scanCode,
                                        const std::string& instrument,
                                        const std::string& location) {
        if (g_IBClient)
            g_IBClient->ReqScannerData(SCAN_REQID, scanCode, instrument, location);
    };
}

static void DestroyTradingWindows() {
    delete g_ChartWindow;    g_ChartWindow    = nullptr;
    delete g_NewsWindow;     g_NewsWindow     = nullptr;
    delete g_TradingWindow;  g_TradingWindow  = nullptr;
    delete g_ScannerWindow;  g_ScannerWindow  = nullptr;
    delete g_PortfolioWindow; g_PortfolioWindow = nullptr;
}

// ============================================================================
// IB API connection wiring
// ============================================================================
static void WireIBCallbacks() {
    // ── Connection state ──────────────────────────────────────────────────
    g_IBClient->onConnectionChanged = [](bool connected, const std::string& info) {
        if (connected) {
            g_Login.state       = ConnectionState::Connected;
            g_Login.connectedAs = g_Login.isLive ? "[LIVE]" : "[PAPER]";
            printf("[IB] %s\n", info.c_str());

            CreateTradingWindows();

            // Set market data type before any subscriptions.
            // Live:  type 1 (real-time, requires active data subscriptions).
            // Paper: type 3 (delayed 15-20 min, free on all paper accounts).
            g_IBClient->ReqMarketDataType(g_Login.isLive ? 1 : 3);

            // Start subscriptions
            g_IBClient->ReqAccountUpdates(true);
            g_IBClient->ReqPositions();
            g_IBClient->ReqScannerData(SCAN_REQID);

            // Chart — default AAPL
            const std::string sym = "AAPL";
            g_pendingBars.symbol    = sym;
            g_pendingBars.timeframe = core::Timeframe::D1;
            g_tickerSymbols[MKT_REQID_BASE] = sym;
            g_IBClient->ReqHistoricalData(HIST_REQID, sym);
            g_IBClient->ReqMarketData(MKT_REQID_BASE, sym);
            g_IBClient->ReqMktDepth(DEPTH_REQID, sym);
        } else {
            if (g_Login.state == ConnectionState::Connecting) {
                g_Login.state    = ConnectionState::Error;
                g_Login.errorMsg = info;
            } else {
                g_Login.state = ConnectionState::Disconnected;
                DestroyTradingWindows();
            }
            printf("[IB] Disconnected: %s\n", info.c_str());
        }
    };

    // ── Historical bars (chart) ───────────────────────────────────────────
    g_IBClient->onBarData = [](int /*reqId*/, const core::Bar& bar, bool done) {
        if (!done) {
            g_pendingBars.bars.push_back(bar);
        } else if (g_ChartWindow) {
            g_ChartWindow->SetHistoricalData(g_pendingBars);
            g_pendingBars.bars.clear();
        }
    };

    // ── Market data ticks ─────────────────────────────────────────────────
    g_IBClient->onTickPrice = [](int tickerId, int field, double price) {
        auto it = g_tickerSymbols.find(tickerId);
        if (it == g_tickerSymbols.end()) return;
        const std::string& sym = it->second;
        // field 4 = LAST
        if (field == 4 && g_TradingWindow) {
            g_TradingWindow->OnTick(price, 1.0, true);
            g_TradingWindow->SetSymbol(sym, price);
        }
        // Update scanner quote
        if (field == 4 && g_ScannerWindow)
            g_ScannerWindow->OnQuoteUpdate(sym, price, 0.0, 0.0, 0.0);
    };

    g_IBClient->onTickSize = [](int /*tickerId*/, int /*field*/, double /*size*/) {
        // Could feed volume/size data here
    };

    // ── Account values ────────────────────────────────────────────────────
    g_IBClient->onAccountValue = [](const std::string& key, const std::string& val,
                                    const std::string& currency,
                                    const std::string& acct) {
        if (g_PortfolioWindow)
            g_PortfolioWindow->OnAccountValue(key, val, currency, acct);
    };

    // ── Positions ─────────────────────────────────────────────────────────
    g_IBClient->onPositionData = [](const core::Position& pos, bool done) {
        if (g_PortfolioWindow) {
            if (!done)
                g_PortfolioWindow->OnPositionUpdate(pos);
            else
                g_PortfolioWindow->OnAccountEnd();
        }
        // Keep scanner aware of held positions
        if (!done && g_ScannerWindow) {
            static std::vector<std::string> held;
            held.push_back(pos.symbol);
            g_ScannerWindow->SetPortfolioSymbols(held);
        }
    };

    // ── Portfolio updates (P&L etc.) ──────────────────────────────────────
    g_IBClient->onPortfolioUpdate = [](const core::Position& pos) {
        if (g_PortfolioWindow) g_PortfolioWindow->OnPositionUpdate(pos);
    };

    // ── Order status ──────────────────────────────────────────────────────
    g_IBClient->onOrderStatusChanged = [](int orderId, core::OrderStatus status,
                                          double filled, double avgPrice) {
        if (g_TradingWindow)
            g_TradingWindow->OnOrderStatus(orderId, status, filled, avgPrice);
    };

    // ── Fills ─────────────────────────────────────────────────────────────
    g_IBClient->onFillReceived = [](const core::Fill& fill) {
        if (g_TradingWindow) g_TradingWindow->OnFill(fill);
        if (g_PortfolioWindow) {
            core::TradeRecord tr;
            tr.tradeId   = fill.orderId;
            tr.symbol    = fill.symbol;
            tr.side      = fill.side == core::OrderSide::Buy ? "BUY" : "SELL";
            tr.quantity  = fill.quantity;
            tr.price     = fill.price;
            tr.commission = fill.commission;
            g_PortfolioWindow->OnTradeExecuted(tr);
        }
    };

    // ── Market depth ──────────────────────────────────────────────────────
    g_IBClient->onDepthUpdate = [](int /*id*/, bool isBid, int pos, int /*op*/,
                                   double price, double size) {
        if (g_TradingWindow)
            g_TradingWindow->OnDepthUpdate(isBid, pos, price, size);
    };

    // ── Scanner ───────────────────────────────────────────────────────────
    g_IBClient->onScanItem = [](int reqId, const core::ScanResult& result) {
        if (reqId == SCAN_REQID) g_pendingScanResults.push_back(result);
    };
    g_IBClient->onScanEnd = [](int reqId) {
        if (reqId == SCAN_REQID && g_ScannerWindow && !g_pendingScanResults.empty()) {
            g_ScannerWindow->OnScanData(reqId, g_pendingScanResults);
            g_pendingScanResults.clear();
        }
    };

    // ── News ──────────────────────────────────────────────────────────────
    g_IBClient->onNewsItem = [](std::time_t ts, const std::string& provider,
                                const std::string& articleId,
                                const std::string& headline) {
        if (!g_NewsWindow) return;
        core::NewsItem item;
        item.id        = static_cast<int>(ts);
        item.headline  = headline;
        item.source    = provider;
        item.summary   = articleId;
        item.timestamp = ts;
        item.sentiment = core::NewsSentiment::Neutral;
        item.category  = core::NewsCategory::Market;
        g_NewsWindow->OnMarketNewsItem(item);
    };

    // ── Errors ────────────────────────────────────────────────────────────
    g_IBClient->onError = [](int code, const std::string& msg) {
        fprintf(stderr, "[IB Error %d] %s\n", code, msg.c_str());
        // Surface critical errors in the UI
        if (code == 162 || code == 200 || code == 321) {
            // Ignore common benign errors (no data/historical, invalid contract)
        }
    };

    // ── Next valid order id ───────────────────────────────────────────────
    g_IBClient->onNextValidId = [](int id) {
        g_nextOrderId = id;
        printf("[IB] Next valid order ID: %d\n", id);
    };
}

// ============================================================================
// Connect / Disconnect
// ============================================================================
static void StartConnect() {
    g_Login.state    = ConnectionState::Connecting;
    g_Login.errorMsg.clear();

    delete g_IBClient;
    g_IBClient = new core::services::IBKRClient();
    WireIBCallbacks();

    printf("[IB] Connecting to %s:%d  clientId=%d  account=%s\n",
           g_Login.host, g_Login.port, g_Login.clientId,
           g_Login.isLive ? "LIVE" : "PAPER");

    bool ok = g_IBClient->Connect(g_Login.host, g_Login.port, g_Login.clientId);
    if (!ok) {
        g_Login.state    = ConnectionState::Error;
        g_Login.errorMsg = std::string("Cannot reach ") + g_Login.host +
                           ":" + std::to_string(g_Login.port) +
                           " — is IB Gateway / TWS running?";
        delete g_IBClient;
        g_IBClient = nullptr;
    }
}

static void Disconnect() {
    if (g_IBClient) {
        g_IBClient->ReqAccountUpdates(false);
        g_IBClient->Disconnect();
        delete g_IBClient;
        g_IBClient = nullptr;
    }
    g_Login.state       = ConnectionState::Disconnected;
    g_Login.connectedAs.clear();
    g_Login.errorMsg.clear();
    g_pendingBars       = core::BarSeries{};
    g_pendingScanResults.clear();
    g_tickerSymbols.clear();
    DestroyTradingWindows();
    printf("[IB] Disconnected.\n");
}

// ============================================================================
// Login Window
// ============================================================================
static void RenderLoginWindow() {
    // Dark overlay
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

    const ImVec2 loginSize = {460.0f, 340.0f};
    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(loginSize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);

    ImGuiWindowFlags dlgFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(8.0f, 10.0f));

    if (ImGui::Begin("Interactive Brokers Login", nullptr, dlgFlags)) {

        // Header
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.1f, 1.0f));
        float tw = ImGui::CalcTextSize("Interactive Brokers").x;
        ImGui::SetCursorPosX((loginSize.x - tw) * 0.5f);
        ImGui::Text("Interactive Brokers");
        ImGui::PopStyleColor();
        float sw = ImGui::CalcTextSize("Trading Application").x;
        ImGui::SetCursorPosX((loginSize.x - sw) * 0.5f);
        ImGui::TextDisabled("Trading Application");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool isConnecting = (g_Login.state == ConnectionState::Connecting);
        if (isConnecting) ImGui::BeginDisabled();

        // Account type
        ImGui::Text("Account Type");
        ImGui::SameLine(140);
        bool changedType = false;
        if (ImGui::RadioButton("Paper", !g_Login.isLive)) { g_Login.isLive = false; changedType = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Live",   g_Login.isLive)) { g_Login.isLive = true;  changedType = true; }

        // API type
        ImGui::Text("API");
        ImGui::SameLine(140);
        int apiIdx = (int)g_Login.apiType;
        if (ImGui::RadioButton("TWS",        &apiIdx, 0)) { g_Login.apiType = ApiType::TWS;     changedType = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("IB Gateway", &apiIdx, 1)) { g_Login.apiType = ApiType::Gateway; changedType = true; }
        if (changedType) g_Login.UpdatePort();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Connection parameters
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

        // Info note
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextWrapped("Ensure IB Gateway or TWS is running with API enabled on the port above. "
                           "Username / password are handled by TWS/Gateway.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Error / status
        if (g_Login.state == ConnectionState::Error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("Error: %s", g_Login.errorMsg.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        if (isConnecting) {
            using namespace std::chrono;
            static auto startTime = steady_clock::now();
            int dots = (int)(duration_cast<milliseconds>(
                steady_clock::now() - startTime).count() / 400) % 4;
            char buf[32];
            snprintf(buf, sizeof(buf), "Connecting%.*s", dots, "...");
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
            float bw = ImGui::CalcTextSize(buf).x;
            ImGui::SetCursorPosX((loginSize.x - bw) * 0.5f);
            ImGui::Text("%s", buf);
            ImGui::PopStyleColor();
        }

        // Connect button
        if (!isConnecting) {
            ImVec4 btnColor = g_Login.isLive ? ImVec4(0.7f, 0.1f, 0.1f, 1.0f)
                                             : ImVec4(0.1f, 0.5f, 0.2f, 1.0f);
            ImVec4 btnHover = g_Login.isLive ? ImVec4(0.9f, 0.2f, 0.2f, 1.0f)
                                             : ImVec4(0.2f, 0.7f, 0.3f, 1.0f);
            ImVec4 btnAct   = g_Login.isLive ? ImVec4(0.5f, 0.05f, 0.05f, 1.0f)
                                             : ImVec4(0.08f, 0.35f, 0.15f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button,        btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  btnAct);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            const char* lbl = g_Login.isLive ? "Connect to Live Account"
                                             : "Connect to Paper Account";
            float btnW = loginSize.x - 48.0f;
            if (ImGui::Button(lbl, ImVec2(btnW, 32.0f)))
                StartConnect();

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();

            if (g_Login.isLive) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));
                ImGui::TextWrapped("WARNING: Live account — real orders will be executed.");
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // Poll IB messages while connecting (connection is async)
    if (g_Login.state == ConnectionState::Connecting && g_IBClient)
        g_IBClient->ProcessMessages();
}

// ============================================================================
// Trading UI (post-login)
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
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Disconnect")) Disconnect();
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(g_AppWindow, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Windows")) {
                ImGui::MenuItem("Chart",      nullptr, nullptr, false);
                ImGui::MenuItem("Order Book", nullptr, nullptr, false);
                ImGui::MenuItem("Portfolio",  nullptr, nullptr, false);
                ImGui::MenuItem("News",       nullptr, nullptr, false);
                ImGui::MenuItem("Scanner",    nullptr, nullptr, false);
                ImGui::EndMenu();
            }

            // Status indicator
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

        ImGuiID dockId = ImGui::GetID("TradingDock");
        ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    }
    ImGui::End();

    if (g_ChartWindow)    g_ChartWindow->Render();
    if (g_NewsWindow)     g_NewsWindow->Render();
    if (g_TradingWindow)  g_TradingWindow->Render();
    if (g_ScannerWindow)  g_ScannerWindow->Render();
    if (g_PortfolioWindow) g_PortfolioWindow->Render();
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
// main
// ============================================================================
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    std::cout << "============================================\n"
              << "Interactive Brokers Trading Application\n"
              << "Version: 1.0.0   Build: " << __DATE__ << " " << __TIME__ << "\n"
              << "============================================\n";

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) { std::cerr << "Failed to initialise GLFW\n"; return 1; }
    if (!glfwVulkanSupported()) {
        std::cerr << "Vulkan not supported\n"; glfwTerminate(); return 1;
    }

    ImVector<const char*> extensions;
    uint32_t ext_count = 0;
    const char** glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    for (uint32_t i = 0; i < ext_count; i++) extensions.push_back(glfw_exts[i]);
    SetupVulkan(extensions);

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    g_AppWindow = glfwCreateWindow(1920, 1080, "IBKR Trading App", nullptr, nullptr);
    if (!g_AppWindow) {
        std::cerr << "Failed to create window\n";
        CleanupVulkan(); glfwTerminate(); return 1;
    }

    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, g_AppWindow, g_Allocator, &surface);
    check_vk_result(err);

    int fb_w, fb_h;
    glfwGetFramebufferSize(g_AppWindow, &fb_w, &fb_h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, fb_w, fb_h);

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

    g_Login.UpdatePort();

    ImVec4 clear_color = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    printf("Application running. Close window to exit.\n");

    // ── Main loop ──────────────────────────────────────────────────────────
    while (!glfwWindowShouldClose(g_AppWindow)) {
        glfwPollEvents();

        // Drain IB message queue each frame (when connected)
        if (g_IBClient) g_IBClient->ProcessMessages();

        int cur_w, cur_h;
        glfwGetFramebufferSize(g_AppWindow, &cur_w, &cur_h);
        if (cur_w > 0 && cur_h > 0 &&
            (g_SwapChainRebuild || wd->Width != cur_w || wd->Height != cur_h)) {
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
    if (g_IBClient) {
        g_IBClient->Disconnect();
        delete g_IBClient;
        g_IBClient = nullptr;
    }
    DestroyTradingWindows();

    err = vkDeviceWaitIdle(g_Device);
    check_vk_result(err);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    CleanupVulkanWindow(wd);
    CleanupVulkan();

    glfwDestroyWindow(g_AppWindow);
    glfwTerminate();

    std::cout << "Application terminated successfully.\n";
    return 0;
}
