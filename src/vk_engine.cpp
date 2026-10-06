//> includes
#include "vk_engine.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>
#include <vk_images.h>

// bootstrap lib
#include "VkBootstrap.h"

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include "vk_pipelines.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

#include <chrono>
#include <thread>
#include <algorithm>
#include <shared_mutex>


VulkanEngine* loadedEngine = nullptr;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }
void VulkanEngine::init()
{
    // only one engine initialization is allowed with the application.
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // We initialize SDL and create a window with it.
    SDL_Init(SDL_INIT_VIDEO);
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);

    _window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        _windowExtent.width,
        _windowExtent.height,
        window_flags);

    init_vulkan();

    init_swapchain();

    init_commands();

    init_sync_structures();

    init_descriptors();

    init_pipelines();

    init_imgui();

    // everything went fine
    _isInitialized = true;
}

void VulkanEngine::cleanup()
{
    if (_isInitialized) {

        // Make sure the gpu has stopped doing its things
        vkDeviceWaitIdle(_device);

        for (int i = 0; i < FRAME_OVERLAP; ++i) {
            vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr);

            // Destroy sync objects
            vkDestroyFence(_device, _frames[i]._renderFence, nullptr);
            vkDestroySemaphore(_device, _frames[i]._swapchainSemaphore, nullptr);

            _frames[i]._deletionQueue.flush();
        }

        for (const auto& s: _swapchainSemaphores) {
            vkDestroySemaphore(_device, s, nullptr);
        }

        _mainDeletionQueue.flush();

        destroy_swapchain();

        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);

        vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
        vkDestroyInstance(_instance, nullptr);

        SDL_DestroyWindow(_window);
    }

    // clear engine pointer
    loadedEngine = nullptr;
}

void VulkanEngine::draw()
{
    // Wait until the gpu has finished rendering last frame. Timeout of 1 second
    VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, true, 1000000000));
    VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence));

    // Request image from swapchain
    uint32_t swapchainImageIndex;
    VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex));

    VkCommandBuffer cmdBf = get_current_frame()._mainCommandBuffer;

    /* Now that we are sure that commands finished executing,
     * we can safely reset command buffer to begin recording again */
    VK_CHECK(vkResetCommandBuffer(cmdBf, 0));

    /* Begin command buffer recording
     * We will use this command buffer exactly once, so we let vulkan know that from flags  */
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    _drawExtent.width = _drawImage.imageExtent.width;
    _drawExtent.height = _drawImage.imageExtent.height;

    // Start the command buffer for recording
    VK_CHECK(vkBeginCommandBuffer(cmdBf, &cmdBeginInfo));

    // Transition main draw image into general layout so we can write to it
    // We will overwrite it all so we dont care about what was in the older layout
    vkutil::transition_image(cmdBf, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    draw_background(cmdBf);

    // Transition draw image and swapchain image into their correct transfer layouts
    vkutil::transition_image(cmdBf, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::transition_image(cmdBf, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // Execute a copy from the draw image into the swapchain
    vkutil::copy_image_to_image(cmdBf, _drawImage.image, _swapchainImages[swapchainImageIndex], _drawExtent, _swapchainExtent);

    // Set swapchain image layout to attachment optimal so we can draw to it
    vkutil::transition_image(cmdBf, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // draw imgui into the swapchain image
    draw_imgui(cmdBf, _swapchainImageViews[swapchainImageIndex]);

    // Set swapchain image layout to present so we can show it on the screen
    vkutil::transition_image(cmdBf, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // Finalize command buffer(we can no longer add commands but can execute it now)
    VK_CHECK(vkEndCommandBuffer(cmdBf));

    // Prepare the submission to the queue
    // We want to wait on the _presentSemaphore, as that semaphore is signaled when swapchain is ready
    // We will signal the _renderSemaphore, to signal that rendering has finished

    VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmdBf);

    VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, get_current_frame()._swapchainSemaphore);
    VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, _swapchainSemaphores[swapchainImageIndex]);

    VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, &signalInfo, &waitInfo);

    // Submit command buffer to queue and execute it
    // _renderFence will now block until graphics commands finish execution
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));

    // Prepare present
    // This will put the image we just rendered to into the visible window
    // We want to wait on the _renderSemaphore for that
    // As it is necessary that drawing commands have finished before the image is displayed to user

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.swapchainCount = 1;

    presentInfo.pWaitSemaphores = &_swapchainSemaphores[swapchainImageIndex];
    presentInfo.waitSemaphoreCount = 1;

    presentInfo.pImageIndices = &swapchainImageIndex;

    VK_CHECK(vkQueuePresentKHR(_graphicsQueue, &presentInfo));

    // Increase number of frames drawn
    ++_frameNumber;
}

