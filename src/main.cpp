/**
 * Interactive Brokers Trading Application — Main Entry Point
 *
 * Connects to a running IB Gateway or TWS via the C++ TWS API.
 * Authentication is handled by TWS/Gateway itself; we only need host/port/clientId.
 */

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <filesystem>

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
#include "ui/windows/WatchlistWindow.h"
#include "ui/windows/WshCalendarWindow.h"
#include "ui/SymbolSearch.h"

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
    int              wshId       = 0;   // reqContractDetails / reqWshEventData id
    bool             wshConIdFired = false;
    core::BarSeries  pendingBars;
    core::BarSeries  pendingExtBars;
    std::string      bboExchange;       // last bboExchange code from tickReqParams
};

struct TradingEntry {
    ui::TradingWindow* win          = nullptr;
    int                depthId      = 0;   // reqMktDepth id
    int                mktId        = 0;   // reqMarketData id (NBBO ticks)
    int                tickId       = 0;   // reqTickByTickData id (Time & Sales)
    double nbboBid = 0, nbboBidSz = 0;
    double nbboAsk = 0, nbboAskSz = 0;
    double lastTickPrice = 0, lastTickSize = 0;
    std::string        bboExchange;         // last bboExchange code from tickReqParams
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

static constexpr int   kMaxMultiWin = 10;   // max instances per window type
static constexpr float kTitleBarH   = 32.0f; // custom title bar height
static bool            g_tbDragging = false; // title-bar drag in progress (shared with resize handler)

struct NewsEntry {
    ui::NewsWindow*              win          = nullptr;
    int                          nextArtReqId = 0;
    int                          artEnd       = 0;
    std::unordered_map<int,int>  artReqToItemId;
    std::unordered_map<int,bool> newsConIdFired;
};

struct WatchlistEntry {
    ui::WatchlistWindow* win = nullptr;
};

// ---- Multi-instance containers -----------------------------------------------
static std::vector<ChartEntry>      g_chartEntries;
static std::vector<TradingEntry>    g_tradingEntries;
static std::vector<ScannerEntry>    g_scannerEntries;
static std::vector<NewsEntry>       g_newsEntries;
static std::vector<WatchlistEntry>  g_watchlistEntries;

// ---- Singleton windows (one each) --------------------------------------------
static ui::PortfolioWindow*    g_PortfolioWindow    = nullptr;
static ui::OrdersWindow*       g_OrdersWindow       = nullptr;
static ui::WshCalendarWindow*  g_WshCalendarWindow  = nullptr;

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

// ---- Settings ---------------------------------------------------------------
enum class FontSize { Small = 0, Medium = 1, Large = 2 };
static FontSize g_fontSize     = FontSize::Medium;
static bool     g_settingsOpen = false;

static constexpr float kFontScales[] = { 0.85f, 1.0f, 1.5f }; // Small / Medium / Large
static ImGuiStyle      g_baseStyle;   // saved after initial style setup; used to re-scale cleanly

// ---- Window groups (4 slots; index 0 = group id 1) -------------------------
static std::array<core::GroupState, 4> g_groups;
// Guard against re-entrant group broadcasts when SetSymbol() re-fires callbacks.
static bool g_groupSyncInProgress = false;

// Live orders for chart overlay (orderId → Order; refreshed on every status change)
static std::unordered_map<int, core::Order> g_liveOrders;

// Per-symbol positions and commissions for the chart P&L strip
static std::unordered_map<std::string, core::Position> g_positions;
static std::unordered_map<std::string, double>          g_symbolCommissions;

// Smart components cache: bboExchange code → routing destinations
// Populated by onSmartComponents; shared across all TradingWindow instances.
static std::unordered_map<std::string, std::vector<core::SmartRoute>> g_smartComponents;

// symbol → conId mapping populated from contractDetails callbacks.
// Used by the IB display-group outbound sync to build contractInfo strings.
static std::unordered_map<std::string, long> g_symbolConIds;

// IB TWS Display Group sync toggle and subscription state.
static bool g_twsGroupSync = false;   // user-controlled in Settings panel

// Multi-account state
static std::vector<std::string>         g_managedAccounts;     // all accounts from managedAccounts()
static std::string                      g_selectedAccount;     // currently active account
static bool                             g_pendingReconnect = false; // deferred reconnect flag

// Real-time P&L subscription state
static std::string                      g_accountId;           // captured from first updateAccountValue
static bool                             g_pnlSubscribed = false;
static std::unordered_map<long, int>    g_pnlSingleConIds;     // conId → reqId
static int                              g_pnlSingleNextReqId = 9001;
static std::unordered_map<int, std::string> g_pnlReqIdToSymbol; // reqId → symbol

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
        } else if (o.type == core::OrderType::LIT) {
            ln.price     = o.auxPrice;    // trigger price — drawn as main leg
            ln.auxPrice  = o.limitPrice;  // limit price — drawn as aux leg
            ln.orderType = "LIT";
        } else if (o.type == core::OrderType::Stop) {
            ln.price     = o.stopPrice;
            ln.orderType = "STP";
        } else if (o.type == core::OrderType::Limit) {
            ln.price     = o.limitPrice;
            ln.orderType = "LMT";
        } else if (o.type == core::OrderType::LOC) {
            ln.price     = o.limitPrice;
            ln.orderType = "LOC";
        } else if (o.type == core::OrderType::MIT) {
            ln.price     = o.auxPrice;
            ln.orderType = "MIT";
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
        info.dailyPnL    = it->second.dailyPnL;
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
//   Chart   hist:  1,3,5,...,19  ext: 2,4,6,...,20  mkt: 100-109
//   Trading mkt:   110-119       depth: 120-129
//   Scanner scan:  1000,1100,...,1900 (+99 ea)  mkt: 800,812,...,908 (+12 ea, 12 slots each)
//   News:          201(RT), 400-420(conId), 500-520(hist), 600-699(art), 700-759(mkt)
//   Account:       900
static constexpr int NEWS_RT_REQID       = 201;  // real-time news subscription (mdoff;292)

// Inline helpers — idx is 0-based instance index
inline int ChartHistId   (int idx) { return 1    + idx * 2; }   // 1,3,5,...,19
inline int ChartExtId    (int idx) { return 2    + idx * 2; }   // 2,4,6,...,20
inline int ChartMktId    (int idx) { return 100  + idx; }       // 100-109
inline int TradingMktId  (int idx) { return 110  + idx; }       // 110-119
inline int TradingDepthId(int idx) { return 120  + idx; }       // 120-129
inline int TradingTickId (int idx) { return 130  + idx; }       // 130-139
inline int ChartWshId    (int idx) { return 8020 + idx; }       // 8020-8029
inline int ScannerBase   (int idx) { return 1000 + idx * 100; } // 1000,1100,...,1900
inline int ScannerMktBase(int idx) { return 800  + idx * 12; }  // 800,812,...,908

static constexpr int ACCT_SUMMARY_REQID  = 900;
static constexpr const char* ACCT_SUMMARY_TAGS =
    "Currency,NetLiquidation,TotalCashValue,BuyingPower,"
    "UnrealizedPnL,RealizedPnL,InitMarginReq,MaintMarginReq,ExcessLiquidity";

// News reqId layout (per-instance, idx 0-9):
//   Stock conId:   2000+idx
//   Port conId:    2010+idx*20  (+19 per entry, 20 portfolio symbols)
//   Stock hist:    2210+idx
//   Port hist:     2220+idx*20  (+19 per entry)
//   Article base:  2500+idx*100 (+99 per entry, rolling)
//   Mkt hist:      3500+idx*10  (+9 per entry, kMktSeedCount seed symbols)
//   Mkt conId:     3600+idx*10  (+9 per entry)
inline int NewsStockConId(int idx) { return 2000 + idx; }
inline int NewsPortConId (int idx) { return 2010 + idx * 20; }
inline int NewsHistStock (int idx) { return 2210 + idx; }
inline int NewsHistPort  (int idx) { return 2220 + idx * 20; }
inline int NewsArtBase   (int idx) { return 2500 + idx * 100; }
inline int NewsArtEnd    (int idx) { return 2599 + idx * 100; }
inline int NewsHistMkt   (int idx) { return 3500 + idx * 10; }
inline int NewsConIdMkt  (int idx) { return 3600 + idx * 10; }

// Symbols fetched on connection to seed the Market-tab news feed.
static const char* kMktSeedSymbols[] = { "AAPL", "SPY", "MSFT", "TSLA", "NVDA" };
static constexpr int kMktSeedCount   = 5;

// Watchlist reqId layout (per-instance, idx 0-9):
//   contract details: 6900+idx   (transient, one in flight per instance)
//   market data:      7000+idx*100  (+99 per instance, up to 100 symbols each)
inline int WatchlistCdId (int idx) { return 6900 + idx; }
inline int WatchlistMktBase(int idx) { return 7000 + idx * 100; }

// ============================================================================
// Connection / Login state
// ============================================================================
enum class ConnectionState { Disconnected, Connecting, Connected,
                             SelectingAccount, LostConnection, Error };
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
    te.bboExchange.clear();
    if (te.win) {
        te.win->SetSymbol(sym, 0.0);
        te.win->SetExchangeList({"SMART"});  // reset until tickReqParams arrives
        // Re-inject position from portfolio cache for the new symbol
        auto it = g_positions.find(sym);
        if (it != g_positions.end())
            te.win->SetPosition(it->second.quantity, it->second.avgCost);
    }
    if (!g_IBClient) return;
    g_IBClient->CancelMarketData(te.mktId);
    g_IBClient->CancelMktDepth(te.depthId);
    g_IBClient->CancelTickByTickData(te.tickId);
    g_tickerSymbols[te.mktId] = sym;
    g_IBClient->ReqMarketData(te.mktId, sym, MktDataTicks());
    g_IBClient->ReqMktDepth(te.depthId, sym, 20);
    g_IBClient->ReqTickByTickData(te.tickId, sym);
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
    for (auto& ne : g_newsEntries)
        if (ne.win && ne.win->groupId() == groupId) ne.win->SetSymbol(sym);
    // ScannerWindow is a symbol source only — no inbound SetSymbol

    // Outbound IB display-group sync: push new symbol into the matching TWS group.
    if (g_twsGroupSync && g_IBClient && groupId >= 1 && groupId <= 4) {
        long conId = 0;
        auto cit = g_symbolConIds.find(sym);
        if (cit != g_symbolConIds.end()) conId = cit->second;
        if (conId > 0) {
            std::string ci = std::to_string(conId) + "@SMART";
            g_IBClient->UpdateDisplayGroup(8060 + groupId, ci);  // 8061–8064
        }
    }

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
        // Reset exchange list; tickReqParams will repopulate once the new sub is live.
        for (auto& ce : g_chartEntries)
            if (ce.mktId == mktId) { ce.bboExchange.clear(); if (ce.win) ce.win->SetExchangeList({"SMART"}); }
    }
}

