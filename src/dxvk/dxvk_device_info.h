#pragma once

#include "dxvk_include.h"

namespace dxvk {

  /**
   * \brief Device info
   * 
   * Stores core properties and a bunch of extension-specific
   * properties, if the respective extensions are available.
   * Structures for unsupported extensions will be undefined,
   * so before using them, check whether they are supported.
   */
  struct DxvkDeviceInfo {
    VkPhysicalDeviceProperties2KHR                      core;
    VkPhysicalDeviceIDProperties                        coreDeviceId;
    VkPhysicalDeviceSubgroupProperties                  coreSubgroup;
    VkPhysicalDeviceTransformFeedbackPropertiesEXT      extTransformFeedback;
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT extVertexAttributeDivisor;
    VkPhysicalDeviceDriverPropertiesKHR                 khrDeviceDriverProperties;
  };


  /**
   * \brief Device features
   * 
   * Stores core features and extension-specific features.
   * If the respective extensions are not available, the
   * extended features will be marked as unsupported.
   */
  struct DxvkDeviceFeatures {
    VkPhysicalDeviceFeatures2KHR                              core;
    VkPhysicalDeviceConditionalRenderingFeaturesEXT           extConditionalRendering;
    VkPhysicalDeviceDepthClipEnableFeaturesEXT                extDepthClipEnable;
    VkPhysicalDeviceHostQueryResetFeaturesEXT                 extHostQueryReset;
    VkPhysicalDeviceMemoryPriorityFeaturesEXT                 extMemoryPriority;
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT extShaderDemoteToHelperInvocation;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT              extTransformFeedback;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT         extVertexAttributeDivisor;
  };

}