void VulkanEngine::run()
{
    SDL_Event e;
    bool bQuit = false;

    // main loop
    while (!bQuit) {
        // Handle events on queue
        while (SDL_PollEvent(&e) != 0) {
            // close the window when user alt-f4s or clicks the X button
            if (e.type == SDL_QUIT)
                bQuit = true;

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    stop_rendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    stop_rendering = false;
                }
            }

            // Send SDL event to imgui for handling
            ImGui_ImplSDL2_ProcessEvent(&e);
        }

        // do not draw if we are minimized
        if (stop_rendering) {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Imgui new frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (ImGui::Begin("background"))
        {
            ComputeEffect& selected = backgroundEffects[currentBackgroundEffect];

            ImGui::Text("Selected effect: %s", selected.name);

            ImGui::SliderInt("Effect Index", &currentBackgroundEffect, 0, backgroundEffects.size() - 1);

            ImGui::InputFloat4("data1",(float*)& selected.data.data1);
            ImGui::InputFloat4("data2",(float*)& selected.data.data2);
            ImGui::InputFloat4("data3",(float*)& selected.data.data3);
            ImGui::InputFloat4("data4",(float*)& selected.data.data4);
        }
        ImGui::End();

        // Make imgui calculate internal draw structures
        ImGui::Render();

        // Our renderer's draw
        draw();
    }
}

void VulkanEngine::immediate_submit(std::function<void(VkCommandBuffer cmd)> &&function)
{
    VK_CHECK(vkResetFences(_device, 1, &_immFence));
    VK_CHECK(vkResetCommandBuffer(_immCommandBuffer, 0));

    VkCommandBuffer cmd = _immCommandBuffer;

    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    function(cmd);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmd);
    VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, nullptr, nullptr);

    // Submit command buffer to queue and execute it
    // _immFence will now block until the graphics commands finish executing
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, _immFence));

    VK_CHECK(vkWaitForFences(_device, 1, &_immFence, true, 9999999999));
}

void VulkanEngine::init_vulkan() {
    vkb::InstanceBuilder builder;

    // Make the vulkan instance with basic debug features
    auto inst_ret = builder.set_app_name("Example Vulkan Application")
                           .request_validation_layers(bUseValidationLayers)
                           .use_default_debug_messenger()
                           .require_api_version(1, 3, 0)
                           .build();

    vkb::Instance vkb_inst = inst_ret.value();

    // Grab the instance
    _instance = vkb_inst.instance;
    _debug_messenger = vkb_inst.debug_messenger;

    SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

    // Vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.dynamicRendering = true;
    features.synchronization2 = true;

    // Vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing  = true;

    // Use vkbootstrap to select a gpu
    // We want a gpu that can write to the SDL surface and supports vulkan 1.3 with the correct features
    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    vkb::PhysicalDevice physicalDevice = selector
                                        .set_minimum_version(1, 3)
                                        .set_required_features_13(features)
                                        .set_required_features_12(features12)
                                        .set_surface(_surface)
                                        .select()
                                        .value();

    // Create final vulkan device
    vkb::DeviceBuilder deviceBuilder{physicalDevice};

    vkb::Device vkbDevice = deviceBuilder.build().value();

    // Get the VkDevice handle used in the rest of a vulkan application
    _device = vkbDevice.device;
    _chosenGPU = physicalDevice.physical_device;

    // Use vkbootstrap to get a graphics queue
    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    // Initialize the memory allocator
    VmaAllocatorCreateInfo allocatorInfo {};
    allocatorInfo.physicalDevice = _chosenGPU;
    allocatorInfo.device = _device;
    allocatorInfo.instance = _instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &_allocator);

    _mainDeletionQueue.push_function([&]() {
        vmaDestroyAllocator(_allocator);
    });

}