static void SpawnChartWindow(int idx) {
    ChartEntry e;
    e.histId = ChartHistId(idx);
    e.extId  = ChartExtId(idx);
    e.mktId  = ChartMktId(idx);
    e.wshId  = ChartWshId(idx);
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
        // WSH: clear stale markers and subscribe for the new symbol
        if (g_IBClient) {
            ce.win->ClearWshEvents();
            ce.wshConIdFired = false;
            g_IBClient->CancelWshEventData(ce.wshId);
            g_IBClient->ReqContractDetails(ce.wshId, sym);
        }
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

    e.win->OnOrderSubmit = [](const core::Order& o) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        int id = g_nextOrderId++;
        for (auto& te : g_tradingEntries)
            if (te.win) te.win->SetNextOrderId(g_nextOrderId);
        core::Order order   = o;
        order.orderId       = id;
        order.account       = g_selectedAccount;
        order.status        = core::OrderStatus::Pending;
        order.submittedAt   = std::time(nullptr);
        order.updatedAt     = order.submittedAt;
        g_liveOrders[id]    = order;
        if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(order);
        UpdateAllChartPendingOrders();
        g_IBClient->PlaceOrder(order);
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
        rep.account           = g_selectedAccount;
        rep.status            = core::OrderStatus::Pending;
        rep.submittedAt       = std::time(nullptr);
        rep.updatedAt         = rep.submittedAt;
        rep.filledQty         = 0.0;
        rep.avgFillPrice      = 0.0;
        if (old.type == core::OrderType::Limit)     { rep.limitPrice = newPrice;  rep.stopPrice  = 0.0; }
        else if (old.type == core::OrderType::Stop) { rep.stopPrice  = newPrice;  rep.limitPrice = 0.0; }
        else if (old.type == core::OrderType::StopLimit) {
            rep.stopPrice = newPrice; rep.limitPrice = newAuxPrice;
        }
        g_liveOrders[newId] = rep;
        if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(rep);
        UpdateAllChartPendingOrders();
        g_IBClient->PlaceOrder(rep);
    };

    g_chartEntries.push_back(std::move(e));
}

static void SpawnTradingWindow(int idx) {
    TradingEntry e;
    e.depthId = TradingDepthId(idx);
    e.mktId   = TradingMktId(idx);
    e.tickId  = TradingTickId(idx);
    e.win     = new ui::TradingWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId(idx + 1);
    e.win->SetNextOrderId(g_nextOrderId);

    e.win->OnOrderSubmit = [](const core::Order& o) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        core::Order order = o;
        order.account     = g_selectedAccount;
        g_IBClient->PlaceOrder(order);
        if (order.orderId >= g_nextOrderId) g_nextOrderId = order.orderId + 1;
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

static void SpawnNewsWindow(int idx) {
    NewsEntry e;
    e.win          = new ui::NewsWindow();
    e.nextArtReqId = NewsArtBase(idx);
    e.artEnd       = NewsArtEnd(idx);
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId(idx + 1);

    e.win->SetStockNewsReqId(NewsHistStock(idx));
    e.win->SetPortNewsReqIdBase(NewsHistPort(idx));
    e.win->SetMktNewsReqIdBase(NewsHistMkt(idx));

    e.win->OnSymbolChanged = [idx](const std::string& sym) {
        if (idx < (int)g_newsEntries.size() && g_newsEntries[idx].win)
            BroadcastGroupSymbol(g_newsEntries[idx].win->groupId(), sym);
    };
    e.win->OnStockNewsRequested = [idx](const std::string& symbol) {
        if (g_IBClient && g_IBClient->IsConnected())
            g_IBClient->ReqContractDetails(NewsStockConId(idx), symbol);
    };
    e.win->OnPortfolioNewsRequested = [idx](const std::vector<std::string>& syms) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        for (int i = 0; i < (int)syms.size() && i < 20; ++i)
            g_IBClient->ReqContractDetails(NewsPortConId(idx) + i, syms[i]);
    };
    e.win->OnArticleRequested = [idx](int itemId, const std::string& provider,
                                      const std::string& articleId) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        auto& ne = g_newsEntries[idx];
        int reqId = ne.nextArtReqId++;
        if (ne.nextArtReqId > ne.artEnd) ne.nextArtReqId = NewsArtBase(idx);
        ne.artReqToItemId[reqId] = itemId;
        g_IBClient->ReqNewsArticle(reqId, provider, articleId);
    };

    g_newsEntries.push_back(std::move(e));

    // If already connected, seed market news for this new window
    if (g_IBClient && g_IBClient->IsConnected()) {
        for (int i = 0; i < kMktSeedCount; ++i)
            g_IBClient->ReqContractDetails(NewsConIdMkt(idx) + i, kMktSeedSymbols[i]);
    }
}

// ============================================================================
// Watchlist persistence (Task #49)
// ============================================================================
static std::string WatchlistsFilePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = "/tmp";
    return std::string(home) + "/.config/ibkr-trading-app/watchlists.cfg";
}

static void EnsureWatchlistConfigDir() {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    if (!home || !*home) return;
    std::filesystem::create_directories(std::string(home) + "/.config/ibkr-trading-app");
}

static void SaveWatchlistsFile() {
    if (g_watchlistEntries.empty()) return;
    EnsureWatchlistConfigDir();
    std::string path = WatchlistsFilePath();
    std::string tmp  = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return;
        for (const auto& we : g_watchlistEntries)
            if (we.win) f << we.win->serialize();
    }
    std::rename(tmp.c_str(), path.c_str());
}

struct WatchlistSaveBlock {
    int instanceId = 1;
    int groupId    = 0;
    std::vector<core::Watchlist> watchlists;
};

static std::vector<WatchlistSaveBlock> LoadWatchlistsFromFile() {
    std::vector<WatchlistSaveBlock> result;
    std::ifstream f(WatchlistsFilePath());
    if (!f.is_open()) return result;

    std::string line;
    while (std::getline(f, line)) {
        if (line.size() >= 9 && line.substr(0, 9) == "INSTANCE:") {
            WatchlistSaveBlock b;
            try { b.instanceId = std::stoi(line.substr(9)); } catch (...) {}
            result.push_back(std::move(b));
        } else if (!result.empty() && line.size() >= 6 && line.substr(0, 6) == "GROUP:") {
            try { result.back().groupId = std::stoi(line.substr(6)); } catch (...) {}
        } else if (!result.empty() && line.size() >= 6 && line.substr(0, 6) == "WATCH:") {
            result.back().watchlists.push_back({line.substr(6), {}});
        } else if (!result.empty() && !result.back().watchlists.empty() && !line.empty()) {
            core::WatchlistItem item;
            std::istringstream ss(line);
            std::string tok;
            int col = 0;
            while (std::getline(ss, tok, ',')) {
                switch (col++) {
                    case 0: item.symbol      = tok; break;
                    case 1: item.secType     = tok; break;
                    case 2: item.primaryExch = tok; break;
                    case 3: item.currency    = tok; break;
                    case 4: try { item.conId = std::stoi(tok); } catch (...) {} break;
                    case 5: item.description = tok; break;
                }
            }
            if (!item.symbol.empty())
                result.back().watchlists.back().items.push_back(std::move(item));
        }
    }
    return result;
}

static void SpawnWatchlistWindow(int idx) {
    WatchlistEntry e;
    e.win = new ui::WatchlistWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId(idx + 1);
    e.win->setReqIdBase(WatchlistCdId(idx), WatchlistMktBase(idx));

    e.win->OnReqContractDetails = [idx](int reqId, const std::string& sym) {
        if (g_IBClient && g_IBClient->IsConnected())
            g_IBClient->ReqContractDetails(reqId, sym);
    };
    e.win->OnReqMktData = [](int reqId, const std::string& sym,
                              const std::string& /*secType*/, const std::string& /*exch*/,
                              const std::string& /*currency*/) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        g_IBClient->ReqMarketData(reqId, sym, "165,233");
    };
    e.win->OnCancelMktData = [](int reqId) {
        if (g_IBClient && g_IBClient->IsConnected())
            g_IBClient->CancelMarketData(reqId);
    };
    e.win->OnBroadcastSymbol = [idx](const std::string& sym) {
        if (idx < (int)g_watchlistEntries.size() && g_watchlistEntries[idx].win)
            BroadcastGroupSymbol(g_watchlistEntries[idx].win->groupId(), sym);
    };
    e.win->OnOpenChart = [](const std::string& sym) {
        int newIdx = (int)g_chartEntries.size();
        if (newIdx < kMaxMultiWin) {
            SpawnChartWindow(newIdx);
            if (!g_chartEntries.empty() && g_chartEntries.back().win)
                g_chartEntries.back().win->SetSymbol(sym);
        }
    };
    e.win->OnOpenOrderBook = [](const std::string& sym) {
        int newIdx = (int)g_tradingEntries.size();
        if (newIdx < kMaxMultiWin) {
            SpawnTradingWindow(newIdx);
            if (!g_tradingEntries.empty() && g_tradingEntries.back().win)
                ApplyTradingSymbol(g_tradingEntries.back(), sym);
        }
    };

    e.win->AddDefaultsIfEmpty();
    g_watchlistEntries.push_back(std::move(e));
}

