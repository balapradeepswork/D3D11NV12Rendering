// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include "OutputManager.h"
#include <array>
using namespace DirectX;

// Below are lists of errors expect from Dxgi API calls when a transition event like mode change, PnpStop, PnpStart
// desktop switch, TDR or session disconnect/reconnect. In all these cases we want the application to clean up the threads that process
// the desktop updates and attempt to recreate them.
// If we get an error that is not on the appropriate list then we exit the application

// These are the errors we expect from general Dxgi API due to a transition
HRESULT SystemTransitionsExpectedErrors[] = {
	DXGI_ERROR_DEVICE_REMOVED,
	DXGI_ERROR_ACCESS_LOST,
	static_cast<HRESULT>(WAIT_ABANDONED),
	S_OK                                    // Terminate list with zero valued HRESULT
};

// These are the errors we expect from IDXGIOutput1::DuplicateOutput due to a transition
HRESULT CreateDuplicationExpectedErrors[] = {
	DXGI_ERROR_DEVICE_REMOVED,
	static_cast<HRESULT>(E_ACCESSDENIED),
	DXGI_ERROR_UNSUPPORTED,
	DXGI_ERROR_SESSION_DISCONNECTED,
	S_OK                                    // Terminate list with zero valued HRESULT
};

// These are the errors we expect from IDXGIOutputDuplication methods due to a transition
HRESULT FrameInfoExpectedErrors[] = {
	DXGI_ERROR_DEVICE_REMOVED,
	DXGI_ERROR_ACCESS_LOST,
	S_OK                                    // Terminate list with zero valued HRESULT
};

// These are the errors we expect from IDXGIAdapter::EnumOutputs methods due to outputs becoming stale during a transition
HRESULT EnumOutputsExpectedErrors[] = {
	DXGI_ERROR_NOT_FOUND,
	S_OK                                    // Terminate list with zero valued HRESULT
};


//
// Constructor NULLs out all pointers & sets appropriate var vals
//
OUTPUTMANAGER::OUTPUTMANAGER() : m_SwapChain(nullptr),
                                 m_Device(nullptr),
                                 m_Factory(nullptr),
                                 m_DeviceContext(nullptr),
                                 m_RTV(nullptr),
                                 m_SamplerLinear(nullptr),
                                 m_BlendState(nullptr),
                                 m_VertexShader(nullptr),
                                 m_PixelShader(nullptr),
                                 m_InputLayout(nullptr),
                                 m_SharedSurf(nullptr),
                                 m_WindowHandle(nullptr),
                                 m_NeedsResize(false),
                                 m_OcclusionCookie(0),
								 m_width(0),
								 m_height(0)
{
}

OUTPUTMANAGER::OUTPUTMANAGER(int width, int height)
{
	m_width = width;
	m_height = height;
}

//
// Destructor which calls CleanRefs to release all references and memory.
//
OUTPUTMANAGER::~OUTPUTMANAGER()
{
    CleanRefs();
}

//
// Indicates that window has been resized.
//
void OUTPUTMANAGER::WindowResize()
{
    m_NeedsResize = true;
}

