#pragma once

#include "d3d9_texture.h"

#include "d3d9_util.h"

namespace dxvk {

  Direct3DCommonTexture9::Direct3DCommonTexture9(
            Direct3DDevice9Ex*      pDevice,
      const D3D9TextureDesc*        pDesc)
      : m_device{ pDevice }, m_desc{ *pDesc } {

    if (m_desc.Format == D3D9Format::NULL_FORMAT)
      return;

    D3D9_VK_FORMAT_INFO   formatInfo   = m_device->LookupFormat(m_desc.Format, false);

    uint32_t layers = m_desc.Type == D3DRTYPE_CUBETEXTURE ? 6 : 1;

    DxvkImageCreateInfo imageInfo;
    imageInfo.type            = GetImageTypeFromResourceType(m_desc.Type);
    imageInfo.format          = formatInfo.Format;
    imageInfo.flags           = 0;
    imageInfo.sampleCount     = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent.width    = m_desc.Width;
    imageInfo.extent.height   = m_desc.Height;
    imageInfo.extent.depth    = m_desc.Depth;
    imageInfo.numLayers       = layers;
    imageInfo.mipLevels       = std::max(1u, m_desc.MipLevels);
    imageInfo.usage           = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                              | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.stages          = VK_PIPELINE_STAGE_TRANSFER_BIT;
    imageInfo.access          = VK_ACCESS_TRANSFER_READ_BIT
                              | VK_ACCESS_TRANSFER_WRITE_BIT;
    imageInfo.tiling          = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout          = VK_IMAGE_LAYOUT_GENERAL;

    DecodeMultiSampleType(m_desc.MultiSample, &imageInfo.sampleCount);

    // The image must be marked as mutable if it can be reinterpreted
    // by a view with a different format. Depth-stencil formats cannot
    // be reinterpreted in Vulkan, so we'll ignore those.
    auto formatProperties = imageFormatInfo(formatInfo.Format);
    
    bool isTypeless = formatInfo.Aspect == 0;
    bool isMutable = formatInfo.Format != VK_FORMAT_UNDEFINED;
    bool isColorFormat = (formatProperties->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;

    if (isMutable && isColorFormat) {
      imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      imageInfo.viewFormatCount = 2;
      imageInfo.viewFormats     = reinterpret_cast<VkFormat*>(&formatInfo);
    }

    // Some games will try to create an SRGB image with the UAV
    // bind flag set. This works on Windows, but no UAVs can be
    // created for the image in practice.
    bool noUav = formatProperties->flags.test(DxvkFormatFlag::ColorSpaceSrgb)
      && !CheckFormatFeatureSupport(formatInfo.Format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    
    // Adjust image flags based on the corresponding D3D flags
    imageInfo.usage  |= VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.stages |= EnabledShaderStages();
    imageInfo.access |= VK_ACCESS_SHADER_READ_BIT;
    
    if (m_desc.Usage & D3DUSAGE_RENDERTARGET) {
      imageInfo.usage  |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      imageInfo.stages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      imageInfo.access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                       |  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    
    if (m_desc.Usage & D3DUSAGE_DEPTHSTENCIL) {
      imageInfo.usage  |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      imageInfo.stages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                       |  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      imageInfo.access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                       |  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    
    // Access pattern for meta-resolve operations
    if (imageInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT && isColorFormat) {
      imageInfo.usage  |= VK_IMAGE_USAGE_SAMPLED_BIT;
      imageInfo.stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      imageInfo.access |= VK_ACCESS_SHADER_READ_BIT;
    }
    
    if (m_desc.Type == D3DRTYPE_CUBETEXTURE)
      imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    
    // Some image formats (i.e. the R32G32B32 ones) are
    // only supported with linear tiling on most GPUs
    if (!CheckImageSupport(&imageInfo, VK_IMAGE_TILING_OPTIMAL))
      imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    
    // Determine map mode based on our findings
    m_mapMode = DetermineMapMode(&imageInfo);
    
    // If the image is mapped directly to host memory, we need
    // to enable linear tiling, and DXVK needs to be aware that
    // the image can be accessed by the host.
    if (m_mapMode == D3D9_COMMON_TEXTURE_MAP_MODE_DIRECT) {
      imageInfo.stages |= VK_PIPELINE_STAGE_HOST_BIT;
      imageInfo.tiling  = VK_IMAGE_TILING_LINEAR;
      imageInfo.access |= VK_ACCESS_HOST_WRITE_BIT;
      
      if (!(m_desc.Usage & D3DUSAGE_WRITEONLY))
        imageInfo.access |= VK_ACCESS_HOST_READ_BIT;
    }
    
    // We must keep LINEAR images in GENERAL layout, but we
    // can choose a better layout for the image based on how
    // it is going to be used by the game.
    if (imageInfo.tiling == VK_IMAGE_TILING_OPTIMAL)
      imageInfo.layout = OptimizeLayout(imageInfo.usage);

    // For some formats, we need to enable sampled and/or
    // render target capabilities if available, but these
    // should in no way affect the default image layout
    imageInfo.usage |= EnableMetaCopyUsage(imageInfo.format, imageInfo.tiling);
    imageInfo.usage |= EnableMetaPackUsage(imageInfo.format, m_desc.Usage & D3DUSAGE_WRITEONLY);
    
    // Check if we can actually create the image
    if (!CheckImageSupport(&imageInfo, imageInfo.tiling)) {
      throw DxvkError(str::format(
        "D3D9: Cannot create texture:",
        "\n  Format:  ", m_desc.Format,
        "\n  Extent:  ", m_desc.Width,
                    "x", m_desc.Height,
                    "x", m_desc.Depth,
        "\n  Samples: ", m_desc.MultiSample,
        "\n  Layers:  ", layers,
        "\n  Levels:  ", m_desc.MipLevels,
        "\n  Usage:   ", std::hex, m_desc.Usage));
    }
    
    // If necessary, create the mapped linear buffer
    if (m_mapMode == D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER)
      m_buffer = CreateMappedBuffer();
    
    // Create the image on a host-visible memory type
    // in case it is going to be mapped directly.
    VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    
    if (m_mapMode == D3D9_COMMON_TEXTURE_MAP_MODE_DIRECT) {
      memoryProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                       | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    
    m_image = m_device->GetDXVKDevice()->createImage(imageInfo, memoryProperties);
  }
  
  VkImageSubresource Direct3DCommonTexture9::GetSubresourceFromIndex(
          VkImageAspectFlags    Aspect,
          UINT                  Subresource) const {
    VkImageSubresource result;
    result.aspectMask     = Aspect;
    result.mipLevel       = Subresource % m_desc.MipLevels;
    result.arrayLayer     = Subresource / m_desc.MipLevels;
    return result;
  }
  
  bool Direct3DCommonTexture9::CheckViewCompatibility(DWORD Usage, D3D9Format Format, bool srgb) const {
    const DxvkImageCreateInfo& imageInfo = m_image->info();

    // Check whether the given bind flags are supported
    VkImageUsageFlags usage = GetImageUsageFlags(Usage);

    if ((imageInfo.usage & usage) != usage)
      return false;

    // Check whether the view format is compatible
    D3D9_VK_FORMAT_INFO viewFormat = m_device->LookupFormat(Format,        srgb);
    D3D9_VK_FORMAT_INFO baseFormat = m_device->LookupFormat(m_desc.Format, false);
    
    if (imageInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
      // Check whether the given combination of image
      // view type and view format is actually supported
      VkFormatFeatureFlags features = GetImageFormatFeatures(Usage);
      
      if (!CheckFormatFeatureSupport(viewFormat.Format, features))
        return false;

      // Using the image format itself is always legal
      if (viewFormat.Format == baseFormat.Format)
        return true;
      
      // If there is a list of compatible formats, the
      // view format must be included in that list.
      for (size_t i = 0; i < imageInfo.viewFormatCount; i++) {
        if (imageInfo.viewFormats[i] == viewFormat.Format)
          return true;
      }

      // Otherwise, all bit-compatible formats can be used.
      if (imageInfo.viewFormatCount == 0) {
        auto baseFormatInfo = imageFormatInfo(baseFormat.Format);
        auto viewFormatInfo = imageFormatInfo(viewFormat.Format);
        
        return baseFormatInfo->aspectMask  == viewFormatInfo->aspectMask
            && baseFormatInfo->elementSize == viewFormatInfo->elementSize;
      }

      return false;
    } else {
      // For non-mutable images, the view format
      // must be identical to the image format.
      return viewFormat.Format == baseFormat.Format;
    }
  }
  
  
  HRESULT Direct3DCommonTexture9::NormalizeTextureProperties(D3D9TextureDesc* pDesc) {
    if (pDesc->Width == 0 || pDesc->Height == 0 || pDesc->Depth == 0)
      return D3DERR_INVALIDCALL;
    
    if (FAILED(DecodeMultiSampleType(pDesc->MultiSample, nullptr)))
      return D3DERR_INVALIDCALL;
    
    // Use the maximum possible mip level count if the supplied
    // mip level count is either unspecified (0) or invalid
    const uint32_t maxMipLevelCount = pDesc->MultiSample <= 1
      ? util::computeMipLevelCount({ pDesc->Width, pDesc->Height, pDesc->Depth })
      : 1u;
    
    if (pDesc->MipLevels == 0 || pDesc->MipLevels > maxMipLevelCount)
      pDesc->MipLevels = maxMipLevelCount;
    
    return D3D_OK;
  }
  
  
  BOOL Direct3DCommonTexture9::CheckImageSupport(
    const DxvkImageCreateInfo*  pImageInfo,
          VkImageTiling         Tiling) const {
    const Rc<DxvkAdapter> adapter = m_device->GetDXVKDevice()->adapter();
    
    VkImageFormatProperties formatProps = { };
    
    VkResult status = adapter->imageFormatProperties(
      pImageInfo->format, pImageInfo->type, Tiling,
      pImageInfo->usage, pImageInfo->flags, formatProps);
    
    if (status != VK_SUCCESS)
      return FALSE;
    
    return (pImageInfo->extent.width  <= formatProps.maxExtent.width)
        && (pImageInfo->extent.height <= formatProps.maxExtent.height)
        && (pImageInfo->extent.depth  <= formatProps.maxExtent.depth)
        && (pImageInfo->numLayers     <= formatProps.maxArrayLayers)
        && (pImageInfo->mipLevels     <= formatProps.maxMipLevels)
        && (pImageInfo->sampleCount    & formatProps.sampleCounts);
  }


  BOOL Direct3DCommonTexture9::CheckFormatFeatureSupport(
          VkFormat              Format,
          VkFormatFeatureFlags  Features) const {
    VkFormatProperties properties = m_device->GetDXVKDevice()->adapter()->formatProperties(Format);

    return (properties.linearTilingFeatures  & Features) == Features
        || (properties.optimalTilingFeatures & Features) == Features;
  }
  
  
  VkImageUsageFlags Direct3DCommonTexture9::EnableMetaCopyUsage(
          VkFormat              Format,
          VkImageTiling         Tiling) const {
    VkFormatFeatureFlags requestedFeatures = 0;

    if (Format == VK_FORMAT_D16_UNORM || Format == VK_FORMAT_D32_SFLOAT) {
      requestedFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                        |  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }

    if (Format == VK_FORMAT_R16_UNORM || Format == VK_FORMAT_R32_SFLOAT) {
      requestedFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                        |  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }

    if (requestedFeatures == 0)
      return 0;

    // Enable usage flags for all supported and requested features
    VkFormatProperties properties = m_device->GetDXVKDevice()->adapter()->formatProperties(Format);

    requestedFeatures &= Tiling == VK_IMAGE_TILING_OPTIMAL
      ? properties.optimalTilingFeatures
      : properties.linearTilingFeatures;
    
    VkImageUsageFlags requestedUsage = 0;

    if (requestedFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
      requestedUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    
    if (requestedFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
      requestedUsage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    
    if (requestedFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
      requestedUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    return requestedUsage;
  }


  VkImageUsageFlags Direct3DCommonTexture9::EnableMetaPackUsage(
          VkFormat              Format,
          BOOL                  WriteOnly) const {
    if (WriteOnly)
      return 0;
    
    const auto dsMask = VK_IMAGE_ASPECT_DEPTH_BIT
                      | VK_IMAGE_ASPECT_STENCIL_BIT;

    auto formatInfo = imageFormatInfo(Format);

    return formatInfo->aspectMask == dsMask
      ? VK_IMAGE_USAGE_SAMPLED_BIT
      : 0;
  }

  
  D3D9_COMMON_TEXTURE_MAP_MODE Direct3DCommonTexture9::DetermineMapMode(
    const DxvkImageCreateInfo*  pImageInfo) const {    
    // Write-only images should go through a buffer for multiple reasons:
    // 1. Some games do not respect the row and depth pitch that is returned
    //    by the Map() method, which leads to incorrect rendering (e.g. Nier)
    // 2. Since the image will most likely be read for rendering by the GPU,
    //    writing the image to device-local image may be more efficient than
    //    reading its contents from host-visible memory.
    if (m_desc.Usage & D3DUSAGE_DYNAMIC)
      return D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER;
    
    // Depth-stencil formats in D3D9 can be mapped and follow special
    // packing rules, so we need to copy that data into a buffer first
    if (GetPackedDepthStencilFormat(m_desc.Format))
      return D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER;
    
    // Images that can be read by the host should be mapped directly in
    // order to avoid expensive synchronization with the GPU. This does
    // however require linear tiling, which may not be supported for all
    // combinations of image parameters.
    return this->CheckImageSupport(pImageInfo, VK_IMAGE_TILING_LINEAR)
      ? D3D9_COMMON_TEXTURE_MAP_MODE_DIRECT
      : D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER;
  }
  
  
  Rc<DxvkBuffer> Direct3DCommonTexture9::CreateMappedBuffer() const {
    const DxvkFormatInfo* formatInfo = imageFormatInfo(
      m_device->LookupFormat(m_desc.Format, false).Format);
    
    const VkExtent3D blockCount = util::computeBlockCount(
      VkExtent3D { m_desc.Width, m_desc.Height, m_desc.Depth },
      formatInfo->blockSize);
    
    DxvkBufferCreateInfo info;
    info.size   = formatInfo->elementSize
                * blockCount.width
                * blockCount.height
                * blockCount.depth;
    info.usage  = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT
                | VK_ACCESS_TRANSFER_WRITE_BIT;
    
    return m_device->GetDXVKDevice()->createBuffer(info,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  }
  
  
  VkImageType Direct3DCommonTexture9::GetImageTypeFromResourceType(D3DRESOURCETYPE Type) {
    switch (Type) {
    case D3DRTYPE_CUBETEXTURE:
    case D3DRTYPE_TEXTURE:
    case D3DRTYPE_SURFACE:
      return VK_IMAGE_TYPE_2D;

    case D3DRTYPE_VOLUME:
    case D3DRTYPE_VOLUMETEXTURE:
      return VK_IMAGE_TYPE_3D;

    default: throw DxvkError("Direct3DCommonTexture9: Unhandled resource type");
    }
  }
  
  
  VkImageLayout Direct3DCommonTexture9::OptimizeLayout(VkImageUsageFlags Usage) {
    const VkImageUsageFlags usageFlags = Usage;
    
    // Filter out unnecessary flags. Transfer operations
    // are handled by the backend in a transparent manner.
    Usage &= ~(VK_IMAGE_USAGE_TRANSFER_DST_BIT
             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    
    // If the image is used only as an attachment, we never
    // have to transform the image back to a different layout
    if (Usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    if (Usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
      return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    
    Usage &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
             | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    
    // If the image is used for reading but not as a storage
    // image, we can optimize the image for texture access
    if (Usage == VK_IMAGE_USAGE_SAMPLED_BIT) {
      return usageFlags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    
    // Otherwise, we have to stick with the default layout
    return VK_IMAGE_LAYOUT_GENERAL;
  }

  HRESULT Direct3DCommonTexture9::Lock(
        UINT            Subresource,
        D3DLOCKED_BOX*  pLockedBox,
  const D3DBOX*         pBox,
        DWORD           Flags) {
    return m_device->LockImage(
      this,
      Subresource,
      pLockedBox,
      pBox,
      Flags);
  }

  HRESULT Direct3DCommonTexture9::Unlock(UINT     Subresource) {
    return m_device->UnlockImage(
      this,
      Subresource);
  }

  // Direct3DTexture9

  D3DRESOURCETYPE STDMETHODCALLTYPE Direct3DTexture9::GetType() {
    return D3DRTYPE_TEXTURE;
  }

  HRESULT STDMETHODCALLTYPE Direct3DTexture9::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
    auto* surface = GetSubresource(Level);
    if (surface == nullptr)
      return D3DERR_INVALIDCALL;

    return surface->GetDesc(pDesc);
  }

  HRESULT STDMETHODCALLTYPE Direct3DTexture9::GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel) {
    InitReturnPtr(ppSurfaceLevel);
    auto* surface = GetSubresource(Level);

    if (ppSurfaceLevel == nullptr || surface == nullptr)
      return D3DERR_INVALIDCALL;

    *ppSurfaceLevel = ref(surface);
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DTexture9::LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) {
    auto* surface = GetSubresource(Level);
    if (surface == nullptr || pLockedRect == nullptr)
      return D3DERR_INVALIDCALL;

    D3DBOX box;
    if (pRect != nullptr) {
      box.Left = pRect->left;
      box.Right = pRect->right;
      box.Top = pRect->top;
      box.Bottom = pRect->bottom;
      box.Front = 0;
      box.Back = 1;
    }

    D3DLOCKED_BOX lockedBox;

    HRESULT hr = m_texture->Lock(Level, &lockedBox, pRect != nullptr ? &box : nullptr, Flags);

    pLockedRect->pBits = lockedBox.pBits;
    pLockedRect->Pitch = lockedBox.RowPitch;

    return hr;
  }

  HRESULT STDMETHODCALLTYPE Direct3DTexture9::UnlockRect(UINT Level) {
    auto* surface = GetSubresource(Level);
    if (surface == nullptr)
      return D3DERR_INVALIDCALL;

    return m_texture->Unlock(Level);
  }

  HRESULT STDMETHODCALLTYPE Direct3DTexture9::AddDirtyRect(CONST RECT* pDirtyRect) {
    Logger::warn("Direct3DTexture9::AddDirtyRect: Stub");
    return D3D_OK;
  }
}