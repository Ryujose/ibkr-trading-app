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
#include "ui/windows/OrdersWindow.h"

#include "core/services/IBKRClient.h"
#include "core/models/WindowGroup.h"

// ============================================================================
// Multi-instance window entry structs
// ============================================================================
struct ChartEntry {
    ui::ChartWindow* win         = nullptr;
    int              histId      = 0;   // reqHistoricalData id
    int              extId       = 0;   // extend-history (pan-left) id
    int              mktId       = 0;   // reqMarketData id (chart ticks)
    core::BarSeries  pendingBars;
    core::BarSeries  pendingExtBars;
};

struct TradingEntry {
    ui::TradingWindow* win          = nullptr;
    int                depthId      = 0;   // reqMktDepth id
    int                mktId        = 0;   // reqMarketData id (NBBO ticks)
    double nbboBid = 0, nbboBidSz = 0;
    double nbboAsk = 0, nbboAskSz = 0;
    double lastTickPrice = 0, lastTickSize = 0;
};

struct ScannerEntry {
    ui::ScannerWindow* win           = nullptr;
    int                scanBase      = 0;   // reqId pool base (scanBase..scanBase+99)
    int                activeScanId  = 0;
    bool               subActive     = false;
    int                mktBase       = 0;   // market-data base for live quotes
    static constexpr int kMktSlots   = 12;
    std::vector<core::ScanResult> pendingResults;
};

static constexpr int kMaxMultiWin = 4;   // max instances per window type

// ---- Multi-instance containers -----------------------------------------------
static std::vector<ChartEntry>   g_chartEntries;
static std::vector<TradingEntry> g_tradingEntries;
static std::vector<ScannerEntry> g_scannerEntries;

// ---- Singleton windows (one each) --------------------------------------------
static ui::NewsWindow*      g_NewsWindow      = nullptr;
static ui::PortfolioWindow* g_PortfolioWindow = nullptr;
static ui::OrdersWindow*    g_OrdersWindow    = nullptr;

// IB API client (created on Connect, deleted on Disconnect)
static core::services::IBKRClient* g_IBClient = nullptr;

// Previous-close prices keyed by symbol, used to compute change/% for scanner rows.
static std::unordered_map<std::string, double> g_scannerPrevClose;
// Day volume keyed by symbol for scanner rows.
static std::unordered_map<std::string, double> g_scannerVolume;

// tickerId → symbol mapping (for routing tick data to windows)
static std::unordered_map<int, std::string> g_tickerSymbols;

static int    g_nextOrderId          = 1;
static double g_reconnectNextAttempt = 0.0;   // glfwGetTime() of next auto-reconnect try
static constexpr double kReconnectIntervalSec = 5.0;

// ---- Window groups (4 slots; index 0 = group id 1) -------------------------
static std::array<core::GroupState, 4> g_groups;
// Guard against re-entrant group broadcasts when SetSymbol() re-fires callbacks.
static bool g_groupSyncInProgress = false;

// Live orders for chart overlay (orderId → Order; refreshed on every status change)
static std::unordered_map<int, core::Order> g_liveOrders;

// Per-symbol positions and commissions for the chart P&L strip
static std::unordered_map<std::string, core::Position> g_positions;
static std::unordered_map<std::string, double>          g_symbolCommissions;

static bool IsTerminalOrderStatus(core::OrderStatus s) {
    return s == core::OrderStatus::Filled   ||
           s == core::OrderStatus::Cancelled ||
           s == core::OrderStatus::Rejected;
}

// Push pending order lines for a specific chart window (matched by symbol)
static void UpdateChartPendingOrders(ui::ChartWindow* win) {
    if (!win) return;
    const std::string sym = win->getSymbol();
    std::vector<ui::ChartWindow::PendingOrderLine> lines;
    for (const auto& [id, o] : g_liveOrders) {
        if (o.symbol != sym) continue;
        if (IsTerminalOrderStatus(o.status)) continue;
        ui::ChartWindow::PendingOrderLine ln;
        ln.orderId = id;
        ln.isBuy   = (o.side == core::OrderSide::Buy);
        ln.qty     = o.quantity;
        if (o.type == core::OrderType::StopLimit) {
            ln.price     = o.stopPrice;
            ln.auxPrice  = o.limitPrice;
            ln.orderType = "STP LMT";
        } else if (o.type == core::OrderType::Stop) {
            ln.price     = o.stopPrice;
            ln.orderType = "STP";
        } else if (o.type == core::OrderType::Limit) {
            ln.price     = o.limitPrice;
            ln.orderType = "LMT";
        } else {
            continue;
        }
        if (ln.price <= 0.0) continue;
        lines.push_back(ln);
    }
    win->SetPendingOrders(lines);
}

static void UpdateAllChartPendingOrders() {
    for (auto& e : g_chartEntries) UpdateChartPendingOrders(e.win);
}

// Push position info for a specific chart window (matched by symbol)
static void UpdateChartPosition(ui::ChartWindow* win) {
    if (!win) return;
    const std::string sym = win->getSymbol();
    ui::ChartWindow::PositionInfo info;
    auto it = g_positions.find(sym);
    if (it != g_positions.end() && std::abs(it->second.quantity) > 1e-9) {
        info.hasPosition = true;
        info.qty         = it->second.quantity;
        info.avgCost     = it->second.avgCost;
        info.lastPrice   = it->second.marketPrice;
        info.unrealPnL   = it->second.unrealizedPnL;
        auto cit = g_symbolCommissions.find(sym);
        info.commission  = (cit != g_symbolCommissions.end()) ? cit->second : 0.0;
    }
    win->SetPosition(info);
}

static void UpdateAllChartPositions() {
    for (auto& e : g_chartEntries) UpdateChartPosition(e.win);
}
static int g_newsItemId    = 10000;  // unique IDs for real-time market news items
static int g_histNewsId    = 20000;  // unique IDs for historical news items
static std::vector<std::string> g_portfolioSymbols;  // known held symbols

// Request IDs (reserved ranges — no overlaps)
// Per-instance helpers: each window type has its own slot.
//   Chart   hist:  1,3,5,7       ext: 2,4,6,8     mkt: 100-103
//   Trading mkt:   110-113       depth: 120-123
//   Scanner scan:  1000,1100,1200,1300 (+99 ea)    mkt: 800,812,824,836 (+12 ea)
//   News:          201(RT), 400-420(conId), 500-520(hist), 600-699(art), 700-759(mkt)
//   Account:       900
static constexpr int NEWS_RT_REQID       = 201;  // real-time news subscription (mdoff;292)

// Inline helpers — idx is 0-based instance index
inline int ChartHistId   (int idx) { return 1    + idx * 2; }   // 1,3,5,7
inline int ChartExtId    (int idx) { return 2    + idx * 2; }   // 2,4,6,8
inline int ChartMktId    (int idx) { return 100  + idx; }       // 100-103
inline int TradingMktId  (int idx) { return 110  + idx; }       // 110-113
inline int TradingDepthId(int idx) { return 120  + idx; }       // 120-123
inline int ScannerBase   (int idx) { return 1000 + idx * 100; } // 1000,1100,1200,1300
inline int ScannerMktBase(int idx) { return 800  + idx * 12; }  // 800,812,824,836