//
// Initialize all state
//
DUPL_RETURN OUTPUTMANAGER::InitOutput(HWND Window, _Out_ RECT* DeskBounds)
{
    HRESULT hr;

    // Store window handle
    m_WindowHandle = Window;

    // Driver types supported
    D3D_DRIVER_TYPE DriverTypes[] =
    {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };
    UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

    // Feature levels supported
    D3D_FEATURE_LEVEL FeatureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1
    };
    UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);
    D3D_FEATURE_LEVEL FeatureLevel;
	// This flag adds support for surfaces with a different color channel ordering
	// than the default. It is required for compatibility with Direct2D.
	UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    // Create device
    for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex)
    {
        hr = D3D11CreateDevice(nullptr, DriverTypes[DriverTypeIndex], nullptr, creationFlags, FeatureLevels, NumFeatureLevels,
        D3D11_SDK_VERSION, &m_Device, &FeatureLevel, &m_DeviceContext);
        if (SUCCEEDED(hr))
        {
            // Device creation succeeded, no need to loop anymore
            break;
        }
    }
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Device creation in OUTPUTMANAGER failed", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Get DXGI factory
    IDXGIDevice* DxgiDevice = nullptr;
    hr = m_Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&DxgiDevice));
    if (FAILED(hr))
    {
        return ProcessFailure(nullptr, L"Failed to QI for DXGI Device", L"Error", hr, nullptr);
    }

    IDXGIAdapter* DxgiAdapter = nullptr;
    hr = DxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&DxgiAdapter));
    DxgiDevice->Release();
    DxgiDevice = nullptr;
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to get parent DXGI Adapter", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    hr = DxgiAdapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&m_Factory));
    DxgiAdapter->Release();
    DxgiAdapter = nullptr;
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to get parent DXGI Factory", L"Error", hr, SystemTransitionsExpectedErrors);
    }

	
    // Register for occlusion status windows message
    hr = m_Factory->RegisterOcclusionStatusWindow(Window, OCCLUSION_STATUS_MSG, &m_OcclusionCookie);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to register for occlusion message", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Get window size
    RECT WindowRect;
    GetClientRect(m_WindowHandle, &WindowRect);
    UINT Width = WindowRect.right - WindowRect.left;
    UINT Height = WindowRect.bottom - WindowRect.top;

	

    // Create swapchain for window
    DXGI_SWAP_CHAIN_DESC1 SwapChainDesc;
    RtlZeroMemory(&SwapChainDesc, sizeof(SwapChainDesc));

    SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    SwapChainDesc.BufferCount = 2;
    SwapChainDesc.Width = Width;
    SwapChainDesc.Height = Height;
    SwapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    SwapChainDesc.SampleDesc.Count = 1;
    SwapChainDesc.SampleDesc.Quality = 0;
    hr = m_Factory->CreateSwapChainForHwnd(m_Device, Window, &SwapChainDesc, nullptr, nullptr, &m_SwapChain);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create window swapchain", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Disable the ALT-ENTER shortcut for entering full-screen mode
    hr = m_Factory->MakeWindowAssociation(Window, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to make window association", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Create shared texture
    DUPL_RETURN Return = CreateSharedSurf(DeskBounds);
    if (Return != DUPL_RETURN_SUCCESS)
    {
        return Return;
    }

    // Make new render target view
    Return = MakeRTV();
    if (Return != DUPL_RETURN_SUCCESS)
    {
        return Return;
    }

    // Set view port
    SetViewPort(Width, Height);

	D3D11_SAMPLER_DESC desc = CD3D11_SAMPLER_DESC(CD3D11_DEFAULT());

	
	hr = m_Device->CreateSamplerState(
			&desc,
			&m_SamplerLinear
	);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create sampler state in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
	}
    

    // Initialize shaders
    Return = InitShaders();
    if (Return != DUPL_RETURN_SUCCESS)
    {
        return Return;
    }

    GetWindowRect(m_WindowHandle, &WindowRect);
    MoveWindow(m_WindowHandle, WindowRect.left, WindowRect.top, (DeskBounds->right - DeskBounds->left) / 2, (DeskBounds->bottom - DeskBounds->top) / 2, TRUE);

    return Return;
}

DUPL_RETURN OUTPUTMANAGER::CreateAccessibleSurf(_In_ RECT* DeskBounds, _In_ DXGI_FORMAT Format)
{
	D3D11_TEXTURE2D_DESC desc;
	
	desc.Width = DeskBounds->right - DeskBounds->left;
	desc.Height = DeskBounds->bottom - DeskBounds->top;
	desc.Format = Format;
	desc.ArraySize = 1;
	desc.BindFlags = 0;
	desc.MiscFlags = 0;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.MipLevels = 1;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
	desc.Usage = D3D11_USAGE_STAGING;

	HRESULT hr = OUTPUTMANAGER::m_Device->CreateTexture2D(&desc, NULL, &m_AccessibleSurf);

	if (FAILED(hr))
	{
		MessageBoxW(nullptr, L"Creating cpu accessable texture failed.", L"Error", MB_OK);
		return DUPL_RETURN_ERROR_UNEXPECTED;
	}

	if (m_AccessibleSurf == nullptr)
	{
		MessageBoxW(nullptr, L"Creating cpu accessable texture failed.", L"Error", MB_OK);
		return DUPL_RETURN_ERROR_UNEXPECTED;
	}
	return DUPL_RETURN_SUCCESS;
}