static void CreateTradingWindows() {
    // Singleton windows
    delete g_PortfolioWindow;   g_PortfolioWindow   = new ui::PortfolioWindow();
    delete g_OrdersWindow;      g_OrdersWindow      = new ui::OrdersWindow();
    delete g_WshCalendarWindow; g_WshCalendarWindow = new ui::WshCalendarWindow();

    g_WshCalendarWindow->OnReqWshEvents = [](int reqId, long conId) {
        if (g_IBClient) g_IBClient->ReqWshEventData(reqId, conId);
    };
    g_WshCalendarWindow->OnCancelWshEvents = [](int reqId) {
        if (g_IBClient) g_IBClient->CancelWshEventData(reqId);
    };
    g_WshCalendarWindow->OnBroadcastSymbol = [](const std::string& sym) {
        BroadcastGroupSymbol(1, sym);
    };

    // Spawn first instance of each multi-window type
    SpawnChartWindow(0);
    SpawnTradingWindow(0);
    SpawnScannerWindow(0);
    SpawnNewsWindow(0);
    SpawnWatchlistWindow(0);

    // Wire OrdersWindow
    g_OrdersWindow->OnCancelOrder = [](int orderId) {
        if (g_IBClient) g_IBClient->CancelOrder(orderId);
    };
    g_OrdersWindow->OnRefresh = []() {
        if (g_IBClient) g_IBClient->ReqOpenOrders();
    };
    g_OrdersWindow->OnLoadHistory = [](const std::string& sym,
                                       const std::string& side,
                                       const std::string& dateFrom) {
        if (g_IBClient) g_IBClient->ReqExecutions(8001, sym, side, dateFrom);
    };
}

static void DestroyTradingWindows() {
    SaveWatchlistsFile();

    for (auto& e : g_chartEntries)   { delete e.win; e.win = nullptr; }
    for (auto& e : g_tradingEntries) { delete e.win; e.win = nullptr; }
    for (auto& e : g_scannerEntries) { delete e.win; e.win = nullptr; }
    g_chartEntries.clear();
    g_tradingEntries.clear();
    g_scannerEntries.clear();

    for (auto& ne : g_newsEntries) { delete ne.win; ne.win = nullptr; }
    g_newsEntries.clear();
    for (auto& we : g_watchlistEntries) { if (we.win) { we.win->CancelAll(); delete we.win; we.win = nullptr; } }
    g_watchlistEntries.clear();
    delete g_PortfolioWindow;   g_PortfolioWindow   = nullptr;
    delete g_OrdersWindow;      g_OrdersWindow      = nullptr;
    delete g_WshCalendarWindow; g_WshCalendarWindow = nullptr;

    g_portfolioSymbols.clear();
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
// Post-account-selection connect setup (called once account is known)
// ============================================================================
static void FinishConnect(bool isReconnect) {
    g_Login.state = ConnectionState::Connected;

    g_IBClient->ReqMarketDataType(g_Login.isLive ? 1 : 4);

    if (!isReconnect) {
        DestroyTradingWindows();
        CreateTradingWindows();

        // Restore persisted watchlists (overrides Mag 7 defaults if file exists)
        {
            auto saved = LoadWatchlistsFromFile();
            for (int i = 0; i < (int)saved.size(); ++i) {
                if (saved[i].watchlists.empty()) continue;
                if (i >= (int)g_watchlistEntries.size()) {
                    if ((int)g_watchlistEntries.size() >= kMaxMultiWin) break;
                    SpawnWatchlistWindow((int)g_watchlistEntries.size());
                }
                auto& we = g_watchlistEntries[i];
                if (!we.win) continue;
                we.win->setGroupId(saved[i].groupId);
                we.win->LoadWatchlists(saved[i].watchlists);
            }
        }

        g_IBClient->ReqAccountUpdates(true, g_selectedAccount);
        g_IBClient->ReqPositions();
        g_IBClient->ReqAccountSummary(ACCT_SUMMARY_REQID, ACCT_SUMMARY_TAGS);
        g_IBClient->ReqOpenOrders();
        g_IBClient->ReqAllOpenOrders();
        g_IBClient->ReqExecutions(8001);
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
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni)
            for (int i = 0; i < kMktSeedCount; ++i)
                g_IBClient->ReqContractDetails(NewsConIdMkt(ni) + i, kMktSeedSymbols[i]);
    } else {
        printf("[IB] Reconnected — re-subscribing all windows.\n");
        g_IBClient->ReqAccountUpdates(true, g_selectedAccount);
        g_IBClient->ReqPositions();
        g_IBClient->ReqAccountSummary(ACCT_SUMMARY_REQID, ACCT_SUMMARY_TAGS);
        g_IBClient->ReqOpenOrders();
        g_IBClient->ReqAllOpenOrders();
        g_IBClient->ReqExecutions(8001);
        for (auto& se : g_scannerEntries)
            g_IBClient->CancelScannerData(se.activeScanId);

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
        for (auto& te : g_tradingEntries) {
            if (!te.win) continue;
            std::string sym = te.win->getSymbol();
            if (!sym.empty()) ApplyTradingSymbol(te, sym);
        }

        g_IBClient->SubscribeToNews(NEWS_RT_REQID);
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni)
            for (int i = 0; i < kMktSeedCount; ++i)
                g_IBClient->ReqContractDetails(NewsConIdMkt(ni) + i, kMktSeedSymbols[i]);
    }

    // Re-subscribe to TWS display groups if sync was already enabled before (re)connect.
    if (g_twsGroupSync)
        for (int g = 1; g <= 4; ++g)
            g_IBClient->SubscribeToGroupEvents(8060 + g, g);
}

