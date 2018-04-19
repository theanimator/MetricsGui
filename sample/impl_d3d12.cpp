/*
Copyright 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <assert.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <stdint.h>
#include <windows.h>
#include <vector>

#include "impl.h"
#include "impl_d3d12.h"

#include <vs.hlsl.h>
#include <ps.hlsl.h>

ImplD3D12::ImplD3D12()
    : HWnd                   (NULL)
    , Width                  (0)
    , Height                 (0)
    , DxgiFactory4           (nullptr)
    , DxgiSwapChain3         (nullptr)
    , SwapChainWaitableObject(NULL)
    , Device                 (nullptr)
    , CommandQueue           (nullptr)
    , Fence                  (nullptr)
    , FenceEvent             (NULL)
    , LastSignalledFenceValue(0)
    , FrameIndex             (0)
    , RTVHeap                (nullptr)
    , SRVHeap                (nullptr)
    , CmdList                (nullptr)
    , PipelineState          (nullptr)
    , RootSignature          (nullptr)
    , VertexBuffer           (nullptr)
{
    memset(BackBuffer, 0, sizeof(BackBuffer));
    memset(FrameCtxt,  0, sizeof(FrameCtxt));
}

// Helper function for resolving the full path of assets.
std::wstring ImplD3D12::GetAssetFullPath(LPCWSTR assetName)
{
	WCHAR assetsPath[512];
	GetAssetsPath(assetsPath, _countof(assetsPath));
	std::wstring m_assetsPath = assetsPath;
	return m_assetsPath + assetName;
}


bool ImplD3D12::Initialize(
    HWND hwnd)
{
    HWnd = hwnd;

    {
        auto hModule = LoadLibrary("d3d12.dll");
        if (hModule == NULL) {
            return false;
        }

        FreeLibrary(hModule);
    }

#ifdef _DEBUG
    {
        ID3D12Debug* debugController = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            debugController->Release();
        }
    }
#endif

    HR_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&DxgiFactory4)));
    HR_CHECK(DxgiFactory4->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

    IDXGIAdapter1* adapter = nullptr;
    for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != DxgiFactory4->EnumAdapters1(adapterIndex, &adapter); ++adapterIndex) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter->Release();
            continue;
        }

        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
            adapter->Release();
            continue;
        }

        break;
    }

    if (adapter == nullptr) {
        return false;
    }

    HR_CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)));

    adapter->Release();

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = D3D12_NUM_BACK_BUFFERS;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask       = 1;
        HR_CHECK(Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&RTVHeap)));

        auto rtvDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto rtvHandle = RTVHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < D3D12_NUM_BACK_BUFFERS; ++i) {
            BackBuffer[i].Handle = rtvHandle;
            rtvHandle.ptr += rtvDescriptorSize;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 2;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        desc.NodeMask       = 1;
        HR_CHECK(Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&SRVHeap)));
    }

    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 1;
        HR_CHECK(Device->CreateCommandQueue(&desc, IID_PPV_ARGS(&CommandQueue)));
    }

	// Create the root signature.
	{
		D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};

		// This is the highest version the sample supports. If CheckFeatureSupport succeeds, the HighestVersion returned will not be greater than this.
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

		CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

		CD3DX12_ROOT_PARAMETER1 rootParameters[1];
		rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);

		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.MipLODBias = 0;
		sampler.MaxAnisotropy = 0;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
		rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error);
		Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&RootSignature));
	}

	// Create the pipeline state, which includes compiling and loading shaders.
	{
		ComPtr<ID3DBlob> vertexShader;
		ComPtr<ID3DBlob> pixelShader;

#if defined(_DEBUG)
		// Enable better shader debugging with the graphics debugging tools.
		UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
		UINT compileFlags = 0;
#endif
		std::wstring m_assetsPath = GetAssetFullPath(L"shaders.hlsl");


		HR_CHECK(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
		HR_CHECK(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));

		// Define the vertex input layout.
		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		// Describe and create the graphics pipeline state object (PSO).
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
		psoDesc.pRootSignature = RootSignature;
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
		psoDesc.SampleDesc.Count = 1;

        HR_CHECK(Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&PipelineState)));
    }

    {
        Vertex triangle[] = {
            { {  0.0f,  0.5f }, { 1.f, 0.f, 0.f } },
            { {  0.5f, -0.5f }, { 0.f, 1.f, 0.f } },
            { { -0.5f, -0.5f }, { 0.f, 0.f, 1.f } },
        };

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type                 = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask     = 1;
        heapProps.VisibleNodeMask      = 1;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment          = 0;
        desc.Width              = sizeof(triangle);
        desc.Height             = 1;
        desc.DepthOrArraySize   = 1;
        desc.MipLevels          = 1;
        desc.Format             = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count   = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

        HR_CHECK(Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&VertexBuffer)));

        {
            D3D12_RANGE readRange = {};
            D3D12_RANGE writeRange = { 0, sizeof(triangle) };
            void* ptr = nullptr;
            HR_CHECK(VertexBuffer->Map(0, &readRange, &ptr));
            memcpy(ptr, triangle, sizeof(triangle));
            VertexBuffer->Unmap(0, &writeRange);
        }

        VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
        VertexBufferView.SizeInBytes    = sizeof(triangle);
        VertexBufferView.StrideInBytes  = sizeof(Vertex);
    }

    for (UINT i = 0; i < D3D12_NUM_FRAMES_IN_FLIGHT; ++i) {
        HR_CHECK(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&FrameCtxt[i].CommandAllocator)));
    }

    HR_CHECK(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, FrameCtxt[0].CommandAllocator, nullptr, IID_PPV_ARGS(&CmdList)));

	ComPtr<ID3D12Resource> textureUploadHeap;

	// Create the texture.
	{
		std::vector<UINT8> pixels = GenerateTextureData();

		D3D12_HEAP_PROPERTIES props;
		memset(&props, 0, sizeof(D3D12_HEAP_PROPERTIES));
		props.Type = D3D12_HEAP_TYPE_DEFAULT;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC desc;
		ZeroMemory(&desc, sizeof(desc));
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Alignment = 0;
		desc.Width = TextureWidth;
		desc.Height = TextureHeight;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		ID3D12Resource* pTexture = NULL;
		Device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&pTexture));

		UINT uploadPitch = (TextureWidth * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
		UINT uploadSize = TextureHeight * uploadPitch;
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Alignment = 0;
		desc.Width = uploadSize;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		props.Type = D3D12_HEAP_TYPE_UPLOAD;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		ID3D12Resource* uploadBuffer = NULL;
		HRESULT hr = Device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&uploadBuffer));
		assert(SUCCEEDED(hr));

		void* mapped = NULL;
		D3D12_RANGE range = { 0, uploadSize };
		hr = uploadBuffer->Map(0, &range, &mapped);
		assert(SUCCEEDED(hr));
		memcpy((void*)((uintptr_t)mapped), static_cast<void*>(pixels.data()),TextureHeight * TextureWidth * 4);
		uploadBuffer->Unmap(0, &range);

		D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
		srcLocation.pResource = uploadBuffer;
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLocation.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		srcLocation.PlacedFootprint.Footprint.Width = TextureWidth;
		srcLocation.PlacedFootprint.Footprint.Height = TextureHeight;
		srcLocation.PlacedFootprint.Footprint.Depth = 1;
		srcLocation.PlacedFootprint.Footprint.RowPitch = uploadPitch;

		D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
		dstLocation.pResource = pTexture;
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = 0;

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = pTexture;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

		ID3D12Fence* fence = NULL;
		hr = Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
		assert(SUCCEEDED(hr));

		HANDLE event = CreateEvent(0, 0, 0, 0);
		assert(event != NULL);

		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.NodeMask = 1;

		ID3D12CommandQueue* cmdQueue = NULL;
		hr = Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&cmdQueue));
		assert(SUCCEEDED(hr));

		ID3D12CommandAllocator* cmdAlloc = NULL;
		hr = Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
		assert(SUCCEEDED(hr));

		ID3D12GraphicsCommandList* cmdList = NULL;
		hr = Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, NULL, IID_PPV_ARGS(&cmdList));
		assert(SUCCEEDED(hr));

		cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, NULL);
		cmdList->ResourceBarrier(1, &barrier);

		hr = cmdList->Close();
		assert(SUCCEEDED(hr));

		cmdQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&cmdList);
		hr = cmdQueue->Signal(fence, 1);
		assert(SUCCEEDED(hr));

		fence->SetEventOnCompletion(1, event);
		WaitForSingleObject(event, INFINITE);

		cmdList->Release();
		cmdAlloc->Release();
		cmdQueue->Release();
		CloseHandle(event);
		fence->Release();
		uploadBuffer->Release();

		UINT mCbvSrvDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(SRVHeap->GetCPUDescriptorHandleForHeapStart());
		hDescriptor.Offset(1, mCbvSrvDescriptorSize);
		// Describe and create a SRV for the texture.
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		Device->CreateShaderResourceView(pTexture, &srvDesc, hDescriptor);

		// Close the command list and execute it to begin the initial GPU setup.
		HR_CHECK(CmdList->Close());

		HR_CHECK(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));

		FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (FenceEvent == nullptr) {
			HR_CHECK(HRESULT_FROM_WIN32(GetLastError()));
		}
	}

    ImGui_ImplDX12_Init(hwnd, D3D12_NUM_FRAMES_IN_FLIGHT, Device,
		DXGI_FORMAT_B8G8R8A8_UNORM,
        SRVHeap->GetCPUDescriptorHandleForHeapStart(),
        SRVHeap->GetGPUDescriptorHandleForHeapStart());
    ImGui_ImplDX12_CreateDeviceObjects();

    return true;
}

// Generate a simple black and white checkerboard texture.
std::vector<UINT8> ImplD3D12::GenerateTextureData()
{
	const UINT rowPitch = TextureWidth * TexturePixelSize;
	const UINT cellPitch = rowPitch >> 3;		// The width of a cell in the checkboard texture.
	const UINT cellHeight = TextureWidth >> 3;	// The height of a cell in the checkerboard texture.
	const UINT textureSize = rowPitch * TextureHeight;

	std::vector<UINT8> data(textureSize);
	UINT8* pData = &data[0];

	for (UINT n = 0; n < textureSize; n += TexturePixelSize)
	{
		UINT x = n % rowPitch;
		UINT y = n / rowPitch;
		UINT i = x / cellPitch;
		UINT j = y / cellHeight;

		if (i % 2 == j % 2)
		{
			pData[n] = 0xff;		// R
			pData[n + 1] = 0xff;	// G
			pData[n + 2] = 0xff;	// B
			pData[n + 3] = 0xff;	// A
		}
		else
		{
			pData[n] = 0x00;		// R
			pData[n + 1] = 0x00;	// G
			pData[n + 2] = 0x00;	// B
			pData[n + 3] = 0xff;	// A
		}
	}

	return data;
}


void ImplD3D12::Finalize()
{
    WaitForLastSubmittedFrame();

    ImGui_ImplDX12_Shutdown();

    for (UINT i = 0; i < D3D12_NUM_FRAMES_IN_FLIGHT; ++i) {
        SafeRelease(FrameCtxt[i].CommandAllocator);
    }
    SafeRelease(RTVHeap);
    SafeRelease(SRVHeap);
    SafeRelease(CmdList);
    SafeRelease(PipelineState);
    SafeRelease(RootSignature);
    SafeRelease(VertexBuffer);
    SafeRelease(Fence);
    for (UINT i = 0; i < D3D12_NUM_BACK_BUFFERS; ++i) {
        SafeRelease(BackBuffer[i].Resource, D3D12_NUM_BACK_BUFFERS - i - 1);
    }
    SafeRelease(DxgiSwapChain3);
    SafeRelease(CommandQueue);
    SafeRelease(Device);
    SafeRelease(DxgiFactory4);
    CloseHandle(SwapChainWaitableObject);
    CloseHandle(FenceEvent);
}

void ImplD3D12::Resize(
    UINT width,
    UINT height)
{
    WaitForLastSubmittedFrame();

    Width  = width;
    Height = height;

    if (DxgiSwapChain3 != nullptr) {
        DxgiSwapChain3->Release();
        DxgiSwapChain3 = nullptr;
        for (UINT i = 0; i < D3D12_NUM_BACK_BUFFERS; ++i) {
            BackBuffer[i].Resource->Release();
            BackBuffer[i].Resource = nullptr;
        }
        CloseHandle(SwapChainWaitableObject);
    }

    {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width              = width;
        desc.Height             = height;
        desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Stereo             = FALSE;
        desc.SampleDesc.Count   = 1;
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount        = D3D12_NUM_BACK_BUFFERS;
        desc.Scaling            = DXGI_SCALING_STRETCH;
        desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc.Flags              = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        IDXGISwapChain1* swapChain1 = nullptr;
        HR_CHECK(DxgiFactory4->CreateSwapChainForHwnd(CommandQueue, HWnd, &desc, nullptr, nullptr, &swapChain1));
        HR_CHECK(swapChain1->QueryInterface(IID_PPV_ARGS(&DxgiSwapChain3)));
        swapChain1->Release();

        HR_CHECK(DxgiSwapChain3->SetMaximumFrameLatency(D3D12_NUM_BACK_BUFFERS));

        SwapChainWaitableObject = DxgiSwapChain3->GetFrameLatencyWaitableObject();
        if (SwapChainWaitableObject == NULL) {
            HR_CHECK(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    for (UINT i = 0; i < D3D12_NUM_BACK_BUFFERS; ++i) {
        HR_CHECK(DxgiSwapChain3->GetBuffer(i, IID_PPV_ARGS(&BackBuffer[i].Resource)));
        Device->CreateRenderTargetView(BackBuffer[i].Resource, nullptr, BackBuffer[i].Handle);
    }
}

void ImplD3D12::Render(
    uint32_t resourcesIndex)
{
    auto frameCtxt = &FrameCtxt[resourcesIndex];

    auto backBufferIdx = DxgiSwapChain3->GetCurrentBackBufferIndex();

    HR_CHECK(frameCtxt->CommandAllocator->Reset());
    HR_CHECK(CmdList->Reset(frameCtxt->CommandAllocator, PipelineState));

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = BackBuffer[backBufferIdx].Resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    CmdList->ResourceBarrier(1, &barrier);

    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    CmdList->ClearRenderTargetView(BackBuffer[backBufferIdx].Handle, clearColor, 0, nullptr);

    D3D12_VIEWPORT viewport = {};
    viewport.Width    = (float) Width;
    viewport.Height   = (float) Height;
    viewport.MaxDepth = 1.f;

    D3D12_RECT scissorRect = {};
    scissorRect.right  = Width;
    scissorRect.bottom = Height;

    CmdList->SetGraphicsRootSignature(RootSignature);
	ID3D12DescriptorHeap* ppHeaps[] = {SRVHeap };
	CmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
    CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CmdList->IASetVertexBuffers(0, 1, &VertexBufferView);
    CmdList->RSSetViewports(1, &viewport);
    CmdList->OMSetRenderTargets(1, &BackBuffer[backBufferIdx].Handle, FALSE, nullptr);
    CmdList->RSSetScissorRects(1, &scissorRect);

	UINT mCbvSrvDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	CD3DX12_GPU_DESCRIPTOR_HANDLE tex(SRVHeap->GetGPUDescriptorHandleForHeapStart());
	tex.Offset(1, mCbvSrvDescriptorSize);

	CmdList->SetGraphicsRootDescriptorTable(0, tex);

    CmdList->DrawInstanced(3, 1, 0, 0);

	CmdList->SetGraphicsRootDescriptorTable(0, SRVHeap->GetGPUDescriptorHandleForHeapStart());

    ImGui::Render();

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    CmdList->ResourceBarrier(1, &barrier);

    HR_CHECK(CmdList->Close());

    CommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*) &CmdList);

    HR_CHECK(DxgiSwapChain3->Present(1, 0));

    auto fenceValue = LastSignalledFenceValue + 1;
    HR_CHECK(CommandQueue->Signal(Fence, fenceValue));
    LastSignalledFenceValue = fenceValue;
    frameCtxt->FenceValue = fenceValue;
}

void ImplD3D12::WaitForLastSubmittedFrame()
{
    auto frameCtxt = &FrameCtxt[FrameIndex % D3D12_NUM_FRAMES_IN_FLIGHT];
    auto fenceValue = frameCtxt->FenceValue;

    if (fenceValue == 0) { // means no fence was signalled
        return;
    }

    frameCtxt->FenceValue = 0;

    if (Fence->GetCompletedValue() >= fenceValue) {
        return;
    }

    HR_CHECK(Fence->SetEventOnCompletion(fenceValue, FenceEvent));
    WaitForSingleObject(FenceEvent, INFINITE);
}

uint32_t ImplD3D12::WaitForResources()
{
    auto nextFrameIndex = FrameIndex + 1;
    auto nextResourcesIndex = nextFrameIndex % D3D12_NUM_FRAMES_IN_FLIGHT;
    FrameIndex = nextFrameIndex;

    auto frameCtxt = &FrameCtxt[nextResourcesIndex];
    auto fence      = Fence;
    auto fenceEvent = FenceEvent;
    auto fenceValue = frameCtxt->FenceValue;

    HANDLE waitableObjects[] = {
        SwapChainWaitableObject,
        NULL,
    };
    DWORD numWaitableObjects = 1;

    if (fenceValue != 0) { // means no fence was signalled
        frameCtxt->FenceValue = 0;

        HR_CHECK(fence->SetEventOnCompletion(fenceValue, fenceEvent));
        waitableObjects[1] = fenceEvent;
        numWaitableObjects = 2;
    }

    HR_CHECK(WaitForMultipleObjects(numWaitableObjects, waitableObjects, TRUE, INFINITE));

    return nextResourcesIndex;
}