//
// Recreate shared texture
//
DUPL_RETURN OUTPUTMANAGER::CreateSharedSurf(_Out_ RECT* DeskBounds)
{
    HRESULT hr;

	// Get DXGI resources
	IDXGIDevice* DxgiDevice = nullptr;
	hr = m_Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&DxgiDevice));
	if (FAILED(hr))
	{
		return ProcessFailure(nullptr, L"Failed to QI for DXGI Device", L"Error", hr);
	}

	IDXGIAdapter* DxgiAdapter = nullptr;
	hr = DxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&DxgiAdapter));
	DxgiDevice->Release();
	DxgiDevice = nullptr;
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to get parent DXGI Adapter", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// Set initial values so that we always catch the right coordinates
	DeskBounds->left = INT_MAX;
	DeskBounds->right = INT_MIN;
	DeskBounds->top = INT_MAX;
	DeskBounds->bottom = INT_MIN;

	IDXGIOutput* DxgiOutput = nullptr;

	// Figure out right dimensions for full size desktop texture and # of outputs to duplicate
	UINT OutputCount;

	hr = S_OK;
	for (OutputCount = 0; SUCCEEDED(hr); ++OutputCount)
	{
		if (DxgiOutput)
		{
			DxgiOutput->Release();
			DxgiOutput = nullptr;
		}
		hr = DxgiAdapter->EnumOutputs(OutputCount, &DxgiOutput);
		if (DxgiOutput && (hr != DXGI_ERROR_NOT_FOUND))
		{
			DXGI_OUTPUT_DESC DesktopDesc;
			DxgiOutput->GetDesc(&DesktopDesc);

			DeskBounds->left = min(DesktopDesc.DesktopCoordinates.left, DeskBounds->left);
			DeskBounds->top = min(DesktopDesc.DesktopCoordinates.top, DeskBounds->top);
			DeskBounds->right = max(DesktopDesc.DesktopCoordinates.right, DeskBounds->right);
			DeskBounds->bottom = max(DesktopDesc.DesktopCoordinates.bottom, DeskBounds->bottom);
		}
	}

	--OutputCount;

	m_width = DeskBounds->right - DeskBounds->left;
	m_height = DeskBounds->bottom - DeskBounds->top;

	DxgiAdapter->Release();
	DxgiAdapter = nullptr;

	D3D11_TEXTURE2D_DESC const texDesc = CD3D11_TEXTURE2D_DESC(
		DXGI_FORMAT_NV12,           // HoloLens PV camera format, common for video sources
		m_width,					// Width of the video frames
		m_height,					// Height of the video frames
		1,                          // Number of textures in the array
		1,                          // Number of miplevels in each texture
		D3D11_BIND_SHADER_RESOURCE, // We read from this texture in the shader
		D3D11_USAGE_DYNAMIC,        // Because we'll be copying from CPU memory
		D3D11_CPU_ACCESS_WRITE      // We only need to write into the texture
	);


	hr = m_Device->CreateTexture2D(&texDesc, nullptr, &m_texture);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create texture", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// https://msdn.microsoft.com/en-us/library/windows/desktop/bb173059(v=vs.85).aspx
	// To access DXGI_FORMAT_NV12 in the shader, we need to map the luminance channel and the chrominance channels
	// into a format that shaders can understand.
	// In the case of NV12, DirectX understands how the texture is laid out, so we can create these
	// shader resource views which represent the two channels of the NV12 texture.
	// Then inside the shader we convert YUV into RGB so we can render.

	// DirectX specifies the view format to be DXGI_FORMAT_R8_UNORM for NV12 luminance channel.
	// Luminance is 8 bits per pixel. DirectX will handle converting 8-bit integers into normalized
	// floats for use in the shader.
	D3D11_SHADER_RESOURCE_VIEW_DESC const luminancePlaneDesc = CD3D11_SHADER_RESOURCE_VIEW_DESC(
		m_texture,
		D3D11_SRV_DIMENSION_TEXTURE2D,
		DXGI_FORMAT_R8_UNORM
	);

	hr = m_Device->CreateShaderResourceView(
		m_texture,
		&luminancePlaneDesc,
		&m_luminanceView
	);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create texture", L"Error", hr, SystemTransitionsExpectedErrors);
	}

	// DirectX specifies the view format to be DXGI_FORMAT_R8G8_UNORM for NV12 chrominance channel.
	// Chrominance has 4 bits for U and 4 bits for V per pixel. DirectX will handle converting 4-bit
	// integers into normalized floats for use in the shader.
	D3D11_SHADER_RESOURCE_VIEW_DESC const chrominancePlaneDesc = CD3D11_SHADER_RESOURCE_VIEW_DESC(
		m_texture,
		D3D11_SRV_DIMENSION_TEXTURE2D,
		DXGI_FORMAT_R8G8_UNORM
	);

	hr = m_Device->CreateShaderResourceView(
		m_texture,
		&chrominancePlaneDesc,
		&m_chrominanceView
	);
	if (FAILED(hr))
	{
		return ProcessFailure(m_Device, L"Failed to create shader resource view", L"Error", hr, SystemTransitionsExpectedErrors);
	}

    return DUPL_RETURN_SUCCESS;
}

