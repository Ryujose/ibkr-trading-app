# Vulkan & ImGui Rules

## ImGui Version
- `v1.92.6-docking` (fetched via FetchContent)

## API — What Exists vs What Doesn't

| Call | Status |
|---|---|
| `init_info.PipelineInfoMain.RenderPass` | **Use this** (current API) |
| `init_info.RenderPass` | Deprecated — do not use |
| `ImGui_ImplVulkan_CreateDeviceObjects()` | **Removed** — does not exist |
| `ImGui_ImplVulkan_GetRenderPass()` | **Removed** — does not exist |

## Setup Pattern (matches official `example_glfw_vulkan`)

Always use the `ImGui_ImplVulkanH_Window` helper infrastructure:

```
SetupVulkan()        → instance, physical device, logical device, descriptor pool
SetupVulkanWindow()  → swapchain, render pass, framebuffers (via ImGui helper)
FrameRender()        → acquire image, record commands, submit
FramePresent()       → present swapchain image
```

Do NOT manage swapchain/framebuffers manually when using this helper — let it own them.

## Known Pitfalls

1. **Tab in variable name**: Watch for `g\tRenderPass` corruptions — use `g_RenderPass`
2. **Framebuffers**: Must be created via `SetupVulkanWindow()` — never pass raw image views as framebuffers
3. **Fence deadlock**: Fences must be created in signaled state OR waited only after first submit
4. **VkClearColorValue**: Assign via `.float32[i]` array members, not direct struct assignment

## System Requirements
- Vulkan SDK installed with ICD loaders configured
- `DISPLAY=:1` on this Linux machine