// ============================================================================
// IB API connection wiring
// ============================================================================
static void WireIBCallbacks() {
    // ── Managed accounts (fires before nextValidId so account is known early) ─
    g_IBClient->onManagedAccounts = [](const std::vector<std::string>& accts) {
        g_managedAccounts = accts;
        if (accts.size() == 1)
            g_selectedAccount = accts[0];
        printf("[IB] Managed accounts: %zu account(s)\n", accts.size());
    };

    // ── Connection state ──────────────────────────────────────────────────
    g_IBClient->onConnectionChanged = [](bool connected, const std::string& info) {
        if (connected) {
            bool isReconnect = (g_Login.state == ConnectionState::LostConnection);
            g_Login.connectedAs = g_Login.isLive ? "[LIVE]" : "[PAPER]";
            printf("[IB] %s\n", info.c_str());

            if (g_selectedAccount.empty() && g_managedAccounts.size() > 1) {
                // Multi-account: defer window creation until user picks account.
                g_Login.state      = ConnectionState::SelectingAccount;
                g_pendingReconnect = isReconnect;
            } else {
                FinishConnect(isReconnect);
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

        // Watchlist entries (reqIds 7000–7999) — self-routing by reqId
        if (tickerId >= 7000 && tickerId < 8000) {
            for (auto& we : g_watchlistEntries)
                if (we.win) we.win->OnTickSize(tickerId, field, size);
            return;
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

        // Watchlist entries (reqIds 7000–7999) — self-routing by reqId
        if (tickerId >= 7000 && tickerId < 8000) {
            for (auto& we : g_watchlistEntries)
                if (we.win) we.win->OnTickPrice(tickerId, field, price);
            return;
        }

        auto it = g_tickerSymbols.find(tickerId);
        if (it == g_tickerSymbols.end()) return;
        const std::string& sym = it->second;

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
        // Capture account ID on first receipt and subscribe account-level P&L.
        if (!acct.empty() && g_accountId.empty()) {
            g_accountId = acct;
        }
        if (!g_pnlSubscribed && !g_selectedAccount.empty() && g_IBClient) {
            g_pnlSubscribed = true;
            g_IBClient->ReqPnL(9000, g_selectedAccount);
        }
    };

    // ── Account summary (base currency via reqAccountSummary) ─────────────
    // tag="Currency", value="USD" — this is the authoritative source.
    g_IBClient->onAccountSummary = [](const std::string& tag, const std::string& value,
                                      const std::string& currency) {
        if (!g_PortfolioWindow) return;
        // Route every financial tag through OnAccountValue so the portfolio
        // header always has live data even when reqAccountUpdates is slow or
        // delivers currency-suffixed variants on live accounts.
        g_PortfolioWindow->OnAccountValue(tag, value, currency, g_selectedAccount);
    };

    // ── Real-time P&L ─────────────────────────────────────────────────────
    g_IBClient->onPnL = [](int /*reqId*/, double daily, double unrealized, double realized) {
        if (g_PortfolioWindow) g_PortfolioWindow->OnPnL(daily, unrealized, realized);
    };

    g_IBClient->onPnLSingle = [](int reqId, double daily,
                                  double /*unrealized*/, double /*realized*/, double /*value*/) {
        auto sit = g_pnlReqIdToSymbol.find(reqId);
        if (sit == g_pnlReqIdToSymbol.end()) return;
        const std::string& sym = sit->second;
        // Update the shared position map so ChartWindow picks it up.
        auto pit = g_positions.find(sym);
        if (pit != g_positions.end()) {
            pit->second.dailyPnL = daily;
            UpdateAllChartPositions();
        }
        if (g_PortfolioWindow) g_PortfolioWindow->OnPnLSingle(reqId, sym, daily);
    };

    // ── Symbol autocomplete ───────────────────────────────────────────────
    ui::g_symbolSearchFn = [](const std::string& pattern) {
        if (g_IBClient) g_IBClient->ReqMatchingSymbols(8000, pattern);
    };
    g_IBClient->onSymbolSamples = [](int /*reqId*/,
                                      const std::vector<core::services::ContractDesc>& results) {
        std::vector<ui::SymbolResult> out;
        out.reserve(results.size());
        for (const auto& r : results)
            out.push_back({r.symbol, r.secType, r.primaryExch, r.currency});
        ui::UpdateSymbolSearchResults(std::move(out));
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
            // Subscribe WSH events for this position (reqPositions feed).
            if (pos.conId > 0 && g_WshCalendarWindow)
                g_WshCalendarWindow->SubscribeConId(static_cast<int>(pos.conId), pos.symbol);
        } else {
            for (auto& se : g_scannerEntries)
                if (se.win) se.win->SetPortfolioSymbols(g_portfolioSymbols);
            for (auto& ne : g_newsEntries)
                if (ne.win) ne.win->SetPortfolioSymbols(g_portfolioSymbols);
        }
    };

    // ── Portfolio updates (P&L etc.) ──────────────────────────────────────
    g_IBClient->onPortfolioUpdate = [](const core::Position& pos) {
        if (g_PortfolioWindow) g_PortfolioWindow->OnPositionUpdate(pos);
        // Preserve dailyPnL already populated by onPnLSingle before overwriting.
        auto it = g_positions.find(pos.symbol);
        double savedDailyPnL = (it != g_positions.end()) ? it->second.dailyPnL : 0.0;
        g_positions[pos.symbol] = pos;
        g_positions[pos.symbol].dailyPnL = savedDailyPnL;
        UpdateAllChartPositions();
        // Keep order book windows in sync with live position data
        for (auto& te : g_tradingEntries)
            if (te.win && te.win->getSymbol() == pos.symbol)
                te.win->SetPosition(pos.quantity, pos.avgCost);
        // Subscribe per-position real-time P&L the first time we see a conId.
        if (pos.conId > 0 && g_pnlSingleConIds.find(pos.conId) == g_pnlSingleConIds.end()
                && !g_accountId.empty() && g_IBClient) {
            int rid = g_pnlSingleNextReqId++;
            g_pnlSingleConIds[pos.conId]   = rid;
            g_pnlReqIdToSymbol[rid]        = pos.symbol;
            g_IBClient->ReqPnLSingle(rid, g_selectedAccount, "", static_cast<int>(pos.conId));
        }
        // Subscribe WSH events for this position in the calendar window.
        if (pos.conId > 0 && g_WshCalendarWindow)
            g_WshCalendarWindow->SubscribeConId(static_cast<int>(pos.conId), pos.symbol);
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
            if (te.win && te.win->getSymbol() == fill.symbol) te.win->OnFill(fill);
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
    g_IBClient->onQueriedFill = [](const core::Fill& fill) {
        if (g_OrdersWindow) g_OrdersWindow->OnQueriedFill(fill);
    };
    g_IBClient->onTickByTick = [](int reqId, const core::Tick& tick) {
        for (auto& te : g_tradingEntries)
            if (te.tickId == reqId && te.win) te.win->OnTickByTick(tick);
    };
    // ── Smart components / exchange routing ───────────────────────────────
    // reqId 8040–8049 = chart instances; 8050–8059 = trading instances.
    g_IBClient->onTickReqParams = [](int tickerId, const std::string& bboExchange) {
        auto applyToWindow = [&](auto& entries, int reqBase) {
            for (int i = 0; i < (int)entries.size(); ++i) {
                auto& e = entries[i];
                if (e.mktId != tickerId) continue;
                e.bboExchange = bboExchange;
                auto it = g_smartComponents.find(bboExchange);
                if (it != g_smartComponents.end()) {
                    std::vector<std::string> exch = {"SMART"};
                    for (const auto& r : it->second) exch.push_back(r.exchange);
                    if (e.win) e.win->SetExchangeList(exch);
                } else if (g_IBClient) {
                    g_IBClient->ReqSmartComponents(reqBase + i, bboExchange);
                }
                return true;
            }
            return false;
        };
        if (!applyToWindow(g_chartEntries,   8040))
              applyToWindow(g_tradingEntries, 8050);
    };
    g_IBClient->onSmartComponents = [](int reqId,
                                        const std::vector<core::SmartRoute>& routes) {
        std::vector<std::string> exch = {"SMART"};
        for (const auto& r : routes) exch.push_back(r.exchange);

        auto apply = [&](auto& entries, int reqBase) {
            int idx = reqId - reqBase;
            if (idx < 0 || idx >= (int)entries.size()) return false;
            auto& e = entries[idx];
            if (e.win) e.win->SetExchangeList(exch);
            if (!e.bboExchange.empty())
                g_smartComponents[e.bboExchange] = routes;
            return true;
        };
        if (!apply(g_chartEntries,   8040))
              apply(g_tradingEntries, 8050);
    };
    g_IBClient->onWshEvent = [](int reqId, const std::string& data) {
        // Chart per-symbol markers (8020–8029)
        WshData::WshEvent ev = WshData::ParseWshEvent(data);
        for (int ci = 0; ci < (int)g_chartEntries.size(); ++ci)
            if (reqId == ChartWshId(ci) && g_chartEntries[ci].win)
                g_chartEntries[ci].win->OnWshEvent(ev);
        // Calendar window aggregate view (8070–8199)
        if (reqId >= ui::WshCalendarWindow::kReqBase &&
            reqId <= ui::WshCalendarWindow::kReqEnd  && g_WshCalendarWindow)
            g_WshCalendarWindow->OnWshEvent(reqId, data);
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
        core::NewsItem item;
        item.id        = ++g_newsItemId;
        item.headline  = headline;
        item.source    = provider;
        item.summary   = articleId;   // transport: window extracts → m_itemArticleIds
        item.timestamp = ts;
        item.sentiment = core::NewsSentiment::Neutral;
        item.category  = core::NewsCategory::Market;
        for (auto& ne : g_newsEntries)
            if (ne.win) ne.win->OnMarketNewsItem(item);
    };

    // ── News — contract details → historical news chain ───────────────────
    g_IBClient->onContractConId = [](int reqId, long conId,
                                      const std::string& description,
                                      const std::string& secType,
                                      const std::string& primaryExch,
                                      const std::string& currency) {
        if (!g_IBClient) return;
        // IB may call contractDetails multiple times (one per exchange match).
        // Only use the first conId per reqId so we don't issue duplicate requests.

        // Cache symbol → conId for display-group outbound sync.
        for (const auto& ce : g_chartEntries)
            if (reqId == ce.wshId && ce.win)
                g_symbolConIds[ce.win->getSymbol()] = conId;

        // Watchlist contract details (reqIds 6900–6909, one per instance)
        for (int wi = 0; wi < (int)g_watchlistEntries.size(); ++wi) {
            if (reqId == WatchlistCdId(wi)) {
                auto& we = g_watchlistEntries[wi];
                if (we.win)
                    we.win->SetContractDetails(reqId, conId, description,
                                               secType, primaryExch, currency);
                return;
            }
        }

        // Chart WSH event markers (reqIds 8020–8029)
        for (int ci = 0; ci < (int)g_chartEntries.size(); ++ci) {
            auto& ce = g_chartEntries[ci];
            if (reqId == ChartWshId(ci)) {
                if (ce.wshConIdFired) return;
                ce.wshConIdFired = true;
                g_IBClient->ReqWshEventData(reqId, (int)conId);
                // Also subscribe this symbol in the calendar aggregate view.
                if (g_WshCalendarWindow && ce.win)
                    g_WshCalendarWindow->SubscribeConId(
                        static_cast<int>(conId), ce.win->getSymbol());
                return;
            }
        }

        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni) {
            auto& ne = g_newsEntries[ni];
            if (reqId == NewsStockConId(ni)) {
                if (ne.newsConIdFired[reqId]) return;
                ne.newsConIdFired[reqId] = true;
                g_IBClient->ReqHistoricalNews(NewsHistStock(ni), (int)conId, 30);
                return;
            }
            if (reqId >= NewsPortConId(ni) && reqId < NewsPortConId(ni) + 20) {
                if (ne.newsConIdFired[reqId]) return;
                ne.newsConIdFired[reqId] = true;
                g_IBClient->ReqHistoricalNews(NewsHistPort(ni) + (reqId - NewsPortConId(ni)), (int)conId, 10);
                return;
            }
            if (reqId >= NewsConIdMkt(ni) && reqId < NewsConIdMkt(ni) + kMktSeedCount) {
                if (ne.newsConIdFired[reqId]) return;
                ne.newsConIdFired[reqId] = true;
                g_IBClient->ReqHistoricalNews(NewsHistMkt(ni) + (reqId - NewsConIdMkt(ni)), (int)conId, 20);
                return;
            }
        }
    };

    g_IBClient->onHistoricalNews = [](int reqId, std::time_t ts,
                                      const std::string& provider,
                                      const std::string& articleId,
                                      const std::string& headline) {
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni) {
            auto& ne = g_newsEntries[ni];
            if (!ne.win) continue;
            core::NewsCategory cat;
            bool found = false;
            if (reqId == NewsHistStock(ni)) {
                cat = core::NewsCategory::Stock; found = true;
            } else if (reqId >= NewsHistMkt(ni) && reqId < NewsHistMkt(ni) + kMktSeedCount) {
                cat = core::NewsCategory::Market; found = true;
            } else if (reqId >= NewsHistPort(ni) && reqId < NewsHistPort(ni) + 20) {
                cat = core::NewsCategory::Portfolio; found = true;
            }
            if (!found) continue;
            core::NewsItem item;
            item.id        = ++g_histNewsId;
            item.headline  = headline;
            item.source    = provider;
            item.summary   = articleId;
            item.timestamp = ts;
            item.sentiment = core::NewsSentiment::Neutral;
            item.category  = cat;
            ne.win->OnHistoricalNewsItem(reqId, item);
            return;
        }
    };

    g_IBClient->onHistoricalNewsEnd = [](int reqId) {
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni) {
            auto& ne = g_newsEntries[ni];
            if (!ne.win) continue;
            if (reqId == NewsHistStock(ni) ||
                (reqId >= NewsHistMkt(ni)  && reqId < NewsHistMkt(ni)  + kMktSeedCount) ||
                (reqId >= NewsHistPort(ni) && reqId < NewsHistPort(ni) + 20)) {
                ne.win->OnHistoricalNewsEnd(reqId);
                return;
            }
        }
    };

    g_IBClient->onNewsArticle = [](int reqId, int /*articleType*/,
                                   const std::string& text) {
        for (auto& ne : g_newsEntries) {
            auto it = ne.artReqToItemId.find(reqId);
            if (it == ne.artReqToItemId.end()) continue;
            int itemId = it->second;
            ne.artReqToItemId.erase(it);
            if (ne.win) ne.win->OnArticleReceived(itemId, text);
            return;
        }
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
            (it->second.status == core::OrderStatus::Pending  ||
             it->second.status == core::OrderStatus::Working  ||
             it->second.status == core::OrderStatus::PartialFill) &&
             it->second.status != core::OrderStatus::PendingCancel) {
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
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni) {
            auto& ne = g_newsEntries[ni];
            if (!ne.win) continue;
            if (reqId == NewsHistStock(ni) || reqId == NewsStockConId(ni)) {
                ne.win->OnHistoricalNewsEnd(NewsHistStock(ni));
                break;
            }
            if (reqId >= NewsHistPort(ni) && reqId < NewsHistPort(ni) + 20) {
                ne.win->OnHistoricalNewsEnd(reqId);
                break;
            }
            if (reqId >= NewsPortConId(ni) && reqId < NewsPortConId(ni) + 20) {
                ne.win->OnHistoricalNewsEnd(NewsHistPort(ni) + (reqId - NewsPortConId(ni)));
                break;
            }
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

    // ── IB TWS Display Group sync ─────────────────────────────────────────
    g_IBClient->onDisplayGroupList = [](int /*reqId*/, const std::string& groups) {
        printf("[IB] Display groups available: %s\n", groups.c_str());
    };
    g_IBClient->onDisplayGroupUpdated = [](int reqId, const std::string& contractInfo) {
        // reqId 8061–8064 maps to G1–G4
        int groupId = reqId - 8060;  // 8061→1, 8062→2, ...
        if (groupId < 1 || groupId > 4) return;
        // Parse "symbol:secType:exchange:conId" — extract first colon-delimited field
        auto colon = contractInfo.find(':');
        std::string sym = (colon != std::string::npos)
                          ? contractInfo.substr(0, colon)
                          : contractInfo;
        if (!sym.empty())
            BroadcastGroupSymbol(groupId, sym);
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
        if (g_pnlSubscribed)
            g_IBClient->CancelPnL(9000);
        for (auto& [conId, rid] : g_pnlSingleConIds)
            g_IBClient->CancelPnLSingle(rid);
        g_IBClient->Disconnect();
        delete g_IBClient;
        g_IBClient = nullptr;
    }
    g_managedAccounts.clear();
    g_selectedAccount.clear();
    g_pendingReconnect = false;
    g_accountId.clear();
    g_pnlSubscribed = false;
    g_pnlSingleConIds.clear();
    g_pnlReqIdToSymbol.clear();
    g_pnlSingleNextReqId = 9001;
    ui::g_symbolSearchFn = nullptr;
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
    ImGuiViewport* vp  = ImGui::GetMainViewport();
    const float    W   = vp->Size.x;
    const float    H   = vp->Size.y - kTitleBarH;
    const float    leftW  = W * 0.42f;
    const float    rightW = W - leftW;

    // ── Full-screen host window (below custom title bar) ──────────────────────
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + kTitleBarH));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.051f, 0.067f, 0.090f, 1.0f));

    ImGui::Begin("##login", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      wp  = ImGui::GetWindowPos();

    // ── Left panel background + decorations ──────────────────────────────────
    dl->AddRectFilled(wp, ImVec2(wp.x + leftW, wp.y + H), IM_COL32(7, 10, 16, 255));

    // Subtle grid overlay
    for (int i = 1; i < 9; i++) {
        float x = wp.x + leftW * (i / 9.0f);
        dl->AddLine(ImVec2(x, wp.y), ImVec2(x, wp.y + H), IM_COL32(0, 180, 216, 7));
    }
    for (int i = 1; i < 12; i++) {
        float y = wp.y + H * (i / 12.0f);
        dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + leftW, y), IM_COL32(0, 180, 216, 7));
    }

    // Top-left corner accent brackets
    const float bk = 28.0f, bkT = 2.0f, bkOff = 32.0f;
    dl->AddRectFilled(ImVec2(wp.x + bkOff,      wp.y + bkOff),
                      ImVec2(wp.x + bkOff + bk,  wp.y + bkOff + bkT), IM_COL32(0,180,216,180));
    dl->AddRectFilled(ImVec2(wp.x + bkOff,      wp.y + bkOff),
                      ImVec2(wp.x + bkOff + bkT, wp.y + bkOff + bk),  IM_COL32(0,180,216,180));
    // Bottom-right corner accent brackets (relative to left panel)
    dl->AddRectFilled(ImVec2(wp.x + leftW - bkOff - bk, wp.y + H - bkOff - bkT),
                      ImVec2(wp.x + leftW - bkOff,       wp.y + H - bkOff),       IM_COL32(0,180,216,180));
    dl->AddRectFilled(ImVec2(wp.x + leftW - bkOff - bkT, wp.y + H - bkOff - bk),
                      ImVec2(wp.x + leftW - bkOff,        wp.y + H - bkOff),      IM_COL32(0,180,216,180));

    // Thin cyan separator between panels
    dl->AddRectFilled(ImVec2(wp.x + leftW - 1, wp.y),
                      ImVec2(wp.x + leftW + 1, wp.y + H), IM_COL32(0, 180, 216, 70));

    // ── Left child: branding ──────────────────────────────────────────────────
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##lp", ImVec2(leftW, H), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* ldl  = ImGui::GetWindowDrawList();
    ImVec2      lwp  = ImGui::GetWindowPos();

    // Vertical cyan accent bar left of brand text
    const float brandTopY = H * 0.36f;
    ldl->AddRectFilled(ImVec2(lwp.x + 58.0f, lwp.y + brandTopY),
                       ImVec2(lwp.x + 62.0f, lwp.y + brandTopY + 72.0f),
                       IM_COL32(0, 180, 216, 220));

    // Large IBKR logotype
    ImGui::SetCursorPos(ImVec2(76.0f, brandTopY));
    ImGui::SetWindowFontScale(2.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.706f, 0.847f, 1.0f));
    ImGui::Text("IBKR");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);

    ImGui::SetCursorPos(ImVec2(78.0f, brandTopY + 42.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.902f, 0.929f, 0.953f, 0.92f));
    ImGui::Text("TRADING TERMINAL");
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(78.0f, brandTopY + 62.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("Professional Market Access");
    ImGui::PopStyleColor();

    // Horizontal rule below tagline
    float ruleY = lwp.y + brandTopY + 82.0f;
    ldl->AddLine(ImVec2(lwp.x + 78.0f, ruleY),
                 ImVec2(lwp.x + leftW * 0.72f, ruleY),
                 IM_COL32(0, 180, 216, 55), 1.0f);

    // Bottom attribution
    ImGui::SetCursorPos(ImVec2(78.0f, H - 32.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.30f, 0.35f, 1.0f));
    ImGui::Text("Interactive Brokers LLC");
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg

    // ── Right panel: covers the full right side ──────────────────────────────
    const float formW    = std::min(380.0f, rightW - 80.0f);
    const float estFormH = 355.0f;
    const float formX    = (rightW - formW) * 0.5f;
    const float formY    = std::max(50.0f, (H - estFormH) * 0.5f);

    ImGui::SetCursorPos(ImVec2(leftW, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.051f, 0.067f, 0.090f, 1.0f));
    ImGui::BeginChild("##rp", ImVec2(rightW, H), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Form inner child — explicitly positioned at the visual center
    ImGui::SetCursorPos(ImVec2(formX, formY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8.0f, 8.0f));
    ImGui::BeginChild("##form", ImVec2(formW, H - formY - 20.0f), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* fdl = ImGui::GetWindowDrawList();

    // Section title + cyan underline
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("CONNECT TO IBKR");
    ImGui::PopStyleColor();
    {
        ImVec2 sp = ImGui::GetCursorScreenPos();
        fdl->AddLine(ImVec2(sp.x, sp.y), ImVec2(sp.x + formW, sp.y),
                     IM_COL32(0, 180, 216, 60), 1.0f);
    }
    ImGui::Spacing();
    ImGui::Spacing();

    bool isConnecting = (g_Login.state == ConnectionState::Connecting);
    if (isConnecting) ImGui::BeginDisabled();

    bool changedType = false;

    // Segmented Paper | Live toggle
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 8.0f));
    const float segW = (formW - 2.0f) * 0.5f;
    auto segBtn = [&](const char* label, bool selected, bool isLiveBtn) -> bool {
        ImVec4 bg, bgH, bgA, fg;
        if (selected && isLiveBtn) {
            bg = bgH = bgA = ImVec4(0.451f, 0.094f, 0.094f, 1.0f);
            fg = ImVec4(1.000f, 0.420f, 0.420f, 1.0f);
        } else if (selected) {
            bg = bgH = bgA = ImVec4(0.000f, 0.353f, 0.424f, 1.0f);
            fg = ImVec4(0.000f, 0.706f, 0.847f, 1.0f);
        } else {
            bg  = ImVec4(0.039f, 0.055f, 0.075f, 1.0f);
            bgH = ImVec4(0.094f, 0.129f, 0.176f, 1.0f);
            bgA = ImVec4(0.059f, 0.094f, 0.141f, 1.0f);
            fg  = ImVec4(0.420f, 0.471f, 0.522f, 1.0f);
        }
        ImGui::PushStyleColor(ImGuiCol_Button,        bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bgH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bgA);
        ImGui::PushStyleColor(ImGuiCol_Text,          fg);
        bool clicked = ImGui::Button(label, ImVec2(segW, 28.0f));
        ImGui::PopStyleColor(4);
        return clicked;
    };
    if (segBtn("  Paper  ", !g_Login.isLive, false)) { g_Login.isLive = false; changedType = true; }
    ImGui::SameLine(0.0f, 2.0f);
    if (segBtn("  Live   ",  g_Login.isLive, true))  { g_Login.isLive = true;  changedType = true; }
    ImGui::PopStyleVar(2);

    ImGui::Spacing();

    // API row
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("API");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 16.0f);
    int apiIdx = (int)g_Login.apiType;
    if (ImGui::RadioButton("TWS",        &apiIdx, 0)) { g_Login.apiType = ApiType::TWS;     changedType = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("IB Gateway", &apiIdx, 1)) { g_Login.apiType = ApiType::Gateway; changedType = true; }
    if (changedType) g_Login.UpdatePort();

    ImGui::Spacing();
    {
        ImVec2 sp = ImGui::GetCursorScreenPos();
        fdl->AddLine(ImVec2(sp.x, sp.y), ImVec2(sp.x + formW, sp.y),
                     IM_COL32(48, 55, 62, 255), 1.0f);
    }
    ImGui::Spacing();
    ImGui::Spacing();

    // HOST label + input
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("HOST");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##host", g_Login.host, sizeof(g_Login.host));
    ImGui::Spacing();

    // PORT + CLIENT ID side by side
    const float halfW = (formW - 12.0f) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("PORT");
    ImGui::SameLine(0.0f, halfW - ImGui::CalcTextSize("PORT").x + 12.0f);
    ImGui::Text("CLIENT ID");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(halfW);
    ImGui::InputInt("##port", &g_Login.port, 0);
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetNextItemWidth(halfW);
    ImGui::InputInt("##cid", &g_Login.clientId, 1);

    if (isConnecting) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Spacing();

    // Info note
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.300f, 0.355f, 0.400f, 1.0f));
    ImGui::PushTextWrapPos(formW);
    ImGui::TextWrapped("IB Gateway or TWS must be running with API enabled. "
                       "Credentials are managed by TWS/Gateway.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Error banner
    if (g_Login.state == ConnectionState::Error) {
        ImVec2 bMin = ImGui::GetCursorScreenPos();
        ImVec2 bMax = ImVec2(bMin.x + formW, bMin.y + 28.0f);
        fdl->AddRectFilled(bMin, bMax, IM_COL32(110, 18, 18, 200), 3.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::Text("  ! %s", g_Login.errorMsg.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // Connect button / progress bar
    ImGui::Spacing();
    if (isConnecting) {
        using namespace std::chrono;
        static auto s_connectStart = steady_clock::now();
        float t = std::fmod((float)duration_cast<milliseconds>(
            steady_clock::now() - s_connectStart).count() / 1500.0f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.0f, 0.706f, 0.847f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,       ImVec4(0.039f, 0.055f, 0.075f, 1.0f));
        ImGui::ProgressBar(t, ImVec2(-1, 36.0f), "Connecting...");
        ImGui::PopStyleColor(2);
    } else {
        bool live = g_Login.isLive;
        ImVec4 bC = live ? ImVec4(0.55f,0.12f,0.00f,1.0f) : ImVec4(0.00f,0.353f,0.424f,1.0f);
        ImVec4 bH = live ? ImVec4(0.75f,0.18f,0.00f,1.0f) : ImVec4(0.00f,0.471f,0.565f,1.0f);
        ImVec4 bA = live ? ImVec4(0.38f,0.08f,0.00f,1.0f) : ImVec4(0.00f,0.235f,0.282f,1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,        bC);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bA);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        const char* lbl = live ? "Connect  —  Live Account" : "Connect  —  Paper Account";
        if (ImGui::Button(lbl, ImVec2(-1, 38.0f))) StartConnect();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        if (live) {
            ImGui::Spacing();
            ImVec2 wMin = ImGui::GetCursorScreenPos();
            ImVec2 wMax = ImVec2(wMin.x + formW, wMin.y + 28.0f);
            fdl->AddRectFilled(wMin, wMax, IM_COL32(75, 38, 0, 180), 3.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.0f, 1.0f));
            ImGui::Text("  ! LIVE — real orders will be executed");
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);  // form WindowPadding + ItemSpacing

    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg (##rp)
    ImGui::PopStyleVar();   // ##rp WindowPadding

    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
    ImGui::PopStyleVar(2);  // WindowPadding + WindowBorderSize

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
    if (!g_newsEntries.empty() && g_newsEntries[0].win) {
        g_newsEntries[0].win->open() = p.news.visible;
        g_newsEntries[0].win->setGroupId(p.news.groupId);
    }
    if (g_PortfolioWindow) { g_PortfolioWindow->open() = p.portfolio.visible; }
    if (g_OrdersWindow)    { g_OrdersWindow->open()    = p.orders.visible; }
    // Reset group state so the next symbol change re-broadcasts correctly
    for (auto& gs : g_groups) gs.symbol.clear();
}

// ============================================================================
// Trading UI (post-login)
// ============================================================================
// ============================================================================
// Window resize (borderless window — manual edge/corner drag)
// ============================================================================
static void HandleWindowResize() {
    // No resize while the title bar is being dragged, or when maximized
    if (g_tbDragging) return;
    if (glfwGetWindowAttrib(g_AppWindow, GLFW_MAXIMIZED)) return;

    constexpr int kEdge   = 5;  // px — edge-only detection zone
    constexpr int kCorner = 16; // px — corner detection zone (must be > kEdge)

    double mx, my;
    glfwGetCursorPos(g_AppWindow, &mx, &my);  // cursor relative to window client area

    int ww, wh;
    glfwGetWindowSize(g_AppWindow, &ww, &wh);

    // Edge flags (narrow zone)
    const bool nearL = (mx < kEdge);
    const bool nearR = (mx > ww - kEdge);
    const bool nearB = (my > wh - kEdge);
    // Top edge excluded — title bar owns it (would conflict with drag-to-move).

    // Corner flags (wider zone) — checked first so corners take priority over edges
    const bool cnrL = (mx < kCorner);
    const bool cnrR = (mx > ww - kCorner);
    const bool cnrB = (my > wh - kCorner);

    // bit0=Left, bit1=Right, bit3=Bottom — corners use the wider zone
    int edge = 0;
    if      (cnrR && cnrB) edge = 2 | 8; // BR
    else if (cnrL && cnrB) edge = 1 | 8; // BL
    else if (nearR)        edge = 2;      // right edge
    else if (nearL)        edge = 1;      // left edge
    else if (nearB)        edge = 8;      // bottom edge

    // Resize cursors — GLFW 3.4 names with 3.3 fallback
    static GLFWcursor* s_curEW = glfwCreateStandardCursor(
#ifdef GLFW_RESIZE_EW_CURSOR
        GLFW_RESIZE_EW_CURSOR
#else
        GLFW_HRESIZE_CURSOR
#endif
    );
    static GLFWcursor* s_curNS = glfwCreateStandardCursor(
#ifdef GLFW_RESIZE_NS_CURSOR
        GLFW_RESIZE_NS_CURSOR
#else
        GLFW_VRESIZE_CURSOR
#endif
    );
#ifdef GLFW_RESIZE_NWSE_CURSOR
    static GLFWcursor* s_curNWSE = glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR);
    static GLFWcursor* s_curNESW = glfwCreateStandardCursor(GLFW_RESIZE_NESW_CURSOR);
#else
    static GLFWcursor* s_curNWSE = s_curEW;
    static GLFWcursor* s_curNESW = s_curEW;
#endif

    // Update cursor shape when not already dragging
    static bool s_resizing = false;
    if (!s_resizing) {
        GLFWcursor* cur = nullptr;
        switch (edge) {
            case 1: case 2:  cur = s_curEW;   break; // L / R
            case 8:          cur = s_curNS;   break; // B
            case 9:          cur = s_curNESW; break; // BL
            case 10:         cur = s_curNWSE; break; // BR
            default:         cur = nullptr;   break;
        }
        glfwSetCursor(g_AppWindow, cur);
    }

    // Track drag state using global mouse coords (io.MousePos = screen space)
    static int  s_edge = 0;
    static float s_startMX = 0, s_startMY = 0;
    static int   s_startWX = 0, s_startWY = 0, s_startWW = 0, s_startWH = 0;

    const ImGuiIO& io = ImGui::GetIO();
    const bool lmb = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (!lmb) {
        s_resizing = false;
        s_edge     = 0;
    }

    if (!s_resizing && lmb && edge != 0) {
        s_resizing = true;
        s_edge     = edge;
        s_startMX  = io.MousePos.x;
        s_startMY  = io.MousePos.y;
        glfwGetWindowPos (g_AppWindow, &s_startWX, &s_startWY);
        glfwGetWindowSize(g_AppWindow, &s_startWW, &s_startWH);
    }

    if (s_resizing) {
        constexpr int kMinW = 640, kMinH = 400;
        const int dx = (int)(io.MousePos.x - s_startMX);
        const int dy = (int)(io.MousePos.y - s_startMY);

        int nx = s_startWX, ny = s_startWY;
        int nw = s_startWW, nh = s_startWH;

        if (s_edge & 2) nw = std::max(kMinW, s_startWW + dx);  // right
        if (s_edge & 8) nh = std::max(kMinH, s_startWH + dy);  // bottom
        if (s_edge & 1) {                                        // left
            nw = std::max(kMinW, s_startWW - dx);
            nx = s_startWX + (s_startWW - nw);
        }

        glfwSetWindowPos (g_AppWindow, nx, ny);
        glfwSetWindowSize(g_AppWindow, nw, nh);
    }
}

// ============================================================================
// Custom Title Bar
// ============================================================================
static void RenderCustomTitleBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiIO&       io = ImGui::GetIO();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kTitleBarH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.016f, 0.020f, 0.031f, 1.0f)); // #040508

    ImGui::Begin("##titlebar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2  wp = ImGui::GetWindowPos();
    float   W  = vp->Size.x;

    // Bottom border line
    dl->AddLine(ImVec2(wp.x, wp.y + kTitleBarH - 1.0f),
                ImVec2(wp.x + W, wp.y + kTitleBarH - 1.0f),
                IM_COL32(0, 180, 216, 50), 1.0f);

    // ── Left: branding ────────────────────────────────────────────────────────
    // Cyan accent bar
    dl->AddRectFilled(ImVec2(wp.x + 10.0f, wp.y + 9.0f),
                      ImVec2(wp.x + 13.0f, wp.y + kTitleBarH - 9.0f),
                      IM_COL32(0, 180, 216, 240));

    float textY = (kTitleBarH - ImGui::GetFontSize()) * 0.5f;
    ImGui::SetCursorPos(ImVec2(20.0f, textY));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.706f, 0.847f, 1.0f));
    ImGui::Text("IBKR");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetCursorPosY(textY);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.61f, 0.67f, 1.0f));
    ImGui::Text("TRADING TERMINAL");
    ImGui::PopStyleColor();

    // ── Right: window control buttons ────────────────────────────────────────
    const float btnW = 46.0f;
    const float btnH = kTitleBarH;
    float btnX = W - btnW * 3.0f;

    // Returns true if clicked; fills outHovered
    auto ctrlBtn = [&](const char* id, float x, bool isClose,
                       bool& outHovered) -> bool {
        ImGui::SetCursorPos(ImVec2(x, 0.0f));
        ImGui::InvisibleButton(id, ImVec2(btnW, btnH));
        outHovered       = ImGui::IsItemHovered();
        bool active      = ImGui::IsItemActive();
        bool clicked     = ImGui::IsItemClicked();
        ImU32 bg = 0;
        if      (active)       bg = isClose ? IM_COL32(180, 28, 28, 255)
                                            : IM_COL32(55, 68, 85, 255);
        else if (outHovered)   bg = isClose ? IM_COL32(198, 40, 40, 230)
                                            : IM_COL32(38, 50, 65, 255);
        if (bg)
            dl->AddRectFilled(ImVec2(wp.x + x, wp.y),
                              ImVec2(wp.x + x + btnW, wp.y + btnH), bg);
        return clicked;
    };

    bool hMin = false, hMax = false, hClose = false;

    // Minimize
    if (ctrlBtn("##min", btnX, false, hMin))
        glfwIconifyWindow(g_AppWindow);
    {
        ImU32 ic = hMin ? IM_COL32(220, 230, 240, 255) : IM_COL32(140, 155, 170, 190);
        float cx = wp.x + btnX + btnW * 0.5f;
        float cy = wp.y + btnH * 0.5f + 2.0f;
        dl->AddLine(ImVec2(cx - 5.0f, cy), ImVec2(cx + 5.0f, cy), ic, 1.5f);
    }

    // Maximize / restore
    btnX += btnW;
    if (ctrlBtn("##max", btnX, false, hMax)) {
        if (glfwGetWindowAttrib(g_AppWindow, GLFW_MAXIMIZED))
            glfwRestoreWindow(g_AppWindow);
        else
            glfwMaximizeWindow(g_AppWindow);
    }
    {
        ImU32 ic = hMax ? IM_COL32(220, 230, 240, 255) : IM_COL32(140, 155, 170, 190);
        float cx = wp.x + btnX + btnW * 0.5f;
        float cy = wp.y + btnH * 0.5f;
        bool maximized = glfwGetWindowAttrib(g_AppWindow, GLFW_MAXIMIZED);
        if (maximized) {
            // Restore icon: two overlapping squares
            dl->AddRect(ImVec2(cx - 3.0f, cy - 5.0f),
                        ImVec2(cx + 5.0f, cy + 3.0f), ic, 0.0f, 0, 1.0f);
            dl->AddRect(ImVec2(cx - 5.0f, cy - 3.0f),
                        ImVec2(cx + 3.0f, cy + 5.0f), ic, 0.0f, 0, 1.0f);
        } else {
            // Maximize icon: single square
            dl->AddRect(ImVec2(cx - 5.0f, cy - 5.0f),
                        ImVec2(cx + 5.0f, cy + 5.0f), ic, 0.0f, 0, 1.0f);
        }
    }

    // Close
    btnX += btnW;
    if (ctrlBtn("##close", btnX, true, hClose))
        glfwSetWindowShouldClose(g_AppWindow, GLFW_TRUE);
    {
        ImU32 ic = hClose ? IM_COL32(255, 255, 255, 255) : IM_COL32(140, 155, 170, 190);
        float cx = wp.x + btnX + btnW * 0.5f;
        float cy = wp.y + btnH * 0.5f;
        dl->AddLine(ImVec2(cx - 5.0f, cy - 5.0f),
                    ImVec2(cx + 5.0f, cy + 5.0f), ic, 1.5f);
        dl->AddLine(ImVec2(cx + 5.0f, cy - 5.0f),
                    ImVec2(cx - 5.0f, cy + 5.0f), ic, 1.5f);
    }

    // ── Drag to move ─────────────────────────────────────────────────────────
    static float s_dragStartMX = 0, s_dragStartMY = 0;
    static int   s_dragStartWX = 0, s_dragStartWY = 0;

    bool overBtns = (io.MousePos.x >= wp.x + W - btnW * 3.0f);

    if (!overBtns && ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_tbDragging   = true;
        s_dragStartMX  = io.MousePos.x;
        s_dragStartMY  = io.MousePos.y;
        glfwGetWindowPos(g_AppWindow, &s_dragStartWX, &s_dragStartWY);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        g_tbDragging = false;
    if (g_tbDragging) {
        glfwSetWindowPos(g_AppWindow,
            s_dragStartWX + (int)(io.MousePos.x - s_dragStartMX),
            s_dragStartWY + (int)(io.MousePos.y - s_dragStartMY));
    }

    // Double-click to maximize / restore
    if (!overBtns && ImGui::IsWindowHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (glfwGetWindowAttrib(g_AppWindow, GLFW_MAXIMIZED))
            glfwRestoreWindow(g_AppWindow);
        else
            glfwMaximizeWindow(g_AppWindow);
    }

    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
    ImGui::PopStyleVar(2);  // WindowPadding + WindowBorderSize
}