void VulkanEngine::init_swapchain() {
    create_swapchain(_windowExtent.width, _windowExtent.height);

    // Draw image size will match the window
    VkExtent3D drawImageExtent = {
        _windowExtent.width,
        _windowExtent.height,
        1
    };

    // Hardcoding draw format to 32bit float
    _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _drawImage.imageExtent = drawImageExtent;

    VkImageUsageFlags drawImageUsages{};
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

    // For the draw image we want to allocate it from gpu local memory
    VmaAllocationCreateInfo rimg_allocinfo{};
    rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Allocate and create the image
    vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_drawImage.image, &_drawImage.allocation, nullptr);

    // Build an image-view for the draw image to use for rendering
    VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);

    VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_drawImage.imageView));

    // Add to deletion queues
    _mainDeletionQueue.push_function([&]() {
        vkDestroyImageView(_device, _drawImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);
    });
}
void VulkanEngine::init_commands() {
    // Create command pool for commands submitted to graphics queue
    // We also want the pool to allow for resetting individiual command buffers

    VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    for (int i = 0; i < FRAME_OVERLAP; ++i) {

        VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));

        // Allocate the default command buffer for rendering
        VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);

        VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));

        // TODO: Push to deletion queue
    }

    VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_immCommandPool));

    // Allocate the command buffer for immediate submits
    VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_immCommandPool, 1);

    VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_immCommandBuffer));

    _mainDeletionQueue.push_function([=, this]() {
       vkDestroyCommandPool(_device, _immCommandPool, nullptr);
    });
}

void VulkanEngine::init_sync_structures() {
    // Create synchronization structures
    // One fence to control when gpu finishes rendering the frame
    // Two semaphores to synchronize rendering with swapchain
    // We want the fence to start signalled so we can wait on it on the first frame
    VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

    for (int i = 0; i < FRAME_OVERLAP; ++i) {
        VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_frames[i]._renderFence));

        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._swapchainSemaphore));
    }

    for (auto& s: _swapchainSemaphores) {
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &s));
    }

    // TODO: Add to deletion queue

    VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immFence));
    _mainDeletionQueue.push_function([=, this]() {
        vkDestroyFence(_device, _immFence, nullptr);
    });
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder swapchainBuilder {_chosenGPU, _device, _surface};

    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

    vkb::Swapchain vkbSwapchain = swapchainBuilder
                                .set_desired_format(VkSurfaceFormatKHR{.format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                                // use vsync present mode
                                .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                                .set_desired_extent(width, height)
                                .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                                .build()
                                .value();

    _swapchainExtent = vkbSwapchain.extent;
    // store swapchain and its related images
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();

    _swapchainSemaphores.resize(_swapchainImages.size());

}

void VulkanEngine::destroy_swapchain() {
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);

    // destroy swapchain resources
    for (int i = 0; i < _swapchainImageViews.size(); ++i) {
        vkDestroyImageView(_device, _swapchainImageViews[i], nullptr);
    }
}

void VulkanEngine::draw_background(VkCommandBuffer cmd) {

    // Make clear color from frame number
    VkClearColorValue clearValue;
    float flash = std::abs(std::sin(_frameNumber / 120.0f));
    clearValue = {{0.0f, 0.0f, flash, 1.0f}};

    VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

    // Clear image
    //vkCmdClearColorImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);

    ComputeEffect& currentEffect = backgroundEffects[currentBackgroundEffect];
    // Bind the gradient drawing compute pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, currentEffect.pipeline);

    // Bind the descriptor set containing the draw image for the compute pipeline
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gradientPipelineLayout, 0, 1, &_drawImageDescriptors, 0, nullptr);

    vkCmdPushConstants(cmd, _gradientPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &currentEffect.data);

    // Execute the compute pipeline dispatch, we are using a 16x16 workgroup so we need to divide by it
    vkCmdDispatch(cmd, std::ceil(_drawExtent.width / 16.0f), std::ceil(_drawExtent.height / 16.0f), 1);
}

void VulkanEngine::init_descriptors() {

    // Create a descriptor pool that will hold 10 sets with 1 image each
    std::vector<DescriptorAllocator::PoolSizeRatio> sizes = {
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .ratio = 1}
    };

    globalDescriptorAllocator.init_pool(_device, 10, sizes);

    // Make descriptor set layout for our compute draw
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    // Allocate a descriptor for our draw image
    _drawImageDescriptors = globalDescriptorAllocator.allocate(_device, _drawImageDescriptorLayout);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgInfo.imageView = _drawImage.imageView;

    VkWriteDescriptorSet drawImageWrite{};
    drawImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    drawImageWrite.pNext = nullptr;

    drawImageWrite.dstBinding = 0;
    drawImageWrite.dstSet = _drawImageDescriptors;
    drawImageWrite.descriptorCount = 1;
    drawImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    drawImageWrite.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(_device, 1, &drawImageWrite, 0, nullptr);

    // Make sure both the descriptor and allocator and the new layout get cleaned up properly
    _mainDeletionQueue.push_function([&]() {
        globalDescriptorAllocator.destroy_pool(_device);

        vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);
    });


}

