// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>
#include "common/logging/log.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"

namespace Vulkan {
namespace {
VkBool32 DebugUtilCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                           VkDebugUtilsMessageTypeFlagsEXT type,
                           const VkDebugUtilsMessengerCallbackDataEXT* data,
                           [[maybe_unused]] void* user_data) {
    // Skip logging known false-positive validation errors
    switch (static_cast<u32>(data->messageIdNumber)) {
#ifdef ANDROID
    case 0xbf9cf353u: // VUID-vkCmdBindVertexBuffers2-pBuffers-04111
    // The below are due to incorrect reporting of extendedDynamicState
    case 0x1093bebbu: // VUID-vkCmdSetCullMode-None-03384
    case 0x9215850fu: // VUID-vkCmdSetDepthTestEnable-None-03352
    case 0x86bf18dcu: // VUID-vkCmdSetDepthWriteEnable-None-03354
    case 0x0792ad08u: // VUID-vkCmdSetStencilOp-None-03351
    case 0x93e1ba4eu: // VUID-vkCmdSetFrontFace-None-03383
    case 0xac9c13c5u: // VUID-vkCmdSetStencilTestEnable-None-03350
    case 0xc9a2001bu: // VUID-vkCmdSetDepthBoundsTestEnable-None-03349
    case 0x8b7159a7u: // VUID-vkCmdSetDepthCompareOp-None-03353
    // The below are due to incorrect reporting of extendedDynamicState2
    case 0xb13c8036u: // VUID-vkCmdSetDepthBiasEnable-None-04872
    case 0xdff2e5c1u: // VUID-vkCmdSetRasterizerDiscardEnable-None-04871
    case 0x0cc85f41u: // VUID-vkCmdSetPrimitiveRestartEnable-None-04866
    case 0x01257b492: // VUID-vkCmdSetLogicOpEXT-None-0486
    // The below are due to incorrect reporting of vertexInputDynamicState
    case 0x398e0dabu: // VUID-vkCmdSetVertexInputEXT-None-04790
    // The below are due to incorrect reporting of extendedDynamicState3
    case 0x970c11a5u: // VUID-vkCmdSetColorWriteMaskEXT-extendedDynamicState3ColorWriteMask-07364
    case 0x6b453f78u: // VUID-vkCmdSetColorBlendEnableEXT-extendedDynamicState3ColorBlendEnable-07355
    case 0xf66469d0u: // VUID-vkCmdSetColorBlendEquationEXT-extendedDynamicState3ColorBlendEquation-07356
    case 0x1d43405eu: // VUID-vkCmdSetLogicOpEnableEXT-extendedDynamicState3LogicOpEnable-07365
    case 0x638462e8u: // VUID-vkCmdSetDepthClampEnableEXT-extendedDynamicState3DepthClampEnable-07448
    // Misc
    case 0xe0a2da61u: // VUID-vkCmdDrawIndexed-format-07753
#else
    case 0x682a878au: // VUID-vkCmdBindVertexBuffers2EXT-pBuffers-parameter
    case 0x99fb7dfdu: // UNASSIGNED-RequiredParameter (vkCmdBindVertexBuffers2EXT pBuffers[0])
    case 0xe8616bf2u: // Bound VkDescriptorSet 0x0[] was destroyed. Likely push_descriptor related
    case 0x1608dec0u: // Image layout in vkUpdateDescriptorSet doesn't match descriptor use
    case 0x55362756u: // Descriptor binding and framebuffer attachment overlap
#endif
        return VK_FALSE;
    default:
        break;
    }

    const std::string_view message{data->pMessage};
    // Filter out sampler custom border color errors that are false positives
    if (message.find("borderColor is VK_BORDER_COLOR_FLOAT_CUSTOM_EXT but there is no VkSamplerCustomBorderColorCreateInfoEXT") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out sampler custom border color format validation errors
    if (message.find("has a custom border color with format = VK_FORMAT_UNDEFINED and is used to sample an image view") != std::string_view::npos &&
        (message.find("VK_FORMAT_B4G4R4A4_UNORM_PACK16") != std::string_view::npos ||
         message.find("VK_FORMAT_B5G6R5_UNORM_PACK16") != std::string_view::npos ||
         message.find("VK_FORMAT_A1B5G5R5_UNORM_PACK16") != std::string_view::npos ||
         message.find("VK_FORMAT_B5G5R5A1_UNORM_PACK16") != std::string_view::npos)) {
        return VK_FALSE;
    }

    // Filter out R32_FLOAT format related validation errors that are false positives
    if (message.find("requires FLOAT component type") != std::string_view::npos &&
        (message.find("VK_FORMAT_R32_UINT") != std::string_view::npos ||
         message.find("VK_FORMAT_R32G32_UINT") != std::string_view::npos ||
         message.find("VK_FORMAT_R32G32B32A32_UINT") != std::string_view::npos ||
         message.find("VK_FORMAT_R8_UINT") != std::string_view::npos ||
         message.find("VK_FORMAT_A8B8G8R8_UINT_PACK32") != std::string_view::npos)) {
        return VK_FALSE;
    }

    // Filter out non-identity swizzle validation errors for storage images that are false positives
    if (message.find("has a non-identiy swizzle component") != std::string_view::npos &&
        message.find("VK_DESCRIPTOR_TYPE_STORAGE_IMAGE") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out 3D image layerCount validation warnings that are false positives
    if (message.find("layerCount is 1 for a 3D image") != std::string_view::npos &&
        message.find("VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out descriptor pool type mismatch warnings that are false positives
    if (message.find("binding") != std::string_view::npos &&
        message.find("was created with VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE") != std::string_view::npos &&
        message.find("was not created with any VkDescriptorPoolSize::type with VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out vertex buffer stride validation errors that are false positives
    if (message.find("The pStrides value") != std::string_view::npos &&
        message.find("is not 0 and less than the extent of the binding for the attribute") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out color blend feature validation errors for integer formats that don't support blending
    if (message.find("does not have VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT") != std::string_view::npos &&
        message.find("vkCmdSetColorBlendEnableEXT") != std::string_view::npos &&
        message.find("was set to VK_TRUE") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out query related validation errors that are false positives
    if (message.find("query (") != std::string_view::npos && message.find("was started outside a renderpass") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out query subpass validation errors that are false positives
    if (message.find("query ") != std::string_view::npos && message.find("from VkQueryPool") != std::string_view::npos &&
        message.find("was began in subpass") != std::string_view::npos && message.find("but never ended") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out conditional rendering stage mask errors when feature is not enabled
    if (message.find("dstStageMask includes VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT") != std::string_view::npos &&
        message.find("conditionalRendering feature is not enabled") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out conditional rendering access mask validation errors
    if (message.find("dstAccessMask") != std::string_view::npos &&
        message.find("VK_ACCESS_INDIRECT_COMMAND_READ_BIT") != std::string_view::npos &&
        message.find("VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT") != std::string_view::npos) {
        return VK_FALSE;
    }

    // Filter out sampler addressMode validation errors caused by GL_CLAMP emulation hack
    if (message.find("addressModeU (51966) does not fall within the begin..end range") != std::string_view::npos ||
        message.find("addressModeV (51966) does not fall within the begin..end range") != std::string_view::npos ||
        message.find("addressModeW (51966) does not fall within the begin..end range") != std::string_view::npos) {
        return VK_FALSE;
    }

    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOG_CRITICAL(Render_Vulkan, "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_WARNING(Render_Vulkan, "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        LOG_INFO(Render_Vulkan, "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        LOG_DEBUG(Render_Vulkan, "{}", message);
    }
    return VK_FALSE;
}

} // Anonymous namespace

vk::DebugUtilsMessenger CreateDebugUtilsCallback(const vk::Instance& instance) {
    return instance.CreateDebugUtilsMessenger(VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = DebugUtilCallback,
        .pUserData = nullptr,
    });
}

} // namespace Vulkan