static constexpr int ACCT_SUMMARY_REQID  = 900;  // reqAccountSummary for base currency

// News reqId layout:
//   400        — contract-details lookup for stock tab
//   401..420   — contract-details lookup for portfolio symbols (up to 20)
//   500        — historical news for stock tab
//   501..520   — historical news for portfolio symbols
//   600..699   — article body fetches (rolling counter)
//   750..759   — contract-details lookup for Market-tab seed symbols (up to 10)
//   700..709   — historical news for Market tab (seeded on connection)
static constexpr int NEWS_CONID_STOCK    = 400;
static constexpr int NEWS_CONID_PORT     = 401;  // base; +i per portfolio symbol
static constexpr int NEWS_HIST_STOCK     = 500;
static constexpr int NEWS_HIST_PORT      = 501;  // base; +i per portfolio symbol
static constexpr int NEWS_ART_BASE       = 600;
static constexpr int NEWS_ART_END        = 699;
static constexpr int NEWS_HIST_MKT       = 700;  // base; +i per seed symbol
static constexpr int NEWS_CONID_MKT      = 750;  // base; +i per seed symbol

// Symbols fetched on connection to seed the Market-tab news feed.
static const char* kMktSeedSymbols[] = { "AAPL", "SPY", "MSFT", "TSLA", "NVDA" };
static constexpr int kMktSeedCount   = 5;
static int           g_nextArtReqId      = NEWS_ART_BASE;
static std::unordered_map<int,int> g_artReqToItemId;  // reqId → NewsItem.id
// Tracks which contract-detail reqIds have already triggered reqHistoricalNews,
// preventing duplicate calls when IB returns multiple exchange matches per symbol.
static std::unordered_map<int,bool> g_newsConIdFired;

// ============================================================================
// Connection / Login state
// ============================================================================
enum class ConnectionState { Disconnected, Connecting, Connected, LostConnection, Error };
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

// Returns the generic tick list appropriate for the current account type.
// Paper/delayed (type 4): gateway rejects ALL generic ticks → use "".
// Live (type 1): use "165" for 52-week hi/lo (fields 79/80 from Misc Stats).
static const char* MktDataTicks() {
    return g_Login.isLive ? "165" : "";
}

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
// Group sync helper — must be defined before CreateTradingWindows lambdas
// ============================================================================

// Update a trading entry's displayed symbol AND re-subscribe IB market data + depth.
// Used both from BroadcastGroupSymbol and from OnSymbolChanged so the logic is in one place.
static void ApplyTradingSymbol(TradingEntry& te, const std::string& sym) {
    if (te.win) te.win->SetSymbol(sym, 0.0);
    if (!g_IBClient) return;
    g_IBClient->CancelMarketData(te.mktId);
    g_IBClient->CancelMktDepth(te.depthId);
    g_tickerSymbols[te.mktId] = sym;
    g_IBClient->ReqMarketData(te.mktId, sym, MktDataTicks());
    g_IBClient->ReqMktDepth(te.depthId, sym, 20);
}

// Propagate a symbol change to all windows in the same group.
// Re-entrant guard prevents loops when SetSymbol() re-fires callbacks.
static void BroadcastGroupSymbol(int groupId, const std::string& sym) {
    if (groupId <= 0 || g_groupSyncInProgress) return;
    core::GroupState& gs = g_groups[groupId - 1];
    if (gs.symbol == sym) return;   // already broadcast this symbol
    g_groupSyncInProgress = true;
    gs.id     = groupId;
    gs.symbol = sym;
    for (auto& e : g_chartEntries)
        if (e.win && e.win->groupId() == groupId) e.win->SetSymbol(sym);
    for (auto& te : g_tradingEntries)
        if (te.win && te.win->groupId() == groupId) ApplyTradingSymbol(te, sym);
    if (g_NewsWindow && g_NewsWindow->groupId() == groupId) g_NewsWindow->SetSymbol(sym);
    // ScannerWindow is a symbol source only — no inbound SetSymbol
    g_groupSyncInProgress = false;
}

// ============================================================================
// Window lifecycle helpers — per-instance spawn functions
// ============================================================================

// Issue / re-issue a historical + market-data subscription for a chart instance.
static void ReqChartData(int histId, int mktId,
                         const std::string& sym, core::Timeframe tf, bool useRTH,
                         core::BarSeries& pendingBars) {
    if (!g_IBClient) return;
    g_IBClient->CancelHistoricalData(histId);
    pendingBars         = core::BarSeries{};
    pendingBars.symbol  = sym;
    pendingBars.timeframe = tf;
    g_IBClient->ReqHistoricalData(histId, sym,
                                  core::TimeframeIBDuration(tf),
                                  core::TimeframeIBBarSize(tf),
                                  useRTH);
    if (g_tickerSymbols[mktId] != sym) {
        g_IBClient->CancelMarketData(mktId);
        g_tickerSymbols[mktId] = sym;
        g_IBClient->ReqMarketData(mktId, sym, MktDataTicks());
    }
}