// ============================================================================
// Settings window
// ============================================================================
// Subscribe / unsubscribe from IB TWS display groups G1–G4 (reqIds 8061–8064).
static void SetTwsGroupSync(bool enable) {
    g_twsGroupSync = enable;
    if (!g_IBClient || !g_IBClient->IsConnected()) return;
    if (enable) {
        for (int g = 1; g <= 4; ++g)
            g_IBClient->SubscribeToGroupEvents(8060 + g, g);
    } else {
        for (int g = 1; g <= 4; ++g)
            g_IBClient->UnsubscribeFromGroupEvents(8060 + g);
    }
}

static void RenderSettingsWindow() {
    if (!g_settingsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(300, 180), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Settings", &g_settingsOpen,
            ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Font Size");
    ImGui::Spacing();

    static const char* kLabels[] = { "Small", "Medium", "Large" };
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0, 16);
        if (ImGui::RadioButton(kLabels[i], (int)g_fontSize == i)) {
            g_fontSize = static_cast<FontSize>(i);
            ImGui::GetIO().FontGlobalScale = kFontScales[i];
            ImGui::GetStyle() = g_baseStyle;
            ImGui::GetStyle().ScaleAllSizes(kFontScales[i]);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("TWS Integration");
    ImGui::Spacing();

    bool syncVal = g_twsGroupSync;
    if (ImGui::Checkbox("Sync with TWS Display Groups", &syncVal))
        SetTwsGroupSync(syncVal);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "When ON: this app's G1–G4 groups stay in sync with\n"
            "TWS linked-window groups 1–4 (bidirectional).\n"
            "Requires IB Gateway / TWS with API access enabled.");

    ImGui::End();
}