void VulkanEngine::init_pipelines()
{
    init_background_pipelines();
}

void VulkanEngine::init_background_pipelines()
{
    VkPipelineLayoutCreateInfo computeLayout{};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.pNext = nullptr;
    computeLayout.pSetLayouts = &_drawImageDescriptorLayout;
    computeLayout.setLayoutCount = 1;

    VkPushConstantRange pushConstant{};
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ComputePushConstants);
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    computeLayout.pPushConstantRanges = &pushConstant;
    computeLayout.pushConstantRangeCount = 1;

    VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr, &_gradientPipelineLayout));

    // Layout code
    VkShaderModule gradientShader;
    if (!vkutil::load_shader_module("shaders/gradient_color.comp.spv", _device, &gradientShader))
    {
        fmt::print("Error when building compute shader \n");
    }

    VkShaderModule skyShader;
    if (!vkutil::load_shader_module("shaders/sky.comp.spv", _device, &skyShader))
    {
        fmt::print("Error when building compute shader \n");
    }


    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.pNext = nullptr;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = gradientShader;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo computePipelineCreateInfo{};
    computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.pNext = nullptr;
    computePipelineCreateInfo.layout = _gradientPipelineLayout;
    computePipelineCreateInfo.stage = stageInfo;

    ComputeEffect gradient {};
    gradient.layout = _gradientPipelineLayout;
    gradient.name = "gradient";

    // Default colours
    gradient.data.data1 = glm::vec4{1.0f, 0.0f, 0.0f, 1.0f};
    gradient.data.data2 = glm::vec4{0.0f, 0.0f, 1.0f, 1.0f};

    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradient.pipeline));

    // Change the shader module only to create the sky shdder
    computePipelineCreateInfo.stage.module = skyShader;

    ComputeEffect sky {};
    sky.layout = _gradientPipelineLayout;
    sky.name = "sky";

    // Default sky parameters
    sky.data.data1 = glm::vec4{0.1f, 0.2f, 0.4f, 0.97f};

    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &sky.pipeline));

    backgroundEffects.push_back(gradient);
    backgroundEffects.push_back(sky);

    vkDestroyShaderModule(_device, gradientShader, nullptr);
    vkDestroyShaderModule(_device, skyShader, nullptr);

    _mainDeletionQueue.push_function([=, this](){
        vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
        vkDestroyPipeline(_device, gradient.pipeline, nullptr);
        vkDestroyPipeline(_device, sky.pipeline, nullptr);

    });
}

void VulkanEngine::init_imgui()
{
    // Create descriptor pool for imgui
    // Size of the pool is very oversized but its from imgui demeo itself
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
         { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
         { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000;
    poolInfo.poolSizeCount = (uint32_t)std::size(poolSizes);
    poolInfo.pPoolSizes = poolSizes;

    VkDescriptorPool imguiPool;
    VK_CHECK(vkCreateDescriptorPool(_device, &poolInfo, nullptr, &imguiPool));

    // Initialize imgui library

    // Init core structures of imgui
    ImGui::CreateContext();

    // Init imgui for SDL
    ImGui_ImplSDL2_InitForVulkan(_window);

    // Init imgui for Vulkan
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.Instance = _instance;
	init_info.PhysicalDevice = _chosenGPU;
	init_info.Device = _device;
	init_info.Queue = _graphicsQueue;
	init_info.DescriptorPool = imguiPool;
	init_info.MinImageCount = 3;
	init_info.ImageCount = 3;
	init_info.UseDynamicRendering = true;

	//dynamic rendering parameters for imgui to use
	init_info.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &_swapchainImageFormat;


	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init(&init_info);

	ImGui_ImplVulkan_CreateFontsTexture();

	// add to destroy the imgui created structures
	_mainDeletionQueue.push_function([=, this]() {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool(_device, imguiPool, nullptr);
	});
}

void VulkanEngine::draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(_swapchainExtent, &colorAttachment, nullptr);

    vkCmdBeginRendering(cmd, &renderInfo);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRendering(cmd);
}