static void SpawnChartWindow(int idx) {
    ChartEntry e;
    e.histId = ChartHistId(idx);
    e.extId  = ChartExtId(idx);
    e.mktId  = ChartMktId(idx);
    e.win    = new ui::ChartWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId(idx + 1);

    // Capture idx (not pointer — vector may reallocate)
    e.win->OnDataRequest = [idx](const std::string& sym, core::Timeframe tf, bool useRTH) {
        auto& ce = g_chartEntries[idx];
        ReqChartData(ce.histId, ce.mktId, sym, tf, useRTH, ce.pendingBars);
        UpdateChartPendingOrders(ce.win);
        UpdateChartPosition(ce.win);
        BroadcastGroupSymbol(ce.win->groupId(), sym);
    };

    e.win->OnExtendHistory = [idx](const std::string& sym, core::Timeframe tf,
                                   const std::string& endDT, bool useRTH) {
        auto& ce = g_chartEntries[idx];
        if (!g_IBClient) { ce.win->PrependHistoricalData({}); return; }
        g_IBClient->CancelHistoricalData(ce.extId);
        ce.pendingExtBars           = core::BarSeries{};
        ce.pendingExtBars.symbol    = sym;
        ce.pendingExtBars.timeframe = tf;
        g_IBClient->ReqHistoricalData(ce.extId, sym,
                                      core::TimeframeIBDuration(tf),
                                      core::TimeframeIBBarSize(tf),
                                      useRTH, endDT);
    };

    e.win->OnOrderSubmit = [](const std::string& sym, const std::string& side,
                              const std::string& orderType, double qty, double price,
                              const std::string& tif, bool outsideRth, double auxPrice) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        int id = g_nextOrderId++;
        for (auto& te : g_tradingEntries)
            if (te.win) te.win->SetNextOrderId(g_nextOrderId);
        core::Order pending;
        pending.orderId     = id;
        pending.symbol      = sym;
        pending.side        = (side == "BUY") ? core::OrderSide::Buy : core::OrderSide::Sell;
        pending.quantity    = qty;
        pending.status      = core::OrderStatus::Pending;
        pending.submittedAt = std::time(nullptr);
        pending.updatedAt   = pending.submittedAt;
        if (orderType == "LMT" || orderType == "LIT" || orderType == "TRAIL LIMIT")
            pending.type = core::OrderType::Limit;
        else if (orderType == "STP")
            pending.type = core::OrderType::Stop;
        else if (orderType == "STP LMT")
            pending.type = core::OrderType::StopLimit;
        else
            pending.type = core::OrderType::Market;
        if (orderType == "LMT" || orderType == "LIT" || orderType == "TRAIL LIMIT")
            pending.limitPrice = price;
        if (orderType == "STP" || orderType == "STP LMT" || orderType == "MIT")
            pending.stopPrice = price;
        g_liveOrders[id] = pending;
        if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(pending);
        UpdateAllChartPendingOrders();
        g_IBClient->PlaceOrder(id, sym, side, orderType, qty, price, tif, outsideRth, auxPrice);
    };

    e.win->OnCancelOrder = [](int orderId) {
        if (g_IBClient) g_IBClient->CancelOrder(orderId);
    };

    e.win->OnModifyOrder = [](int orderId, double newPrice, double newAuxPrice) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        auto it = g_liveOrders.find(orderId);
        if (it == g_liveOrders.end()) return;
        const core::Order old = it->second;
        g_IBClient->CancelOrder(orderId);
        g_liveOrders.erase(it);
        int newId = g_nextOrderId++;
        for (auto& te : g_tradingEntries)
            if (te.win) te.win->SetNextOrderId(g_nextOrderId);
        core::Order rep       = old;
        rep.orderId           = newId;
        rep.status            = core::OrderStatus::Pending;
        rep.submittedAt       = std::time(nullptr);
        rep.updatedAt         = rep.submittedAt;
        rep.filledQty         = 0.0;
        rep.avgFillPrice      = 0.0;
        double mainPrice = newPrice, auxPrice = 0.0;
        if (old.type == core::OrderType::Limit)      { rep.limitPrice = newPrice; rep.stopPrice = 0.0; }
        else if (old.type == core::OrderType::Stop)  { rep.stopPrice = newPrice; rep.limitPrice = 0.0; }
        else if (old.type == core::OrderType::StopLimit) {
            rep.stopPrice = newPrice; rep.limitPrice = newAuxPrice;
            mainPrice = newPrice; auxPrice = newAuxPrice;
        }
        g_liveOrders[newId] = rep;
        if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(rep);
        UpdateAllChartPendingOrders();
        g_IBClient->PlaceOrder(newId, old.symbol,
                               core::OrderSideStr(old.side),
                               core::OrderTypeStr(old.type),
                               old.quantity, mainPrice,
                               core::TIFStr(old.tif), false, auxPrice);
    };

    g_chartEntries.push_back(std::move(e));
}

static void SpawnTradingWindow(int idx) {
    TradingEntry e;
    e.depthId = TradingDepthId(idx);
    e.mktId   = TradingMktId(idx);
    e.win     = new ui::TradingWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId(idx + 1);
    e.win->SetNextOrderId(g_nextOrderId);

    e.win->OnOrderSubmit = [](int orderId, const std::string& sym,
                               const std::string& action,
                               const std::string& orderType,
                               double qty, double price, double auxPrice,
                               const std::string& tif, bool outsideRth) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        g_IBClient->PlaceOrder(orderId, sym, action, orderType, qty, price,
                               tif, outsideRth, auxPrice);
        if (orderId >= g_nextOrderId) g_nextOrderId = orderId + 1;
    };

    e.win->OnOrderCancel = [](int orderId) {
        if (g_IBClient) g_IBClient->CancelOrder(orderId);
    };

    e.win->OnSymbolChanged = [idx](const std::string& sym) {
        auto& te = g_tradingEntries[idx];
        ApplyTradingSymbol(te, sym);
        BroadcastGroupSymbol(te.win->groupId(), sym);
    };

    g_tradingEntries.push_back(std::move(e));
}

static void SpawnScannerWindow(int idx) {
    ScannerEntry e;
    e.scanBase     = ScannerBase(idx);
    e.activeScanId = e.scanBase - 1;   // first increment lands on scanBase
    e.mktBase      = ScannerMktBase(idx);
    e.win          = new ui::ScannerWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId(idx + 1);

    e.win->OnSymbolSelected = [idx](const std::string& sym) {
        // Propagate to all charts/trading in the same group, or just broadcast
        BroadcastGroupSymbol(g_scannerEntries[idx].win->groupId(), sym);
        // Also update any chart windows not in a group (default behavior)
        for (auto& ce : g_chartEntries)
            if (ce.win && ce.win->groupId() == 0) {
                ReqChartData(ce.histId, ce.mktId, sym, core::Timeframe::D1, true, ce.pendingBars);
                ce.win->SetSymbol(sym);
                UpdateChartPendingOrders(ce.win);
                UpdateChartPosition(ce.win);
            }
        for (auto& te : g_tradingEntries)
            if (te.win && te.win->groupId() == 0) {
                if (g_IBClient) {
                    g_IBClient->CancelMarketData(te.mktId);
                    g_IBClient->CancelMktDepth(te.depthId);
                    g_tickerSymbols[te.mktId] = sym;
                    g_IBClient->ReqMarketData(te.mktId, sym, MktDataTicks());
                    g_IBClient->ReqMktDepth(te.depthId, sym, 20);
                }
                te.win->SetSymbol(sym, 0.0);
            }
    };

    e.win->OnScanRequest = [idx](const std::string& scanCode,
                                  const std::string& instrument,
                                  const std::string& location) {
        if (!g_IBClient) return;
        auto& se = g_scannerEntries[idx];
        if (se.subActive) {
            g_IBClient->CancelScannerData(se.activeScanId);
            se.subActive = false;
        }
        // Cancel stale market-data slots for this scanner
        for (int i = 0; i < ScannerEntry::kMktSlots; ++i) {
            int rid = se.mktBase + i;
            if (g_tickerSymbols.count(rid)) {
                g_IBClient->CancelMarketData(rid);
                g_tickerSymbols.erase(rid);
            }
        }
        se.pendingResults.clear();
        if (++se.activeScanId >= se.scanBase + 100)
            se.activeScanId = se.scanBase;
        g_IBClient->ReqScannerData(se.activeScanId, scanCode, instrument, location);
        se.subActive = true;
    };

    g_scannerEntries.push_back(std::move(e));
}