// ============================================================================
// Trading UI
// ============================================================================
static void RenderTradingUI() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + kTitleBarH));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - kTitleBarH));
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDecoration  | ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;

    // Taller menu bar via FramePadding — scale with font size so Large mode doesn't clip
    const float kMenuBarScale = ImGui::GetFontSize() / 13.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(6.0f * kMenuBarScale, 9.0f * kMenuBarScale));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.020f, 0.027f, 0.039f, 1.0f)); // #050709

    if (ImGui::Begin("##TradingHost", nullptr, hostFlags)) {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Disconnect")) Disconnect();
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(g_AppWindow, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Windows")) {
                if (ImGui::BeginMenu("IBKR")) {
                    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
                    // Per-instance chart windows
                    for (auto& ce : g_chartEntries) {
                        if (!ce.win) continue;
                        char lbl[64];
                        const std::string sym = ce.win->getSymbol();
                        const int gid = ce.win->groupId();
                        std::snprintf(lbl, sizeof(lbl), "Chart %s %s###chart_menu_%d",
                            sym.empty() ? "--" : sym.c_str(),
                            gid > 0 ? ("G" + std::to_string(gid)).c_str() : "G-",
                            ce.win->instanceId());
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
                        char lbl[64];
                        const std::string sym = te.win->getSymbol();
                        const int gid = te.win->groupId();
                        std::snprintf(lbl, sizeof(lbl), "Order Book %s %s###ob_menu_%d",
                            sym.empty() ? "--" : sym.c_str(),
                            gid > 0 ? ("G" + std::to_string(gid)).c_str() : "G-",
                            te.win->instanceId());
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
                        char lbl[64];
                        const int gid = se.win->groupId();
                        std::snprintf(lbl, sizeof(lbl), "Scanner %s %s###sc_menu_%d",
                            se.win->getPresetLabel(),
                            gid > 0 ? ("G" + std::to_string(gid)).c_str() : "G-",
                            se.win->instanceId());
                        ImGui::MenuItem(lbl, nullptr, &se.win->open());
                    }
                    if ((int)g_scannerEntries.size() < kMaxMultiWin) {
                        if (ImGui::MenuItem("+ New Scanner"))
                            SpawnScannerWindow((int)g_scannerEntries.size());
                    }
                    ImGui::Separator();
                    // Per-instance news windows
                    for (auto& ne : g_newsEntries) {
                        if (!ne.win) continue;
                        char lbl[64];
                        const char* sym = ne.win->getSymbol();
                        const int gid = ne.win->groupId();
                        char grp[8];
                        if (gid > 0) std::snprintf(grp, sizeof(grp), "G%d", gid);
                        else         std::strncpy(grp, "G-", sizeof(grp));
                        std::snprintf(lbl, sizeof(lbl), "News %s %s###news_menu_%d",
                            sym[0] == '\0' ? "--" : sym, grp, ne.win->instanceId());
                        ImGui::MenuItem(lbl, nullptr, &ne.win->open());
                    }
                    if ((int)g_newsEntries.size() < kMaxMultiWin) {
                        if (ImGui::MenuItem("+ New News"))
                            SpawnNewsWindow((int)g_newsEntries.size());
                    }
                    ImGui::Separator();
                    // Per-instance watchlist windows
                    for (auto& we : g_watchlistEntries) {
                        if (!we.win) continue;
                        char lbl[64];
                        const int gid = we.win->groupId();
                        std::snprintf(lbl, sizeof(lbl), "Watchlist %d %s###wl_menu_%d",
                            we.win->instanceId(),
                            gid > 0 ? ("G" + std::to_string(gid)).c_str() : "G-",
                            we.win->instanceId());
                        ImGui::MenuItem(lbl, nullptr, &we.win->open());
                    }
                    if ((int)g_watchlistEntries.size() < kMaxMultiWin) {
                        if (ImGui::MenuItem("+ New Watchlist"))
                            SpawnWatchlistWindow((int)g_watchlistEntries.size());
                    }
                    ImGui::Separator();
                    // Singleton windows
                    if (g_OrdersWindow)       ImGui::MenuItem("Orders",        nullptr, &g_OrdersWindow->open());
                    if (g_PortfolioWindow)    ImGui::MenuItem("Portfolio",     nullptr, &g_PortfolioWindow->open());
                    if (g_WshCalendarWindow)  ImGui::MenuItem("WSH Calendar",  nullptr, &g_WshCalendarWindow->open());
                    ImGui::PopItemFlag();
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Presets")) {
                for (int i = 0; i < kNumBuiltinPresets; i++) {
                    if (ImGui::MenuItem(kBuiltinPresets[i].name))
                        ApplyPreset(kBuiltinPresets[i]);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Settings"))
                g_settingsOpen = !g_settingsOpen;

            // Status indicator (right-aligned): [acct v]  DISCONNECTED  [LIVE]/[PAPER]
            const std::string& who = g_Login.connectedAs;
            bool lostConn = (g_Login.state == ConnectionState::LostConnection);
            static const char* kDiscLabel = " DISCONNECTED ";
            float discW = lostConn ? ImGui::CalcTextSize(kDiscLabel).x + 8.0f : 0.0f;
            float whoW  = ImGui::CalcTextSize(who.c_str()).x;

            // Account label/selector width
            std::string acctMenuLabel;
            float acctW = 0.0f;
            if (!g_selectedAccount.empty()) {
                if (g_managedAccounts.size() > 1)
                    acctMenuLabel = g_selectedAccount + " v";
                else
                    acctMenuLabel = g_selectedAccount;
                acctW = ImGui::CalcTextSize(acctMenuLabel.c_str()).x + 10.0f;
            }

            float totalR = discW + (acctW > 0 ? acctW + 6.0f : 0.0f) + whoW + 12.0f;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - totalR);

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

            if (!g_selectedAccount.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
                if (g_managedAccounts.size() > 1) {
                    if (ImGui::BeginMenu(acctMenuLabel.c_str())) {
                        for (const auto& acct : g_managedAccounts) {
                            bool sel = (acct == g_selectedAccount);
                            if (ImGui::MenuItem(acct.c_str(), nullptr, sel) && !sel) {
                                g_selectedAccount = acct;
                                if (g_IBClient) {
                                    g_IBClient->ReqAccountUpdates(false, "");
                                    g_IBClient->ReqAccountUpdates(true, g_selectedAccount);
                                    if (g_pnlSubscribed) {
                                        g_IBClient->CancelPnL(9000);
                                        g_pnlSubscribed = false;
                                    }
                                }
                            }
                        }
                        ImGui::EndMenu();
                    }
                } else {
                    ImGui::TextUnformatted(acctMenuLabel.c_str());
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            ImGui::PushStyleColor(ImGuiCol_Text,
                g_Login.isLive ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                               : ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
            ImGui::Text("%s", who.c_str());
            ImGui::PopStyleColor();

            ImGui::EndMenuBar();

            // Cyan accent line at the bottom of the menu bar
            {
                ImVec2 cs = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(ImGui::GetWindowPos().x, cs.y),
                    ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), cs.y),
                    IM_COL32(0, 180, 216, 80), 1.0f);
            }
        }
        ImGui::PopStyleVar();   // FramePadding

        ImGuiID dockId = ImGui::GetID("TradingDock");
        ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    }
    ImGui::PopStyleColor(); // MenuBarBg
    ImGui::End();

    // Render all window instances
    for (auto& ce : g_chartEntries)   if (ce.win) ce.win->Render();
    for (auto& te : g_tradingEntries) if (te.win) te.win->Render();
    for (auto& se : g_scannerEntries) if (se.win) se.win->Render();
    for (auto& ne : g_newsEntries)    if (ne.win) ne.win->Render();
    for (auto& we : g_watchlistEntries) if (we.win) we.win->Render();
    if (g_PortfolioWindow)   g_PortfolioWindow->Render();
    if (g_OrdersWindow)      g_OrdersWindow->Render();
    if (g_WshCalendarWindow) g_WshCalendarWindow->Render();
    RenderSettingsWindow();
}