//
// Present to the application window
//
DUPL_RETURN OUTPUTMANAGER::UpdateApplicationWindow(_Inout_ bool* Occluded)
{
    // In a typical desktop duplication application there would be an application running on one system collecting the desktop images
    // and another application running on a different system that receives the desktop images via a network and display the image. This
    // sample contains both these aspects into a single application.
    // This routine is the part of the sample that displays the desktop image onto the display

	HRESULT hr;
    //draw
    DUPL_RETURN Ret = DrawFrame();
    
    // Present to window if all worked
    if (Ret == DUPL_RETURN_SUCCESS)
    {
        // Present to window
        hr = m_SwapChain->Present(1, 0);
        if (FAILED(hr))
        {
            return ProcessFailure(m_Device, L"Failed to present", L"Error", hr, SystemTransitionsExpectedErrors);
        }
        else if (hr == DXGI_STATUS_OCCLUDED)
        {
            *Occluded = true;
        }
    }

    return Ret;
}

//
// Returns shared handle
//
HANDLE OUTPUTMANAGER::GetSharedHandle()
{
    HANDLE Hnd = nullptr;

    // QI IDXGIResource interface to synchronized shared surface.
    IDXGIResource* DXGIResource = nullptr;
    HRESULT hr = m_SharedSurf->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&DXGIResource));
    if (SUCCEEDED(hr))
    {
        // Obtain handle to IDXGIResource object.
        DXGIResource->GetSharedHandle(&Hnd);
        DXGIResource->Release();
        DXGIResource = nullptr;
    }

    return Hnd;
}