static void CreateTradingWindows() {
    // Singleton windows
    delete g_NewsWindow;      g_NewsWindow      = new ui::NewsWindow();
    g_NewsWindow->setGroupId(1);
    delete g_PortfolioWindow; g_PortfolioWindow = new ui::PortfolioWindow();
    delete g_OrdersWindow;    g_OrdersWindow    = new ui::OrdersWindow();

    // Spawn first instance of each multi-window type
    SpawnChartWindow(0);
    SpawnTradingWindow(0);
    SpawnScannerWindow(0);

    // Wire OrdersWindow
    g_OrdersWindow->OnCancelOrder = [](int orderId) {
        if (g_IBClient) g_IBClient->CancelOrder(orderId);
    };
    g_OrdersWindow->OnRefresh = []() {
        if (g_IBClient) g_IBClient->ReqOpenOrders();
    };

    // Wire NewsWindow
    g_NewsWindow->SetStockNewsReqId(NEWS_HIST_STOCK);
    g_NewsWindow->SetPortNewsReqIdBase(NEWS_HIST_PORT);
    g_NewsWindow->SetMktNewsReqIdBase(NEWS_HIST_MKT);

    g_NewsWindow->OnStockNewsRequested = [](const std::string& symbol) {
        if (g_IBClient && g_IBClient->IsConnected())
            g_IBClient->ReqContractDetails(NEWS_CONID_STOCK, symbol);
    };
    g_NewsWindow->OnPortfolioNewsRequested = [](const std::vector<std::string>& syms) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        for (int i = 0; i < (int)syms.size() && i < 20; ++i)
            g_IBClient->ReqContractDetails(NEWS_CONID_PORT + i, syms[i]);
    };
    g_NewsWindow->OnArticleRequested = [](int itemId, const std::string& provider,
                                          const std::string& articleId) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        int reqId = g_nextArtReqId++;
        if (g_nextArtReqId > NEWS_ART_END) g_nextArtReqId = NEWS_ART_BASE;
        g_artReqToItemId[reqId] = itemId;
        g_IBClient->ReqNewsArticle(reqId, provider, articleId);
    };
}

static void DestroyTradingWindows() {
    for (auto& e : g_chartEntries)   { delete e.win; e.win = nullptr; }
    for (auto& e : g_tradingEntries) { delete e.win; e.win = nullptr; }
    for (auto& e : g_scannerEntries) { delete e.win; e.win = nullptr; }
    g_chartEntries.clear();
    g_tradingEntries.clear();
    g_scannerEntries.clear();

    delete g_NewsWindow;      g_NewsWindow      = nullptr;
    delete g_PortfolioWindow; g_PortfolioWindow = nullptr;
    delete g_OrdersWindow;    g_OrdersWindow    = nullptr;

    g_portfolioSymbols.clear();
    g_artReqToItemId.clear();
    g_nextArtReqId = NEWS_ART_BASE;
    g_newsConIdFired.clear();
    g_liveOrders.clear();
    g_positions.clear();
    g_symbolCommissions.clear();
    g_scannerPrevClose.clear();
    g_scannerVolume.clear();
    // Clear all multi-window market-data ticker slots
    for (int i = 0; i < kMaxMultiWin; ++i) {
        g_tickerSymbols.erase(ChartMktId(i));
        g_tickerSymbols.erase(TradingMktId(i));
        for (int s = 0; s < ScannerEntry::kMktSlots; ++s)
            g_tickerSymbols.erase(ScannerMktBase(i) + s);
    }
}

