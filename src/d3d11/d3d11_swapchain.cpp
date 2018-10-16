#include "d3d11_device.h"
#include "d3d11_swapchain.h"

namespace dxvk {

  D3D11SwapChain::D3D11SwapChain(
          D3D11Device*            pDevice,
          HWND                    hWnd,
    const DXGI_SWAP_CHAIN_DESC1*  pDesc)
  : m_parent(pDevice),
    m_window(hWnd),
    m_desc  (*pDesc) {
    
    // TODO deferred surface creation
    CreateSurface();
    CreateBackBuffer();
    
    InitRenderState();
  }


  D3D11SwapChain::~D3D11SwapChain() {
    m_device->waitForIdle();
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    InitReturnPtr(ppvObject);

    // TODO COM interface
    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIVkSwapChain)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D11SwapChain::QueryInterface: Unknown interface query");
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDesc(
          DXGI_SWAP_CHAIN_DESC1*    pDesc) {
    *pDesc = m_desc;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetImage(
          UINT                      BufferId,
          REFIID                    riid,
          void**                    ppBuffer) {
    InitReturnPtr(ppBuffer);

    if (BufferId > 0) {
      Logger::err("D3D11: GetImage: BufferId > 0 not supported");
      return DXGI_ERROR_UNSUPPORTED;
    }

    return m_backBuffer->QueryInterface(riid, ppBuffer);
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetImageIndex() {
    return 0;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::ChangeProperties(
    const DXGI_SWAP_CHAIN_DESC1*  pDesc) {
    m_desc = *pDesc;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetPresentRegion(
    const RECT*                     pRegion) {
    // TODO implement
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetGammaControl(
          UINT                      NumControlPoints,
    const DXGI_RGB*                 pControlPoints) {
    // TODO implement
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::Present(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
    bool vsync = SyncInterval != 0;

    m_dirty |= vsync != m_vsync;
    m_vsync  = vsync;

    if (std::exchange(m_dirty, false))
      CreateSwapChain();
    
    PresentImage(SyncInterval);
    return S_OK;
  }


  void D3D11SwapChain::PresentImage(UINT SyncInterval) {
    // TODO sync event
    // TODO hud
    for (uint32_t i = 0; i < SyncInterval || i < 1; i++) {
      m_context->beginRecording(
        m_device->createCommandList());
      
      // Resolve back buffer if it is multisampled. We
      // only have to do it only for the first frame.
      if (m_swapImageResolve != nullptr && i == 0) {
        VkImageSubresourceLayers resolveSubresources;
        resolveSubresources.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveSubresources.mipLevel        = 0;
        resolveSubresources.baseArrayLayer  = 0;
        resolveSubresources.layerCount      = 1;
        
        m_context->resolveImage(
          m_swapImageResolve, resolveSubresources,
          m_swapImage,        resolveSubresources,
          VK_FORMAT_UNDEFINED);
      }
      
      // Presentation semaphores and WSI swap chain image
      auto wsiSemas = m_swapchain->getSemaphorePair();
      auto wsiImage = m_swapchain->getImageView(wsiSemas.acquireSync);

      // Use an appropriate texture filter depending on whether
      // the back buffer size matches the swap image size
      bool fitSize = m_swapImage->info().extent == wsiImage->imageInfo().extent;

      m_context->bindShader(VK_SHADER_STAGE_VERTEX_BIT,   m_vertShader);
      m_context->bindShader(VK_SHADER_STAGE_FRAGMENT_BIT, m_fragShader);

      DxvkRenderTargets renderTargets;
      renderTargets.color[0].view   = wsiImage;
      renderTargets.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      m_context->bindRenderTargets(renderTargets, false);

      VkViewport viewport;
      viewport.x        = 0.0f;
      viewport.y        = 0.0f;
      viewport.width    = float(wsiImage->imageInfo().extent.width);
      viewport.height   = float(wsiImage->imageInfo().extent.height);
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;
      
      VkRect2D scissor;
      scissor.offset.x      = 0;
      scissor.offset.y      = 0;
      scissor.extent.width  = wsiImage->imageInfo().extent.width;
      scissor.extent.height = wsiImage->imageInfo().extent.height;

      m_context->setViewports(1, &viewport, &scissor);

      m_context->setRasterizerState(m_rsState);
      m_context->setMultisampleState(m_msState);
      m_context->setDepthStencilState(m_dsState);
      m_context->setLogicOpState(m_loState);
      m_context->setBlendMode(0, m_blendMode);
      
      m_context->setInputAssemblyState(m_iaState);
      m_context->setInputLayout(0, nullptr, 0, nullptr);

      m_context->bindResourceSampler(BindingIds::Sampler, fitSize ? m_samplerFitting : m_samplerScaling);
      m_context->bindResourceSampler(BindingIds::GammaSmp, m_gammaSampler);

      m_context->bindResourceView(BindingIds::Texture, m_swapImageView, nullptr);
      m_context->bindResourceView(BindingIds::GammaTex, m_gammaTextureView, nullptr);

      m_context->draw(4, 1, 0, 0);

      m_device->submitCommandList(
        m_context->endRecording(),
        wsiSemas.acquireSync,
        wsiSemas.presentSync);
      
      m_swapchain->present(
        wsiSemas.presentSync);
    }
  }


  void D3D11SwapChain::CreateBackBuffer() {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    m_swapImage         = nullptr;
    m_swapImageResolve  = nullptr;
    m_swapImageView     = nullptr;
    m_backBuffer        = nullptr;

    // Create new back buffer
    D3D11_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_desc.Width,  1u);
    desc.Height             = std::max(m_desc.Height, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = m_desc.Format;
    desc.SampleDesc         = m_desc.SampleDesc;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = D3D11_BIND_RENDER_TARGET
                            | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = 0;

    if (m_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    
    m_backBuffer = new D3D11Texture2D(m_parent, &desc);
    m_swapImage = GetCommonTexture(m_backBuffer.ptr())->GetImage();

    // If the image is multisampled, we need to create
    // another image which we'll use as a resolve target
    if (m_swapImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      DxvkImageCreateInfo resolveInfo;
      resolveInfo.type          = VK_IMAGE_TYPE_2D;
      resolveInfo.format        = m_swapImage->info().format;
      resolveInfo.flags         = 0;
      resolveInfo.sampleCount   = VK_SAMPLE_COUNT_1_BIT;
      resolveInfo.extent        = m_swapImage->info().extent;
      resolveInfo.numLayers     = 1;
      resolveInfo.mipLevels     = 1;
      resolveInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                                | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      resolveInfo.stages        = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                | VK_PIPELINE_STAGE_TRANSFER_BIT;
      resolveInfo.access        = VK_ACCESS_SHADER_READ_BIT
                                | VK_ACCESS_TRANSFER_WRITE_BIT
                                | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      resolveInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
      resolveInfo.layout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      
      m_swapImageResolve = m_device->createImage(
        resolveInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }

    // Create an image view that allows the
    // image to be bound as a shader resource.
    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type       = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format     = m_swapImage->info().format;
    viewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel   = 0;
    viewInfo.numLevels  = 1;
    viewInfo.minLayer   = 0;
    viewInfo.numLayers  = 1;
    
    m_swapImageView = m_device->createImageView(
      m_swapImageResolve != nullptr
        ? m_swapImageResolve
        : m_swapImage,
      viewInfo);
    
    // Initialize the image so that we can use it. Clearing
    // to black prevents garbled output for the first frame.
    VkImageSubresourceRange subresources;
    subresources.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.baseMipLevel   = 0;
    subresources.levelCount     = 1;
    subresources.baseArrayLayer = 0;
    subresources.layerCount     = 1;

    VkClearColorValue clearColor;
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;

    m_context->beginRecording(
      m_device->createCommandList());
    
    m_context->clearColorImage(
      m_swapImage, clearColor, subresources);

    m_device->submitCommandList(
      m_context->endRecording(),
      nullptr, nullptr);
  }


  void D3D11SwapChain::CreateSurface() {
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
      GetWindowLongPtr(m_window, GWLP_HINSTANCE));
    
    m_surface = m_device->adapter()->createSurface(instance, m_window);
  }


  void D3D11SwapChain::CreateSwapChain() {
    DxvkSwapchainProperties options;
    // options.preferredSurfaceFormat      = PickSurfaceFormat();
    // options.preferredPresentMode        = PickPresentMode();
    options.preferredBufferSize.width   = m_desc.Width;
    options.preferredBufferSize.height  = m_desc.Height;
    options.preferredBufferCount        = m_desc.BufferCount;

    if (m_surface == nullptr)
      CreateSurface();
    
    if (m_swapchain == nullptr)
      m_swapchain = m_device->createSwapchain(m_surface, options);
    else
      m_swapchain->changeProperties(options);
  }


  void D3D11SwapChain::InitRenderState() {
    m_iaState.primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    m_iaState.primitiveRestart  = VK_FALSE;
    m_iaState.patchVertexCount  = 0;
    
    m_rsState.polygonMode        = VK_POLYGON_MODE_FILL;
    m_rsState.cullMode           = VK_CULL_MODE_BACK_BIT;
    m_rsState.frontFace          = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    m_rsState.depthClampEnable   = VK_FALSE;
    m_rsState.depthBiasEnable    = VK_FALSE;
    m_rsState.depthBiasConstant  = 0.0f;
    m_rsState.depthBiasClamp     = 0.0f;
    m_rsState.depthBiasSlope     = 0.0f;
    
    m_msState.sampleMask            = 0xffffffff;
    m_msState.enableAlphaToCoverage = VK_FALSE;
    m_msState.enableAlphaToOne      = VK_FALSE;
    
    VkStencilOpState stencilOp;
    stencilOp.failOp      = VK_STENCIL_OP_KEEP;
    stencilOp.passOp      = VK_STENCIL_OP_KEEP;
    stencilOp.depthFailOp = VK_STENCIL_OP_KEEP;
    stencilOp.compareOp   = VK_COMPARE_OP_ALWAYS;
    stencilOp.compareMask = 0xFFFFFFFF;
    stencilOp.writeMask   = 0xFFFFFFFF;
    stencilOp.reference   = 0;
    
    m_dsState.enableDepthTest   = VK_FALSE;
    m_dsState.enableDepthWrite  = VK_FALSE;
    m_dsState.enableStencilTest = VK_FALSE;
    m_dsState.depthCompareOp    = VK_COMPARE_OP_ALWAYS;
    m_dsState.stencilOpFront    = stencilOp;
    m_dsState.stencilOpBack     = stencilOp;
    
    m_loState.enableLogicOp = VK_FALSE;
    m_loState.logicOp       = VK_LOGIC_OP_NO_OP;
    
    m_blendMode.enableBlending  = VK_FALSE;
    m_blendMode.colorSrcFactor  = VK_BLEND_FACTOR_ONE;
    m_blendMode.colorDstFactor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    m_blendMode.colorBlendOp    = VK_BLEND_OP_ADD;
    m_blendMode.alphaSrcFactor  = VK_BLEND_FACTOR_ONE;
    m_blendMode.alphaDstFactor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    m_blendMode.alphaBlendOp    = VK_BLEND_OP_ADD;
    m_blendMode.writeMask       = VK_COLOR_COMPONENT_R_BIT
                                | VK_COLOR_COMPONENT_G_BIT
                                | VK_COLOR_COMPONENT_B_BIT
                                | VK_COLOR_COMPONENT_A_BIT;
  }
  
}