//
// Draw frame into backbuffer
//
DUPL_RETURN OUTPUTMANAGER::DrawFrame()
{
    HRESULT hr;

    // If window was resized, resize swapchain
    if (m_NeedsResize)
    {
        DUPL_RETURN Ret = ResizeSwapChain();
        if (Ret != DUPL_RETURN_SUCCESS)
        {
            return Ret;
        }
        m_NeedsResize = false;
    }

    // Vertices for drawing whole texture
    VERTEX Vertices[NUMVERTICES] =
    {
        {XMFLOAT3(-1.0f, -1.0f, 0), XMFLOAT2(0.0f, 1.0f)},
        {XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
        {XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
        {XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
        {XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
        {XMFLOAT3(1.0f, 1.0f, 0), XMFLOAT2(1.0f, 0.0f)},
    };

	

	// Rendering NV12 requires two resource views, which represent the luminance and chrominance channels of the YUV formatted texture.
	std::array<ID3D11ShaderResourceView*, 2> const textureViews = {
		m_luminanceView,
		m_chrominanceView
	};

	// Bind the NV12 channels to the shader.
	m_DeviceContext->PSSetShaderResources(
		0,
		textureViews.size(),
		textureViews.data()
	);

    // Set resources
    UINT Stride = sizeof(VERTEX);
    UINT Offset = 0;
    FLOAT blendFactor[4] = {0.f, 0.f, 0.f, 0.f};
    m_DeviceContext->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    m_DeviceContext->OMSetRenderTargets(1, &m_RTV, nullptr);
    m_DeviceContext->VSSetShader(m_VertexShader, nullptr, 0);
    m_DeviceContext->PSSetShader(m_PixelShader, nullptr, 0);
    //m_DeviceContext->PSSetShaderResources(0, 1, &ShaderResource);
    m_DeviceContext->PSSetSamplers(0, 1, &m_SamplerLinear);
    m_DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D11_BUFFER_DESC BufferDesc;
    RtlZeroMemory(&BufferDesc, sizeof(BufferDesc));
    BufferDesc.Usage = D3D11_USAGE_DEFAULT;
    BufferDesc.ByteWidth = sizeof(VERTEX) * NUMVERTICES;
    BufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    BufferDesc.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA InitData;
    RtlZeroMemory(&InitData, sizeof(InitData));
    InitData.pSysMem = Vertices;

    ID3D11Buffer* VertexBuffer = nullptr;

    // Create vertex buffer
    hr = m_Device->CreateBuffer(&BufferDesc, &InitData, &VertexBuffer);
    if (FAILED(hr))
    {
       
        return ProcessFailure(m_Device, L"Failed to create vertex buffer when drawing a frame", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    m_DeviceContext->IASetVertexBuffers(0, 1, &VertexBuffer, &Stride, &Offset);

    // Draw textured quad onto render target
    m_DeviceContext->Draw(NUMVERTICES, 0);

    VertexBuffer->Release();
    VertexBuffer = nullptr;


    return DUPL_RETURN_SUCCESS;
}

//
// Initialize shaders for drawing to screen
//
DUPL_RETURN OUTPUTMANAGER::InitShaders()
{
    HRESULT hr;

    UINT Size = ARRAYSIZE(g_VS);
    hr = m_Device->CreateVertexShader(g_VS, Size, nullptr, &m_VertexShader);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create vertex shader in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

	
	constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 2> Layout =
        {{
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        }};
	

    /*D3D11_INPUT_ELEMENT_DESC Layout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };*/
	//UINT NumElements = 2;// ARRAYSIZE(Layout);
    hr = m_Device->CreateInputLayout(Layout.data(), Layout.size(), g_VS, Size, &m_InputLayout);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create input layout in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }
    m_DeviceContext->IASetInputLayout(m_InputLayout);

    Size = ARRAYSIZE(g_PS);
    hr = m_Device->CreatePixelShader(g_PS, Size, nullptr, &m_PixelShader);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create pixel shader in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    return DUPL_RETURN_SUCCESS;
}

//
// Reset render target view
//
DUPL_RETURN OUTPUTMANAGER::MakeRTV()
{
    // Get backbuffer
    ID3D11Texture2D* BackBuffer = nullptr;
    HRESULT hr = m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&BackBuffer));
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to get backbuffer for making render target view in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Create a render target view
    hr = m_Device->CreateRenderTargetView(BackBuffer, nullptr, &m_RTV);
    BackBuffer->Release();
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create render target view in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Set new render target
    m_DeviceContext->OMSetRenderTargets(1, &m_RTV, nullptr);

    return DUPL_RETURN_SUCCESS;
}

//
// Set new viewport
//
void OUTPUTMANAGER::SetViewPort(UINT Width, UINT Height)
{
    D3D11_VIEWPORT VP;
    VP.Width = static_cast<FLOAT>(Width);
    VP.Height = static_cast<FLOAT>(Height);
    VP.MinDepth = 0.0f;
    VP.MaxDepth = 1.0f;
    VP.TopLeftX = 0;
    VP.TopLeftY = 0;
    m_DeviceContext->RSSetViewports(1, &VP);
}

//
// Resize swapchain
//
DUPL_RETURN OUTPUTMANAGER::ResizeSwapChain()
{
    if (m_RTV)
    {
        m_RTV->Release();
        m_RTV = nullptr;
    }

    RECT WindowRect;
    GetClientRect(m_WindowHandle, &WindowRect);
    UINT Width = WindowRect.right - WindowRect.left;
    UINT Height = WindowRect.bottom - WindowRect.top;

    // Resize swapchain
    DXGI_SWAP_CHAIN_DESC SwapChainDesc;
    m_SwapChain->GetDesc(&SwapChainDesc);
    HRESULT hr = m_SwapChain->ResizeBuffers(SwapChainDesc.BufferCount, Width, Height, SwapChainDesc.BufferDesc.Format, SwapChainDesc.Flags);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to resize swapchain buffers in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Make new render target view
    DUPL_RETURN Ret = MakeRTV();
    if (Ret != DUPL_RETURN_SUCCESS)
    {
        return Ret;
    }

    // Set new viewport
    SetViewPort(Width, Height);

    return Ret;
}

//
// Releases all references
//
void OUTPUTMANAGER::CleanRefs()
{
    if (m_VertexShader)
    {
        m_VertexShader->Release();
        m_VertexShader = nullptr;
    }

    if (m_PixelShader)
    {
        m_PixelShader->Release();
        m_PixelShader = nullptr;
    }

    if (m_InputLayout)
    {
        m_InputLayout->Release();
        m_InputLayout = nullptr;
    }

    if (m_RTV)
    {
        m_RTV->Release();
        m_RTV = nullptr;
    }

    if (m_SamplerLinear)
    {
        m_SamplerLinear->Release();
        m_SamplerLinear = nullptr;
    }

    if (m_BlendState)
    {
        m_BlendState->Release();
        m_BlendState = nullptr;
    }

    if (m_DeviceContext)
    {
        m_DeviceContext->Release();
        m_DeviceContext = nullptr;
    }

    if (m_Device)
    {
        m_Device->Release();
        m_Device = nullptr;
    }

    if (m_SwapChain)
    {
        m_SwapChain->Release();
        m_SwapChain = nullptr;
    }

	if (m_luminanceView)
	{
		m_luminanceView->Release();
		m_luminanceView = nullptr;
	}
	
	if (m_chrominanceView)
	{
		m_chrominanceView->Release();
		m_chrominanceView = nullptr;
	}

    if (m_SharedSurf)
    {
        m_SharedSurf->Release();
        m_SharedSurf = nullptr;
    }

	if (m_AccessibleSurf)
	{
		m_AccessibleSurf->Release();
		m_AccessibleSurf = nullptr;
	}

    if (m_Factory)
    {
        if (m_OcclusionCookie)
        {
            m_Factory->UnregisterOcclusionStatus(m_OcclusionCookie);
            m_OcclusionCookie = 0;
        }
        m_Factory->Release();
        m_Factory = nullptr;
    }
}


_Post_satisfies_(return != DUPL_RETURN_SUCCESS)
DUPL_RETURN ProcessFailure(_In_opt_ ID3D11Device* Device, _In_ LPCWSTR Str, _In_ LPCWSTR Title, HRESULT hr, _In_opt_z_ HRESULT* ExpectedErrors)
{
	HRESULT TranslatedHr;

	// On an error check if the DX device is lost
	if (Device)
	{
		HRESULT DeviceRemovedReason = Device->GetDeviceRemovedReason();

		switch (DeviceRemovedReason)
		{
		case DXGI_ERROR_DEVICE_REMOVED:
		case DXGI_ERROR_DEVICE_RESET:
		case static_cast<HRESULT>(E_OUTOFMEMORY) :
		{
			// Our device has been stopped due to an external event on the GPU so map them all to
			// device removed and continue processing the condition
			TranslatedHr = DXGI_ERROR_DEVICE_REMOVED;
			break;
		}

		case S_OK:
		{
			// Device is not removed so use original error
			TranslatedHr = hr;
			break;
		}

		default:
		{
			// Device is removed but not a error we want to remap
			TranslatedHr = DeviceRemovedReason;
		}
		}
	}
	else
	{
		TranslatedHr = hr;
	}

	// Check if this error was expected or not
	if (ExpectedErrors)
	{
		HRESULT* CurrentResult = ExpectedErrors;

		while (*CurrentResult != S_OK)
		{
			if (*(CurrentResult++) == TranslatedHr)
			{
				return DUPL_RETURN_ERROR_EXPECTED;
			}
		}
	}

	// Error was not expected so display the message box
	DisplayMsg(Str, Title, TranslatedHr);

	return DUPL_RETURN_ERROR_UNEXPECTED;
}

//
// Displays a message
//
void DisplayMsg(_In_ LPCWSTR Str, _In_ LPCWSTR Title, HRESULT hr)
{
	if (SUCCEEDED(hr))
	{
		MessageBoxW(nullptr, Str, Title, MB_OK);
		return;
	}

	const UINT StringLen = (UINT)(wcslen(Str) + sizeof(" with HRESULT 0x########."));
	wchar_t* OutStr = new wchar_t[StringLen];
	if (!OutStr)
	{
		return;
	}

	INT LenWritten = swprintf_s(OutStr, StringLen, L"%s with 0x%X.", Str, hr);
	if (LenWritten != -1)
	{
		MessageBoxW(nullptr, OutStr, Title, MB_OK);
	}

	delete[] OutStr;
}