// ============================================================================
// IB API connection wiring
// ============================================================================
static void WireIBCallbacks() {
    // ── Connection state ──────────────────────────────────────────────────
    g_IBClient->onConnectionChanged = [](bool connected, const std::string& info) {
        if (connected) {
            bool isReconnect = (g_Login.state == ConnectionState::LostConnection);
            g_Login.state       = ConnectionState::Connected;
            g_Login.connectedAs = g_Login.isLive ? "[LIVE]" : "[PAPER]";
            printf("[IB] %s\n", info.c_str());

            // Set market data type before any subscriptions.
            // Live:  type 1 (real-time, requires active data subscriptions).
            // Paper: type 4 (delayed-frozen — 15-20 min delayed during RTH, shows
            //        last known price outside RTH so NBBO doesn't go blank after hours).
            g_IBClient->ReqMarketDataType(g_Login.isLive ? 1 : 4);

            if (!isReconnect) {
                // ── First connect: create all windows, load default symbol ────────
                DestroyTradingWindows();
                CreateTradingWindows();

                g_IBClient->ReqAccountUpdates(true);
                g_IBClient->ReqPositions();
                g_IBClient->ReqAccountSummary(ACCT_SUMMARY_REQID, "Currency");
                g_IBClient->ReqOpenOrders();
                for (auto& se : g_scannerEntries)
                    g_IBClient->CancelScannerData(se.activeScanId);

                const std::string sym = "AAPL";
                if (!g_chartEntries.empty()) {
                    auto& ce = g_chartEntries[0];
                    ce.pendingBars.symbol    = sym;
                    ce.pendingBars.timeframe = core::Timeframe::D1;
                    g_tickerSymbols[ce.mktId] = sym;
                    g_IBClient->ReqHistoricalData(ce.histId, sym,
                                                  core::TimeframeIBDuration(core::Timeframe::D1),
                                                  core::TimeframeIBBarSize(core::Timeframe::D1),
                                                  true);
                    g_IBClient->ReqMarketData(ce.mktId, sym, MktDataTicks());
                }
                if (!g_tradingEntries.empty())
                    ApplyTradingSymbol(g_tradingEntries[0], sym);

                g_IBClient->SubscribeToNews(NEWS_RT_REQID);
                for (int i = 0; i < kMktSeedCount; ++i)
                    g_IBClient->ReqContractDetails(NEWS_CONID_MKT + i, kMktSeedSymbols[i]);
            } else {
                // ── Silent reconnect: windows already exist, re-subscribe data ───
                printf("[IB] Reconnected — re-subscribing all windows.\n");
                g_IBClient->ReqAccountUpdates(true);
                g_IBClient->ReqPositions();
                g_IBClient->ReqAccountSummary(ACCT_SUMMARY_REQID, "Currency");
                g_IBClient->ReqOpenOrders();
                // Cancel any stale scanner subs the gateway may have retained.
                for (auto& se : g_scannerEntries)
                    g_IBClient->CancelScannerData(se.activeScanId);

                // Re-subscribe each chart window with its current symbol + timeframe.
                for (auto& ce : g_chartEntries) {
                    if (!ce.win) continue;
                    std::string sym = ce.win->getSymbol();
                    if (sym.empty()) continue;
                    core::Timeframe tf = ce.win->getTimeframe();
                    g_tickerSymbols[ce.mktId] = sym;
                    ce.pendingBars.symbol    = sym;
                    ce.pendingBars.timeframe = tf;
                    g_IBClient->ReqHistoricalData(ce.histId, sym,
                                                  core::TimeframeIBDuration(tf),
                                                  core::TimeframeIBBarSize(tf),
                                                  true);
                    g_IBClient->ReqMarketData(ce.mktId, sym, MktDataTicks());
                }

                // Re-subscribe each trading window with its current symbol.
                for (auto& te : g_tradingEntries) {
                    if (!te.win) continue;
                    std::string sym = te.win->getSymbol();
                    if (!sym.empty()) ApplyTradingSymbol(te, sym);
                }

                g_IBClient->SubscribeToNews(NEWS_RT_REQID);
                for (int i = 0; i < kMktSeedCount; ++i)
                    g_IBClient->ReqContractDetails(NEWS_CONID_MKT + i, kMktSeedSymbols[i]);
            }
        } else {
            // Any disconnect while not in the normal "initial Connecting" flow
            // → keep windows alive, null client, schedule a reconnect attempt.
            if (g_Login.state == ConnectionState::Connecting) {
                g_Login.state    = ConnectionState::Error;
                g_Login.errorMsg = info;
            } else {
                delete g_IBClient;
                g_IBClient                = nullptr;
                g_Login.state             = ConnectionState::LostConnection;
                g_reconnectNextAttempt    = glfwGetTime() + kReconnectIntervalSec;
            }
            printf("[IB] Disconnected: %s\n", info.c_str());
        }
    };

    // ── Historical bars (chart) ───────────────────────────────────────────
    g_IBClient->onBarData = [](int reqId, const core::Bar& bar, bool done, bool isLive) {
        for (auto& ce : g_chartEntries) {
            if (reqId == ce.extId) {
                // Extend-history (pan-left) response — prepend to existing chart data
                if (!done) {
                    ce.pendingExtBars.bars.push_back(bar);
                } else if (ce.win) {
                    ce.win->PrependHistoricalData(ce.pendingExtBars);
                    ce.pendingExtBars.bars.clear();
                }
                return;
            }
            if (reqId == ce.histId) {
                if (isLive) {
                    if (ce.win) ce.win->UpdateLiveBar(bar);
                } else if (!done) {
                    ce.pendingBars.bars.push_back(bar);
                } else if (ce.win) {
                    ce.win->SetHistoricalData(ce.pendingBars);
                    ce.pendingBars.bars.clear();
                }
                return;
            }
        }
    };

    // ── Market data ticks ─────────────────────────────────────────────────
    g_IBClient->onTickSize = [](int tickerId, int field, double size) {
        // Normalise delayed-data variants (paper / reqMarketDataType(3)) to standard fields.
        // DELAYED_BID_SIZE=69, DELAYED_ASK_SIZE=70, DELAYED_LAST_SIZE=71, DELAYED_VOLUME=74
        switch (field) {
            case 69: field = 0; break;
            case 70: field = 3; break;
            case 71: field = 5; break;
            case 74: field = 8; break;  // DELAYED_VOLUME → VOLUME
            default: break;
        }

        // Trading entry: NBBO sizes and LAST_SIZE
        for (auto& te : g_tradingEntries) {
            if (tickerId != te.mktId) continue;
            switch (field) {
                case 5: te.lastTickSize = size; break;
                case 0:  // BID_SIZE
                    te.nbboBidSz = size;
                    if (te.win) te.win->OnNBBO(te.nbboBid, te.nbboBidSz, te.nbboAsk, te.nbboAskSz);
                    break;
                case 3:  // ASK_SIZE
                    te.nbboAskSz = size;
                    if (te.win) te.win->OnNBBO(te.nbboBid, te.nbboBidSz, te.nbboAsk, te.nbboAskSz);
                    break;
                default: break;
            }
        }

        // Scanner entries: avg volume (87) and day volume (8)
        auto symIt = g_tickerSymbols.find(tickerId);
        if (symIt == g_tickerSymbols.end()) return;
        const std::string& sym = symIt->second;
        for (auto& se : g_scannerEntries) {
            if (tickerId < se.mktBase || tickerId >= se.mktBase + ScannerEntry::kMktSlots) continue;
            if (field == 87 && se.win)
                se.win->SetAvgVolume(sym, size);
            if (field == 8) {
                g_scannerVolume[sym] = size;
                if (se.win) se.win->OnQuoteUpdate(sym, 0.0, 0.0, 0.0, size);
            }
        }
    };

    g_IBClient->onTickPrice = [](int tickerId, int field, double price) {
        auto it = g_tickerSymbols.find(tickerId);
        if (it == g_tickerSymbols.end()) return;
        const std::string& sym = it->second;

        // Normalise delayed-data tick fields (paper / reqMarketDataType(3)) to their
        // standard equivalents so the switch below handles both live and paper accounts.
        // DELAYED_BID=66, DELAYED_ASK=67, DELAYED_LAST=68,
        // DELAYED_HIGH=72, DELAYED_LOW=73, DELAYED_OPEN=76
        switch (field) {
            case 66: field = 1;  break;  // DELAYED_BID
            case 67: field = 2;  break;  // DELAYED_ASK
            case 68: field = 4;  break;  // DELAYED_LAST
            case 72: field = 6;  break;  // DELAYED_HIGH
            case 73: field = 7;  break;  // DELAYED_LOW
            case 75: field = 9;  break;  // DELAYED_CLOSE (previous session close)
            case 76: field = 14; break;  // DELAYED_OPEN
            default: break;
        }

        // Find which scanner entry owns this tickerId (if any)
        ScannerEntry* scanEntry = nullptr;
        for (auto& se : g_scannerEntries) {
            if (tickerId >= se.mktBase && tickerId < se.mktBase + ScannerEntry::kMktSlots) {
                scanEntry = &se;
                break;
            }
        }

        switch (field) {
            case 1:  // BID price — fire NBBO update
                for (auto& te : g_tradingEntries) {
                    if (tickerId == te.mktId) {
                        te.nbboBid = price;
                        if (te.win) te.win->OnNBBO(te.nbboBid, te.nbboBidSz, te.nbboAsk, te.nbboAskSz);
                    }
                }
                break;
            case 2:  // ASK price — fire NBBO update
                for (auto& te : g_tradingEntries) {
                    if (tickerId == te.mktId) {
                        te.nbboAsk = price;
                        if (te.win) te.win->OnNBBO(te.nbboBid, te.nbboBidSz, te.nbboAsk, te.nbboAskSz);
                    }
                }
                break;
            case 9: {  // CLOSE — previous session close price
                g_scannerPrevClose[sym] = price;
                if (scanEntry && scanEntry->win)
                    scanEntry->win->SetPrevClose(sym, price);
                break;
            }
            case 79: {  // IB_52_WK_HIGH (from generic tick 165)
                if (scanEntry && scanEntry->win)
                    scanEntry->win->Set52WHigh(sym, price);
                break;
            }
            case 80: {  // IB_52_WK_LOW (from generic tick 165)
                if (scanEntry && scanEntry->win)
                    scanEntry->win->Set52WLow(sym, price);
                break;
            }
            case 4: {  // LAST price
                // Trading windows
                for (auto& te : g_tradingEntries) {
                    if (tickerId == te.mktId) {
                        bool isUp = (price >= te.lastTickPrice);
                        te.lastTickPrice = price;
                        if (te.win) {
                            te.win->UpdateMidPrice(price);
                            te.win->OnTick(price, te.lastTickSize, isUp);
                        }
                    }
                }
                // Chart windows
                for (auto& ce : g_chartEntries) {
                    if (tickerId == ce.mktId && ce.win && sym == ce.win->getSymbol()) {
                        ce.win->OnLastPrice(price);
                        ce.win->OnDayTick(4, price);
                    }
                }
                // Position market-price update
                auto pit = g_positions.find(sym);
                if (pit != g_positions.end()) {
                    pit->second.marketPrice = price;
                    UpdateAllChartPositions();
                }
                // Scanner: compute change/% from stored prevClose
                if (scanEntry && scanEntry->win) {
                    double prevC = 0.0;
                    auto pcIt = g_scannerPrevClose.find(sym);
                    if (pcIt != g_scannerPrevClose.end()) prevC = pcIt->second;
                    double chg    = prevC > 0.0 ? price - prevC : 0.0;
                    double chgPct = prevC > 0.0 ? (chg / prevC) * 100.0 : 0.0;
                    double vol    = 0.0;
                    auto vIt = g_scannerVolume.find(sym);
                    if (vIt != g_scannerVolume.end()) vol = vIt->second;
                    scanEntry->win->OnQuoteUpdate(sym, price, chg, chgPct, vol);
                }
                break;
            }
            case 6: {  // HIGH
                for (auto& ce : g_chartEntries)
                    if (tickerId == ce.mktId && ce.win && sym == ce.win->getSymbol())
                        ce.win->OnDayTick(6, price);
                break;
            }
            case 7: {  // LOW
                for (auto& ce : g_chartEntries)
                    if (tickerId == ce.mktId && ce.win && sym == ce.win->getSymbol())
                        ce.win->OnDayTick(7, price);
                break;
            }
            case 14: {  // OPEN
                for (auto& ce : g_chartEntries)
                    if (tickerId == ce.mktId && ce.win && sym == ce.win->getSymbol())
                        ce.win->OnDayTick(14, price);
                break;
            }
            default: break;
        }
    };

    // ── Market depth (Level II order book) ───────────────────────────────
    g_IBClient->onDepthUpdate = [](int id, bool isBid, int pos, int op,
                                   double price, double size) {
        for (auto& te : g_tradingEntries)
            if (id == te.depthId && te.win)
                te.win->OnDepthUpdate(id, isBid, pos, op, price, size);
    };

    // ── Account values ────────────────────────────────────────────────────
    g_IBClient->onAccountValue = [](const std::string& key, const std::string& val,
                                    const std::string& currency,
                                    const std::string& acct) {
        if (g_PortfolioWindow)
            g_PortfolioWindow->OnAccountValue(key, val, currency, acct);
    };

    // ── Account summary (base currency via reqAccountSummary) ─────────────
    // tag="Currency", value="USD" — this is the authoritative source.
    g_IBClient->onAccountSummary = [](const std::string& tag, const std::string& value,
                                      const std::string& /*currency*/) {
        if (tag == "Currency" && !value.empty() && g_PortfolioWindow)
            g_PortfolioWindow->SetBaseCurrency(value);
    };

    // ── Positions ─────────────────────────────────────────────────────────
    g_IBClient->onPositionData = [](const core::Position& pos, bool done) {
        if (g_PortfolioWindow) {
            if (!done)
                g_PortfolioWindow->OnPositionUpdate(pos);
            else
                g_PortfolioWindow->OnAccountEnd();
        }
        // Accumulate symbols; on done push to scanner and news window
        if (!done) {
            auto it = std::find(g_portfolioSymbols.begin(),
                                g_portfolioSymbols.end(), pos.symbol);
            if (it == g_portfolioSymbols.end())
                g_portfolioSymbols.push_back(pos.symbol);
        } else {
            for (auto& se : g_scannerEntries)
                if (se.win) se.win->SetPortfolioSymbols(g_portfolioSymbols);
            if (g_NewsWindow) g_NewsWindow->SetPortfolioSymbols(g_portfolioSymbols);
        }
    };

    // ── Portfolio updates (P&L etc.) ──────────────────────────────────────
    g_IBClient->onPortfolioUpdate = [](const core::Position& pos) {
        if (g_PortfolioWindow) g_PortfolioWindow->OnPositionUpdate(pos);
        g_positions[pos.symbol] = pos;
        UpdateAllChartPositions();
    };

    // ── Open orders (full detail on submit / reqOpenOrders) ───────────────
    g_IBClient->onOpenOrder = [](const core::Order& order) {
        g_liveOrders[order.orderId] = order;
        if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(order);
        UpdateAllChartPendingOrders();
    };
    g_IBClient->onOpenOrderEnd = []() {
        // nothing extra needed — data already pushed via onOpenOrder
    };

    // ── Order status ──────────────────────────────────────────────────────
    g_IBClient->onOrderStatusChanged = [](int orderId, core::OrderStatus status,
                                          double filled, double avgPrice) {
        for (auto& te : g_tradingEntries)
            if (te.win) te.win->OnOrderStatus(orderId, status, filled, avgPrice);
        if (g_OrdersWindow)
            g_OrdersWindow->OnOrderStatus(orderId, status, filled, avgPrice);
        // Keep our local order map in sync for the chart overlay
        auto it = g_liveOrders.find(orderId);
        if (it != g_liveOrders.end()) {
            it->second.status       = status;
            it->second.filledQty    = filled;
            it->second.avgFillPrice = avgPrice;
        }
        UpdateAllChartPendingOrders();
    };

    // ── Fills ─────────────────────────────────────────────────────────────
    g_IBClient->onFillReceived = [](const core::Fill& fill) {
        for (auto& te : g_tradingEntries)
            if (te.win) te.win->OnFill(fill);
        if (g_OrdersWindow) g_OrdersWindow->OnFill(fill);
        // Accumulate commission per symbol for the P&L strip
        g_symbolCommissions[fill.symbol] += fill.commission;
        if (g_PortfolioWindow) {
            core::TradeRecord tr;
            tr.tradeId    = fill.orderId;
            tr.symbol     = fill.symbol;
            tr.side       = fill.side == core::OrderSide::Buy ? "BUY" : "SELL";
            tr.quantity   = fill.quantity;
            tr.price      = fill.price;
            tr.commission = fill.commission;
            tr.realizedPnL = fill.realizedPnL;
            tr.executedAt  = fill.timestamp;
            g_PortfolioWindow->OnTradeExecuted(tr);
        }
        UpdateAllChartPositions();
    };


    // ── Scanner ───────────────────────────────────────────────────────────
    g_IBClient->onScanItem = [](int reqId, const core::ScanResult& result) {
        for (auto& se : g_scannerEntries)
            if (reqId == se.activeScanId) se.pendingResults.push_back(result);
    };
    g_IBClient->onScanEnd = [](int reqId) {
        for (auto& se : g_scannerEntries) {
            if (reqId != se.activeScanId || !se.win) continue;

            // Cancel previously-subscribed market data slots before (re)subscribing.
            // Without this, a rapid rescan reuses the same reqIds while the farm still has
            // them active → "Duplicate ticker id" errors for each slot.
            for (int i = 0; i < ScannerEntry::kMktSlots; ++i) {
                int rid = se.mktBase + i;
                if (g_tickerSymbols.count(rid)) {
                    g_IBClient->CancelMarketData(rid);
                    g_tickerSymbols.erase(rid);
                }
            }

            // Deliver results (empty vector clears m_scanning without wiping the table).
            se.win->OnScanData(reqId, se.pendingResults);

            // Subscribe market data for each result so price/change/volume columns live-update.
            int slot = 0;
            for (const auto& r : se.pendingResults) {
                if (slot >= ScannerEntry::kMktSlots) break;
                int rid = se.mktBase + slot;
                g_tickerSymbols[rid] = r.symbol;
                g_IBClient->ReqMarketData(rid, r.symbol, MktDataTicks());
                ++slot;
            }
            se.pendingResults.clear();
            break;
        }
    };

    // ── News — real-time (tickNews fires for subscribed market data) ──────
    g_IBClient->onNewsItem = [](std::time_t ts, const std::string& provider,
                                const std::string& articleId,
                                const std::string& headline) {
        if (!g_NewsWindow) return;
        core::NewsItem item;
        item.id        = ++g_newsItemId;
        item.headline  = headline;
        item.source    = provider;
        item.summary   = articleId;   // transport: window extracts → m_itemArticleIds
        item.timestamp = ts;
        item.sentiment = core::NewsSentiment::Neutral;
        item.category  = core::NewsCategory::Market;
        g_NewsWindow->OnMarketNewsItem(item);
    };

    // ── News — contract details → historical news chain ───────────────────
    g_IBClient->onContractConId = [](int reqId, long conId) {
        if (!g_IBClient) return;
        // IB may call contractDetails multiple times (one per exchange match).
        // Only use the first conId per reqId so we don't issue duplicate news requests.
        if (g_newsConIdFired[reqId]) return;
        g_newsConIdFired[reqId] = true;
        if (reqId == NEWS_CONID_STOCK) {
            g_IBClient->ReqHistoricalNews(NEWS_HIST_STOCK, (int)conId, 30);
        } else if (reqId >= NEWS_CONID_PORT && reqId < NEWS_CONID_PORT + 20) {
            int i = reqId - NEWS_CONID_PORT;
            g_IBClient->ReqHistoricalNews(NEWS_HIST_PORT + i, (int)conId, 10);
        } else if (reqId >= NEWS_CONID_MKT && reqId < NEWS_CONID_MKT + kMktSeedCount) {
            int i = reqId - NEWS_CONID_MKT;
            g_IBClient->ReqHistoricalNews(NEWS_HIST_MKT + i, (int)conId, 20);
        }
    };

    g_IBClient->onHistoricalNews = [](int reqId, std::time_t ts,
                                      const std::string& provider,
                                      const std::string& articleId,
                                      const std::string& headline) {
        if (!g_NewsWindow) return;
        core::NewsItem item;
        item.id        = ++g_histNewsId;
        item.headline  = headline;
        item.source    = provider;    // providerCode needed for reqNewsArticle
        item.summary   = articleId;   // transport: window extracts → m_itemArticleIds
        item.timestamp = ts;
        item.sentiment = core::NewsSentiment::Neutral;
        if (reqId == NEWS_HIST_STOCK)
            item.category = core::NewsCategory::Stock;
        else if (reqId >= NEWS_HIST_MKT && reqId < NEWS_HIST_MKT + kMktSeedCount)
            item.category = core::NewsCategory::Market;
        else
            item.category = core::NewsCategory::Portfolio;
        g_NewsWindow->OnHistoricalNewsItem(reqId, item);
    };

    g_IBClient->onHistoricalNewsEnd = [](int reqId) {
        if (g_NewsWindow) g_NewsWindow->OnHistoricalNewsEnd(reqId);
    };

    g_IBClient->onNewsArticle = [](int reqId, int /*articleType*/,
                                   const std::string& text) {
        auto it = g_artReqToItemId.find(reqId);
        if (it == g_artReqToItemId.end()) return;
        int itemId = it->second;
        g_artReqToItemId.erase(it);
        if (g_NewsWindow) g_NewsWindow->OnArticleReceived(itemId, text);
    };

    // ── Errors ────────────────────────────────────────────────────────────
    g_IBClient->onError = [](int reqId, int code, const std::string& msg) {
        fprintf(stderr, "[IB Error reqId=%d code=%d] %s\n", reqId, code, msg.c_str());

        // Order-related error: mark the order as Rejected in all windows.
        // IB sends error() for rejections alongside (or instead of) orderStatus().
        // We act on Pending OR Working orders — outside-RTH rejections arrive after
        // IB has already set the order to Working state.
        auto it = g_liveOrders.find(reqId);
        if (it != g_liveOrders.end() &&
            (it->second.status == core::OrderStatus::Pending ||
             it->second.status == core::OrderStatus::Working)) {
            char reason[512];
            std::snprintf(reason, sizeof(reason), "[%d] %s", code, msg.c_str());
            it->second.status       = core::OrderStatus::Rejected;
            it->second.rejectReason = reason;
            if (g_OrdersWindow)
                g_OrdersWindow->OnOrderStatus(reqId, core::OrderStatus::Rejected, 0, 0, reason);
            for (auto& te : g_tradingEntries)
                if (te.win) te.win->OnOrderStatus(reqId, core::OrderStatus::Rejected, 0, 0);
            UpdateAllChartPendingOrders();
        }

        // News errors: if reqHistoricalNews or reqContractDetails fails (e.g. no news
        // subscription, code 321/10197), IB sends an error instead of historicalNewsEnd.
        // Clear the loading state so the UI doesn't hang on "Loading..." indefinitely.
        if (g_NewsWindow) {
            if (reqId == NEWS_HIST_STOCK || reqId == NEWS_CONID_STOCK) {
                g_NewsWindow->OnHistoricalNewsEnd(NEWS_HIST_STOCK);
            } else if (reqId >= NEWS_HIST_PORT && reqId < NEWS_HIST_PORT + 20) {
                g_NewsWindow->OnHistoricalNewsEnd(reqId);
            } else if (reqId >= NEWS_CONID_PORT && reqId < NEWS_CONID_PORT + 20) {
                int i = reqId - NEWS_CONID_PORT;
                g_NewsWindow->OnHistoricalNewsEnd(NEWS_HIST_PORT + i);
            }
            // Market-tab seed errors don't affect loading UI — market tab is push-based.
        }

        // Subscription errors for market data (NBBO) and Level II depth per trading entry.
        // 354 = not subscribed, 10090 = partial, 10092 = deep book not allowed, 322 = no perms
        for (auto& te : g_tradingEntries) {
            if (!te.win) continue;
            if (reqId == te.mktId && (code == 354 || code == 10090))
                te.win->OnMktDataError(code);
            if (reqId == te.depthId && (code == 354 || code == 10090 || code == 10092 || code == 322))
                te.win->OnDepthError(code);
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

// Silent background reconnect — called from the main loop when LostConnection.
// State stays LostConnection until onConnectionChanged fires with connected=true.
static void StartSilentReconnect() {
    printf("[IB] Auto-reconnect attempt to %s:%d...\n", g_Login.host, g_Login.port);
    delete g_IBClient;
    g_IBClient = new core::services::IBKRClient();
    WireIBCallbacks();

    bool ok = g_IBClient->Connect(g_Login.host, g_Login.port, g_Login.clientId);
    if (!ok) {
        // Gateway still down — delete client and schedule next retry.
        delete g_IBClient;
        g_IBClient             = nullptr;
        g_reconnectNextAttempt = glfwGetTime() + kReconnectIntervalSec;
        printf("[IB] Reconnect failed — will retry in %.0fs.\n", kReconnectIntervalSec);
    }
    // On success the async onConnectionChanged(true) callback fires and sets Connected.
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
    g_scannerPrevClose.clear();
    g_scannerVolume.clear();
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
// Window presets
// ============================================================================
static const core::WindowPreset kBuiltinPresets[] = {
    // name           chart       trading     news        scanner     portfolio   orders
    { "Trading Focus",{true,  1}, {true,  1}, {false, 0}, {false, 0}, {false, 0}, {true,  1} },
    { "Research",     {true,  1}, {false, 0}, {true,  1}, {true,  1}, {false, 0}, {false, 0} },
    { "Full Desk",    {true,  1}, {true,  1}, {true,  2}, {true,  2}, {true,  0}, {true,  1} },
};
static constexpr int kNumBuiltinPresets = static_cast<int>(
    sizeof(kBuiltinPresets) / sizeof(kBuiltinPresets[0]));

static void ApplyPreset(const core::WindowPreset& p) {
    // Apply to first instance of each multi-instance type
    if (!g_chartEntries.empty() && g_chartEntries[0].win) {
        g_chartEntries[0].win->open()       = p.chart.visible;
        g_chartEntries[0].win->setGroupId(p.chart.groupId);
    }
    if (!g_tradingEntries.empty() && g_tradingEntries[0].win) {
        g_tradingEntries[0].win->open()     = p.trading.visible;
        g_tradingEntries[0].win->setGroupId(p.trading.groupId);
    }
    if (!g_scannerEntries.empty() && g_scannerEntries[0].win) {
        g_scannerEntries[0].win->open()     = p.scanner.visible;
        g_scannerEntries[0].win->setGroupId(p.scanner.groupId);
    }
    if (g_NewsWindow)      { g_NewsWindow->open()      = p.news.visible;      g_NewsWindow->setGroupId(p.news.groupId); }
    if (g_PortfolioWindow) { g_PortfolioWindow->open() = p.portfolio.visible; }
    if (g_OrdersWindow)    { g_OrdersWindow->open()    = p.orders.visible; }
    // Reset group state so the next symbol change re-broadcasts correctly
    for (auto& gs : g_groups) gs.symbol.clear();
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
                // Per-instance chart windows
                for (auto& ce : g_chartEntries) {
                    if (!ce.win) continue;
                    char lbl[48];
                    std::snprintf(lbl, sizeof(lbl), "Chart %d", ce.win->instanceId());
                    ImGui::MenuItem(lbl, nullptr, &ce.win->open());
                }
                if ((int)g_chartEntries.size() < kMaxMultiWin) {
                    if (ImGui::MenuItem("+ New Chart"))
                        SpawnChartWindow((int)g_chartEntries.size());
                }
                ImGui::Separator();
                // Per-instance trading windows
                for (auto& te : g_tradingEntries) {
                    if (!te.win) continue;
                    char lbl[48];
                    std::snprintf(lbl, sizeof(lbl), "Order Book %d", te.win->instanceId());
                    ImGui::MenuItem(lbl, nullptr, &te.win->open());
                }
                if ((int)g_tradingEntries.size() < kMaxMultiWin) {
                    if (ImGui::MenuItem("+ New Order Book"))
                        SpawnTradingWindow((int)g_tradingEntries.size());
                }
                ImGui::Separator();
                // Per-instance scanner windows
                for (auto& se : g_scannerEntries) {
                    if (!se.win) continue;
                    char lbl[48];
                    std::snprintf(lbl, sizeof(lbl), "Scanner %d", se.win->instanceId());
                    ImGui::MenuItem(lbl, nullptr, &se.win->open());
                }
                if ((int)g_scannerEntries.size() < kMaxMultiWin) {
                    if (ImGui::MenuItem("+ New Scanner"))
                        SpawnScannerWindow((int)g_scannerEntries.size());
                }
                ImGui::Separator();
                // Singleton windows
                if (g_OrdersWindow)    ImGui::MenuItem("Orders",    nullptr, &g_OrdersWindow->open());
                if (g_PortfolioWindow) ImGui::MenuItem("Portfolio", nullptr, &g_PortfolioWindow->open());
                if (g_NewsWindow)      ImGui::MenuItem("News",      nullptr, &g_NewsWindow->open());
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Presets")) {
                for (int i = 0; i < kNumBuiltinPresets; i++) {
                    if (ImGui::MenuItem(kBuiltinPresets[i].name))
                        ApplyPreset(kBuiltinPresets[i]);
                }
                ImGui::EndMenu();
            }

            // Status indicator
            const std::string& who = g_Login.connectedAs;
            bool lostConn = (g_Login.state == ConnectionState::LostConnection);
            static const char* kDiscLabel = " DISCONNECTED ";
            float discW = lostConn ? ImGui::CalcTextSize(kDiscLabel).x + 8.0f : 0.0f;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x
                            - discW - ImGui::CalcTextSize(who.c_str()).x - 12.0f);
            if (lostConn) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImVec2 sz = ImGui::CalcTextSize(kDiscLabel);
                float pad = 4.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(
                    ImVec2(p.x - pad, p.y - 1),
                    ImVec2(p.x + sz.x + pad, p.y + sz.y + 1),
                    IM_COL32(180, 60, 0, 220), 3.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
                ImGui::Text("%s", kDiscLabel);
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
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

    // Render all window instances
    for (auto& ce : g_chartEntries)   if (ce.win) ce.win->Render();
    for (auto& te : g_tradingEntries) if (te.win) te.win->Render();
    for (auto& se : g_scannerEntries) if (se.win) se.win->Render();
    if (g_NewsWindow)      g_NewsWindow->Render();
    if (g_PortfolioWindow) g_PortfolioWindow->Render();
    if (g_OrdersWindow)    g_OrdersWindow->Render();
}

// ============================================================================
// Top-level UI dispatcher
// ============================================================================
static void RenderMainUI() {
    if (g_Login.state == ConnectionState::Connected ||
        g_Login.state == ConnectionState::LostConnection)
        RenderTradingUI();
    else
        RenderLoginWindow();
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

        // Auto-reconnect when connection was lost unexpectedly
        if (g_Login.state == ConnectionState::LostConnection && !g_IBClient &&
            glfwGetTime() >= g_reconnectNextAttempt)
            StartSilentReconnect();

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