// ============================================================================
// Top-level UI dispatcher
// ============================================================================
static void RenderAccountSelectorUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
    ImGui::Begin("Select Account##acctsel",
                 nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted("Multiple accounts found. Select one to continue:");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    for (const auto& acct : g_managedAccounts) {
        if (ImGui::Button(acct.c_str(), ImVec2(-1, 0))) {
            g_selectedAccount = acct;
            FinishConnect(g_pendingReconnect);
        }
    }
    ImGui::End();
}

static void RenderMainUI() {
    if (g_Login.state == ConnectionState::Connected ||
        g_Login.state == ConnectionState::LostConnection)
        RenderTradingUI();
    else if (g_Login.state == ConnectionState::SelectingAccount)
        RenderAccountSelectorUI();
    else
        RenderLoginWindow();
    RenderCustomTitleBar(); // always last so it renders on top of everything
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
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED,  GLFW_FALSE); // custom title bar replaces OS decoration
    g_AppWindow = glfwCreateWindow(1920, 1080, "IBKR Trading Terminal", nullptr, nullptr);
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

    // Font sizes are applied via FontGlobalScale at runtime (no atlas rebuild needed)
    // g_fonts[0..2] unused — scale factors applied in RenderSettingsWindow
    io.Fonts->AddFontDefault();

    // ── Terminal Dark theme ───────────────────────────────────────────────────
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& s = ImGui::GetStyle();

        // Geometry
        s.WindowRounding    = 4.0f;
        s.ChildRounding     = 4.0f;
        s.FrameRounding     = 2.0f;
        s.PopupRounding     = 4.0f;
        s.ScrollbarRounding = 2.0f;
        s.GrabRounding      = 2.0f;
        s.TabRounding       = 2.0f;
        s.WindowBorderSize  = 1.0f;
        s.FrameBorderSize   = 0.0f;
        s.PopupBorderSize   = 1.0f;
        s.WindowPadding     = ImVec2(10.0f, 8.0f);
        s.FramePadding      = ImVec2(6.0f, 3.0f);
        s.ItemSpacing       = ImVec2(6.0f, 4.0f);
        s.ItemInnerSpacing  = ImVec2(4.0f, 4.0f);
        s.ScrollbarSize     = 10.0f;
        s.GrabMinSize       = 8.0f;
        s.IndentSpacing     = 14.0f;

        // Palette
        //   bg0  = near-black window bg
        //   bg1  = panel / child bg (slightly lighter)
        //   bg2  = frame / input bg
        //   fg0  = primary text
        //   fg1  = dimmed text
        //   acc  = cyan accent
        //   bdr  = border
        ImVec4* c = s.Colors;

        c[ImGuiCol_WindowBg]             = ImVec4(0.051f, 0.067f, 0.090f, 1.000f); // #0D1117
        c[ImGuiCol_ChildBg]              = ImVec4(0.086f, 0.106f, 0.141f, 1.000f); // #161B24
        c[ImGuiCol_PopupBg]              = ImVec4(0.063f, 0.082f, 0.110f, 1.000f); // #10151C
        c[ImGuiCol_Border]               = ImVec4(0.188f, 0.216f, 0.243f, 1.000f); // #30373E
        c[ImGuiCol_BorderShadow]         = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);

        c[ImGuiCol_FrameBg]              = ImVec4(0.039f, 0.055f, 0.075f, 1.000f); // #0A0E13
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.094f, 0.129f, 0.176f, 1.000f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.059f, 0.094f, 0.141f, 1.000f);

        c[ImGuiCol_TitleBg]              = ImVec4(0.039f, 0.055f, 0.078f, 1.000f); // #0A0E14
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.000f, 0.176f, 0.243f, 1.000f); // #002D3E (cyan-dark)
        c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.039f, 0.055f, 0.078f, 0.800f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.027f, 0.039f, 0.055f, 1.000f); // #070A0E

        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.039f, 0.055f, 0.075f, 1.000f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.188f, 0.216f, 0.243f, 1.000f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.000f, 0.706f, 0.847f, 0.600f);
        c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);

        // Cyan accent: #00B4D8
        c[ImGuiCol_CheckMark]            = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_SliderGrab]           = ImVec4(0.000f, 0.600f, 0.720f, 1.000f);
        c[ImGuiCol_SliderGrabActive]     = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);

        c[ImGuiCol_Button]               = ImVec4(0.000f, 0.353f, 0.424f, 1.000f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.000f, 0.471f, 0.565f, 1.000f);
        c[ImGuiCol_ButtonActive]         = ImVec4(0.000f, 0.235f, 0.282f, 1.000f);

        c[ImGuiCol_Header]               = ImVec4(0.000f, 0.353f, 0.424f, 0.700f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.000f, 0.471f, 0.565f, 0.800f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);

        c[ImGuiCol_Separator]            = ImVec4(0.188f, 0.216f, 0.243f, 1.000f);
        c[ImGuiCol_SeparatorHovered]     = ImVec4(0.000f, 0.706f, 0.847f, 0.600f);
        c[ImGuiCol_SeparatorActive]      = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);

        c[ImGuiCol_ResizeGrip]           = ImVec4(0.000f, 0.353f, 0.424f, 0.400f);
        c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.000f, 0.706f, 0.847f, 0.600f);
        c[ImGuiCol_ResizeGripActive]     = ImVec4(0.000f, 0.706f, 0.847f, 0.900f);

        c[ImGuiCol_Tab]                  = ImVec4(0.051f, 0.082f, 0.118f, 1.000f);
        c[ImGuiCol_TabHovered]           = ImVec4(0.000f, 0.471f, 0.565f, 1.000f);
        c[ImGuiCol_TabSelected]          = ImVec4(0.000f, 0.353f, 0.424f, 1.000f);
        c[ImGuiCol_TabSelectedOverline]  = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_TabDimmed]            = ImVec4(0.039f, 0.055f, 0.078f, 1.000f);
        c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.051f, 0.082f, 0.118f, 1.000f);
        c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.000f, 0.353f, 0.424f, 1.000f);

        c[ImGuiCol_DockingPreview]       = ImVec4(0.000f, 0.706f, 0.847f, 0.400f);
        c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.027f, 0.039f, 0.055f, 1.000f);

        c[ImGuiCol_PlotLines]            = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_PlotLinesHovered]     = ImVec4(1.000f, 0.600f, 0.000f, 1.000f);
        c[ImGuiCol_PlotHistogram]        = ImVec4(0.000f, 0.600f, 0.720f, 1.000f);
        c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.000f, 0.600f, 0.000f, 1.000f);

        c[ImGuiCol_TableHeaderBg]        = ImVec4(0.027f, 0.043f, 0.063f, 1.000f);
        c[ImGuiCol_TableBorderStrong]    = ImVec4(0.188f, 0.216f, 0.243f, 1.000f);
        c[ImGuiCol_TableBorderLight]     = ImVec4(0.102f, 0.122f, 0.153f, 1.000f);
        c[ImGuiCol_TableRowBg]           = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);
        c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.000f, 1.000f, 1.000f, 0.030f);

        c[ImGuiCol_TextLink]             = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(0.000f, 0.706f, 0.847f, 0.300f);

        c[ImGuiCol_DragDropTarget]       = ImVec4(1.000f, 0.600f, 0.000f, 0.900f);
        c[ImGuiCol_NavCursor]            = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.000f, 1.000f, 1.000f, 0.700f);
        c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.800f, 0.800f, 0.800f, 0.200f);
        c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.000f, 0.000f, 0.000f, 0.600f);

        c[ImGuiCol_Text]                 = ImVec4(0.902f, 0.929f, 0.953f, 1.000f); // #E6EDF3
        c[ImGuiCol_TextDisabled]         = ImVec4(0.420f, 0.471f, 0.522f, 1.000f); // #6B7885
    }
    g_baseStyle = ImGui::GetStyle(); // snapshot before any scaling

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
        HandleWindowResize(); // override cursor after ImGui sets it
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
