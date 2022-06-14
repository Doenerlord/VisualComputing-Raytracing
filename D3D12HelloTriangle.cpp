//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include "D3D12HelloTriangle.h"
#include "nv_helpers_dx12/BottomLevelASGenerator.h"
#include "nv_helpers_dx12/RaytracingPipelineGenerator.h"
#include "nv_helpers_dx12/RootSignatureGenerator.h"
#include <stdexcept>
#include <DXRHelper.h>
#include "glm/gtc/type_ptr.hpp"
#include "manipulator.h"
#include "Windowsx.h"

D3D12HelloTriangle::D3D12HelloTriangle(UINT width, UINT height, std::wstring name) :
	DXSample(width, height, name),
	m_frameIndex(0),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
	m_rtvDescriptorSize(0)
{
}

void D3D12HelloTriangle::OnInit()
{

	nv_helpers_dx12::CameraManip.setWindowSize(GetWidth(), GetHeight());
	nv_helpers_dx12::CameraManip.setLookat(glm::vec3(1.5f, 1.5f, 1.5f), glm::vec3(0, 0, 0),
		glm::vec3(0, 1, 0));

	LoadPipeline();
	LoadAssets();

	// Check the raytracing capabilities of the device
	CheckRaytracingSupport();

	// Setup the acceleration structures (AS) for raytracing. When setting up
	// geometry, each bottom-level AS has its own transform matrix.
	CreateAccelerationStructure();

	// Command lists are created in the recording state, but there is
	// nothing to record yet. The main loop expects it to be closed, so close it now.
	ThrowIfFailed(m_commandList->Close());

	// Create the raytracing pipeline, associating the shader code to symbol names
	// and to their root signatures, and defining the amount of memory carried by
	// rays (ray payload)
	CreateRaytracingPipeline();

	// #DXR Extra: Per-Instance Data
	CreatePerInstanceConstantBuffers();

	// #DXR Extra: Per-Instance Data
	// Create a constant buffers, with a color for each vertex of the triangle, for each
	// triangle instance
	CreateGlobalConstantBuffer();

	// Allocate the buffer storing the raytracing output, with the same dimensions
	// as the target image
	CreateRaytracingOutputBuffer();

	// #DXR Extra: Perspective Camera
	// Create a buffer to store the modelview and perspective camera matrices
	CreateCameraBuffer();

	// Create the buffer containing the raytracing result (always output in a
	// UAV), and create the heap referencing the resources used by the raytracing,
	// such as the acceleration structure
	CreateShaderResourceHeap();

	// Create the shader binding table and indicating which shaders
	// are invoked for each instance in the  AS
	CreateShaderBindingTable();
}

// Load the rendering pipeline dependencies.
void D3D12HelloTriangle::LoadPipeline()
{
	UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	ComPtr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	if (m_useWarpDevice)
	{
		ComPtr<IDXGIAdapter> warpAdapter;
		ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

		ThrowIfFailed(D3D12CreateDevice(
			warpAdapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
			IID_PPV_ARGS(&m_device)
			));
	}
	else
	{
		ComPtr<IDXGIAdapter1> hardwareAdapter;
		GetHardwareAdapter(factory.Get(), &hardwareAdapter);

		ThrowIfFailed(D3D12CreateDevice(
			hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
			IID_PPV_ARGS(&m_device)
			));
	}

	// Describe and create the command queue.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = FrameCount;
	swapChainDesc.Width = m_width;
	swapChainDesc.Height = m_height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		m_commandQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
		Win32Application::GetHwnd(),
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain
		));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain.As(&m_swapChain));
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create descriptor heaps.
	{
		// Describe and create a render target view (RTV) descriptor heap.
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = FrameCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

		m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	// Create frame resources.
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

		// Create a RTV for each frame.
		for (UINT n = 0; n < FrameCount; n++)
		{
			ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
			m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
			rtvHandle.Offset(1, m_rtvDescriptorSize);
		}
	}

	ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
}

// Load the sample assets.
void D3D12HelloTriangle::LoadAssets()
{
	// Create an empty root signature.
	{
		// #DXR Extra: Perspective Camera
		// The root signature describes which data is accessed by the shader. The camera matrices are held
		// in a constant buffer, itself referenced the heap. To do this we reference a range in the heap,
		// and use that range as the sole parameter of the shader. The camera buffer is associated in the
		// index 0, making it accessible in the shader in the b0 register.
		CD3DX12_ROOT_PARAMETER constantParameter;
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
		constantParameter.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);

		CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
		rootSignatureDesc.Init(1, &constantParameter, 0, nullptr,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		ThrowIfFailed(D3D12SerializeRootSignature(
			&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
		ThrowIfFailed(m_device->CreateRootSignature(
			0, signature->GetBufferPointer(), signature->GetBufferSize(),
			IID_PPV_ARGS(&m_rootSignature)));
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

		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));

		// Define the vertex input layout.
		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		// Describe and create the graphics pipeline state object (PSO).
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
		psoDesc.pRootSignature = m_rootSignature.Get();
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.SampleDesc.Count = 1;
		// #DXR Extra: Depth Buffering
		// Add support for depth testing, using a 32-bit floating-point depth buffer
		psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

		// #DXR Extra - Refitting
		psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
	}

	// Create the command list.
	ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_commandList)));

	// Create the vertex buffer.
	{
		// Define the geometry for a triangle.
		//Vertex triangleVertices[] = { {{0.0f, 0.25f * m_aspectRatio, 0.0f}, {1.0f, 1.0f, 0.0f, 1.0f}}, {{0.25f, -0.25f * m_aspectRatio, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}}, {{-0.25f, -0.25f * m_aspectRatio, 0.0f}, {1.0f, 0.0f, 1.0f, 1.0f}} };
		Vertex triangleVertices[] = { {{std::sqrtf(8.f / 9.f), 0.f, -1.f / 3.f}, {1.f, 0.f, 0.f, 1.f}},
									  {{-std::sqrtf(2.f / 9.f), std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 1.f, 0.f, 1.f}},
									  {{-std::sqrtf(2.f / 9.f), -std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 0.f, 1.f, 1.f}},
									  {{0.f, 0.f, 1.f}, {1, 0, 1, 1}} 
		};
		const UINT vertexBufferSize = sizeof(triangleVertices);

		// Note: using upload heaps to transfer static data like vert buffers is not 
		// recommended. Every time the GPU needs it, the upload heap will be marshalled 
		// over. Please read up on Default Heap usage. An upload heap is used here for 
		// code simplicity and because there are very few verts to actually transfer.
		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_vertexBuffer)));

		// Copy the triangle data to the vertex buffer.
		UINT8* pVertexDataBegin;
		CD3DX12_RANGE readRange(0, 0);		// We do not intend to read from this resource on the CPU.
		ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
		memcpy(pVertexDataBegin, triangleVertices, sizeof(triangleVertices));
		m_vertexBuffer->Unmap(0, nullptr);

		// Initialize the vertex buffer view.
		m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
		m_vertexBufferView.StrideInBytes = sizeof(Vertex);
		m_vertexBufferView.SizeInBytes = vertexBufferSize;

		CreateMengerSpongeVB();

		//----------------------------------------------------------------------------------------------
		// Indices
		std::vector<UINT> indices = { 0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2 };
		const UINT indexBufferSize = static_cast<UINT>(indices.size()) * sizeof(UINT);

		CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
		ThrowIfFailed(m_device->CreateCommittedResource(
			&heapProperty, D3D12_HEAP_FLAG_NONE, & bufferResource,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_indexBuffer)));

		// Copy the triangle data to the index buffer.
		UINT8* pIndexDataBegin;
		ThrowIfFailed(m_indexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pIndexDataBegin)));
		memcpy(pIndexDataBegin, indices.data(), indexBufferSize);
		m_indexBuffer->Unmap(0, nullptr);

		// Initialize the index buffer view.
		m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
		m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
		m_indexBufferView.SizeInBytes = indexBufferSize;

		CreatePlaneVB();
	}

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
		m_fenceValue = 1;

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForPreviousFrame();
	}
}

// Update frame-based values.
void D3D12HelloTriangle::OnUpdate()
{
	UpdateCameraBuffer();
}

// Render the scene.
void D3D12HelloTriangle::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	// Present the frame.
	ThrowIfFailed(m_swapChain->Present(1, 0));

	WaitForPreviousFrame();
}

void D3D12HelloTriangle::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForPreviousFrame();

	CloseHandle(m_fenceEvent);
}

void D3D12HelloTriangle::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	ThrowIfFailed(m_commandAllocator->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

	// Set necessary state.
	m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	m_commandList->RSSetViewports(1, &m_viewport);
	m_commandList->RSSetScissorRects(1, &m_scissorRect);

	// Indicate that the back buffer will be used as a render target.
	m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
	m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

	// Record commands.
	//const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	//m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	//m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	//m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
	//m_commandList->DrawInstanced(3, 1, 0, 0);
	
	//#DXR
	if (m_raster) {

		std::vector<ID3D12DescriptorHeap*> heaps = { m_constHeap.Get() };
		m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
		m_commandList->SetGraphicsRootDescriptorTable(0, m_constHeap->GetGPUDescriptorHandleForHeapStart());


		const float clearColor[]{ 0.0f, 0.2f, 0.4f, 1.0f };
		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
		//m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
		m_commandList->IASetIndexBuffer(&m_indexBufferView);
		m_commandList->DrawIndexedInstanced(12, 1, 0, 0, 0);


		// #DXR Extra: Per-Instance Data
		// In a way similar to triangle rendering, rasterize the plane
		m_commandList->IASetVertexBuffers(0, 1, &m_planeBufferView);
		m_commandList->DrawInstanced(6, 1, 0, 0);

		// #DXR Extra: Indexed Geometry
		// In a way similar to triangle rendering, rasterize the Menger Sponge
		m_commandList->IASetVertexBuffers(0, 1, &m_mengerVBView);
		m_commandList->IASetIndexBuffer(&m_mengerIBView);
		m_commandList->DrawIndexedInstanced(m_mengerIndexCount, 1, 0, 0, 0);

	}
	else {
		CreateTopLevelAS(m_instances);

		std::vector<ID3D12DescriptorHeap*> heaps = { m_srvUavHeap.Get() };
		m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());

		CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(m_outputResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_commandList->ResourceBarrier(1, &transition);

		D3D12_DISPATCH_RAYS_DESC desc = {};

		desc.RayGenerationShaderRecord.StartAddress = m_sbtStorage->GetGPUVirtualAddress();
		uint32_t rayGenerationSectionSizeInBytes = m_sbtHelper.GetRayGenSectionSize();
		desc.RayGenerationShaderRecord.SizeInBytes = rayGenerationSectionSizeInBytes;

		uint32_t missSectionSizeInBytes = m_sbtHelper.GetMissSectionSize();
		desc.MissShaderTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes;
		desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
		desc.MissShaderTable.StrideInBytes = m_sbtHelper.GetMissEntrySize();

		uint32_t hitGroupsSectionSize = m_sbtHelper.GetHitGroupSectionSize();
		desc.HitGroupTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes + missSectionSizeInBytes;
		desc.HitGroupTable.SizeInBytes = hitGroupsSectionSize;
		desc.HitGroupTable.StrideInBytes = m_sbtHelper.GetHitGroupEntrySize();

		desc.Width = GetWidth();
		desc.Height = GetHeight();
		desc.Depth = 1;

		m_commandList->SetPipelineState1(m_rtStateObject.Get());
		m_commandList->DispatchRays(&desc);

		transition = CD3DX12_RESOURCE_BARRIER::Transition(m_outputResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		m_commandList->ResourceBarrier(1, &transition);
		transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		m_commandList->ResourceBarrier(1, &transition);
		m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(), m_outputResource.Get());
		transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_commandList->ResourceBarrier(1, &transition);
	}

	// Indicate that the back buffer will now be used to present.
	m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(m_commandList->Close());
}

void D3D12HelloTriangle::WaitForPreviousFrame()
{
	// WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
	// This is code implemented as such for simplicity. The D3D12HelloFrameBuffering
	// sample illustrates how to use fences for efficient resource usage and to
	// maximize GPU utilization.

	// Signal and increment the fence value.
	const UINT64 fence = m_fenceValue;
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
	m_fenceValue++;

	// Wait until the previous frame is finished.
	if (m_fence->GetCompletedValue() < fence)
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12HelloTriangle::CheckRaytracingSupport()
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	ThrowIfFailed(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
	if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
	{
		throw std::runtime_error("Raytracing not supported on device");
	}
}

void D3D12HelloTriangle::OnKeyUp(UINT8 key)
{
	if (key == VK_SPACE)
	{
		m_raster = !m_raster;
	}
}

//--------------------------------------------------------------------------------------------------
//
//

void D3D12HelloTriangle::OnButtonDown(UINT32 lParam)
{
	nv_helpers_dx12::CameraManip.setMousePosition(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam));
}

//--------------------------------------------------------------------------------------------------
//
//

void D3D12HelloTriangle::OnMouseMove(UINT8 wParam, UINT32 lParam)
{
	using nv_helpers_dx12::Manipulator;
	Manipulator::Inputs inputs;
	inputs.lmb = wParam & MK_LBUTTON;
	inputs.mmb = wParam & MK_MBUTTON;
	inputs.rmb = wParam & MK_RBUTTON;
	if (!inputs.lmb && !inputs.rmb && !inputs.mmb)
		return; // no mouse button pressed

	inputs.ctrl = GetAsyncKeyState(VK_CONTROL);
	inputs.shift = GetAsyncKeyState(VK_SHIFT);
	inputs.alt = GetAsyncKeyState(VK_MENU);

	CameraManip.mouseMove(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam), inputs);
}



D3D12HelloTriangle::AccelerationStructureBuffers D3D12HelloTriangle::CreateBottomLevelAS(std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vVertexBuffers, std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vIndexBuffers)
{
	nv_helpers_dx12::BottomLevelASGenerator bottomLevelAS;

	// Adding all vertex buffers and not transforming their position.
	for (size_t i = 0; i < vVertexBuffers.size(); i++) {
		// for (const auto &buffer; : vVertexBuffers) {
		if (i < vIndexBuffers.size() && vIndexBuffers[i].second > 0)
			bottomLevelAS.AddVertexBuffer(vVertexBuffers[i].first.Get(), 0,
				vVertexBuffers[i].second, sizeof(Vertex),
				vIndexBuffers[i].first.Get(), 0,
				vIndexBuffers[i].second, nullptr, 0, true);

		else
			bottomLevelAS.AddVertexBuffer(vVertexBuffers[i].first.Get(), 0,
				vVertexBuffers[i].second, sizeof(Vertex), 0,
				0);
	}

	UINT64 scratchSize, resultSize;
	bottomLevelAS.ComputeASBufferSizes(m_device.Get(), false, &scratchSize, &resultSize);

	AccelerationStructureBuffers buffers;
	buffers.pScratch = nv_helpers_dx12::CreateBuffer(m_device.Get(), scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nv_helpers_dx12::kDefaultHeapProps);
	buffers.pResult = nv_helpers_dx12::CreateBuffer(m_device.Get(), resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);

	bottomLevelAS.Generate(m_commandList.Get(), buffers.pScratch.Get(), buffers.pResult.Get(), false, nullptr);

	return buffers;
}

void D3D12HelloTriangle::CreateTopLevelAS(const std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>>& instances)
{
	//for (size_t i = 0; i < instances.size(); i++) { 
	//	m_topLevelASGenerator.AddInstance(instances[i].first.Get(), instances[i].second, static_cast<UINT>(i), static_cast<UINT>(0)); 
	//} 
	// #DXR Extra: Per-Instance Data
	for (size_t i = 0; i < instances.size(); i++)
	{
		m_topLevelASGenerator.AddInstance(instances[i].first.Get(), instances[i].second,
			static_cast<UINT>(i), static_cast<UINT>(2*i));
	}

	UINT64 scratchSize, resultSize, instanceDescsSize;

	m_topLevelASGenerator.ComputeASBufferSizes(m_device.Get(), true, &scratchSize, &resultSize, &instanceDescsSize); 
	m_topLevelASBuffers.pScratch = nv_helpers_dx12::CreateBuffer( m_device.Get(), scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nv_helpers_dx12::kDefaultHeapProps);

	m_topLevelASBuffers.pResult = nv_helpers_dx12::CreateBuffer( m_device.Get(), resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);
	m_topLevelASBuffers.pInstanceDesc = nv_helpers_dx12::CreateBuffer( m_device.Get(), instanceDescsSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

	m_topLevelASGenerator.Generate(m_commandList.Get(), m_topLevelASBuffers.pScratch.Get(), m_topLevelASBuffers.pResult.Get(), m_topLevelASBuffers.pInstanceDesc.Get());
}

void D3D12HelloTriangle::CreateAccelerationStructure()
{	
	/*AccelerationStructureBuffers bottomLevelBuffers = CreateBottomLevelAS(
		{ {m_vertexBuffer.Get(), 4} }, { {m_indexBuffer.Get(), 12} });*/

	// #DXR Extra: Indexed Geometry
	// Build the bottom AS from the Menger Sponge vertex buffer
	// #DXR Extra: Indexed Geometry
	// Build the bottom AS from the Menger Sponge vertex buffer
	AccelerationStructureBuffers mengerBottomLevelBuffers =
		CreateBottomLevelAS({ {m_mengerVB.Get(), m_mengerVertexCount} },
			{ {m_mengerIB.Get(), m_mengerIndexCount} });

	// #DXR Extra: Per-Instance Data
	AccelerationStructureBuffers planeBottomLevelBuffers =
		CreateBottomLevelAS({ {m_planeBuffer.Get(), 6} });

	// Just one instance for now
	// #DXR Extra: Per-Instance Data
	// 3 instances of the triangle
	// 3 instances of the triangle + a plane
	m_instances = {
		{mengerBottomLevelBuffers.pResult, XMMatrixIdentity()},
		//{bottomLevelBuffers.pResult, XMMatrixTranslation(.6f, 0, 0)},
		//{bottomLevelBuffers.pResult, XMMatrixTranslation(-.6f, 0, 0)},
		// #DXR Extra: Per-Instance Data
		{planeBottomLevelBuffers.pResult, XMMatrixTranslation(0, 0, 0)} };
	CreateTopLevelAS(m_instances);

	// Flush the command list and wait for it to finish
	m_commandList->Close();
	ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
	m_fenceValue++;
	m_commandQueue->Signal(m_fence.Get(), m_fenceValue);

	m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
	WaitForSingleObject(m_fenceEvent, INFINITE);

	// Once the command list is finished executing, reset it to be reused for
	// rendering
	ThrowIfFailed(
		m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

	// Store the AS buffers. The rest of the buffers will be released once we exit
	// the function
	//m_bottomLevelAS = bottomLevelBuffers.pResult;
}

ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateRayGenSignature()
{
		nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddHeapRangesParameter(
		{
			{
				0 /*u0*/,
				1 /*1 descriptor */,
				0 /*use the implicit register space 0*/,
				D3D12_DESCRIPTOR_RANGE_TYPE_UAV /* UAV representing the output buffer*/,
				0 /*heap slot where the UAV is defined*/
			},
			{
				0 /*t0*/,
				1,
				0,
				D3D12_DESCRIPTOR_RANGE_TYPE_SRV /*Top-level acceleration structure*/,
				1
			},
			{
				0 /*b0*/,
				1,
				0,
				D3D12_DESCRIPTOR_RANGE_TYPE_CBV /*Camera parameters*/,
				2
			}
		});

	return rsc.Generate(m_device.Get(), true);
}

ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateMissSignature()
{
	nv_helpers_dx12::RootSignatureGenerator rsc;
	return rsc.Generate(m_device.Get(), true);
}

ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateHitSignature()
{
	nv_helpers_dx12::RootSignatureGenerator rsc;

	//rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV);
	// #DXR Extra: Per-Instance Data
	// The vertex colors may differ for each instance, so it is not possible to
	// point to a single buffer in the heap. Instead we use the concept of root
	// parameters, which are defined directly by a pointer in memory. In the
	// shader binding table we will associate each hit shader instance with its
	// constant buffer. Here we bind the buffer to the first slot, accessible in
	// HLSL as register(b0)
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0);
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1);
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0);
	// #DXR Extra - Another ray type
	// Add a single range pointing to the TLAS in the heap
	rsc.AddHeapRangesParameter({
	  { 2 /*t2*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1 /*2nd slot of the heap*/ },
		});
	return rsc.Generate(m_device.Get(), true);
}

void D3D12HelloTriangle::CreateRaytracingPipeline()
{
	nv_helpers_dx12::RayTracingPipelineGenerator pipeline(m_device.Get());

	m_rayGenLibrary = nv_helpers_dx12::CompileShaderLibrary(L"RayGen.hlsl");
	m_missLibrary = nv_helpers_dx12::CompileShaderLibrary(L"Miss.hlsl");
	m_hitLibrary = nv_helpers_dx12::CompileShaderLibrary(L"Hit.hlsl");
	m_shadowLibrary = nv_helpers_dx12::CompileShaderLibrary(L"ShadowRay.hlsl");

	pipeline.AddLibrary(m_rayGenLibrary.Get(), { L"RayGen" });
	pipeline.AddLibrary(m_missLibrary.Get(), { L"Miss" });
	pipeline.AddLibrary(m_hitLibrary.Get(), { L"ClosestHit", L"PlaneClosestHit"});
	pipeline.AddLibrary(m_shadowLibrary.Get(), { L"ShadowClosestHit", L"ShadowMiss" });

	m_rayGenSignature = CreateRayGenSignature();
	m_missSignature = CreateMissSignature();
	m_hitSignature = CreateHitSignature();
	m_shadowSignature = CreateHitSignature();

	pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");
	// #DXR Extra: Per-Instance Data
	pipeline.AddHitGroup(L"PlaneHitGroup", L"PlaneClosestHit");
	// #DXR Extra - Another ray type
	// Hit group for all geometry when hit by a shadow ray
	pipeline.AddHitGroup(L"ShadowHitGroup", L"ShadowClosestHit");

	pipeline.AddRootSignatureAssociation(m_rayGenSignature.Get(), { L"RayGen" });
	//pipeline.AddRootSignatureAssociation(m_missSignature.Get(), { L"Miss" });
	// #DXR Extra - Another ray type
	pipeline.AddRootSignatureAssociation(m_missSignature.Get(), { L"Miss", L"ShadowMiss" });
	pipeline.AddRootSignatureAssociation(m_hitSignature.Get(), { L"HitGroup", L"PlaneHitGroup"});
	// #DXR Extra - Another ray type
	pipeline.AddRootSignatureAssociation(m_shadowSignature.Get(), { L"ShadowHitGroup" });

	pipeline.SetMaxPayloadSize(4 * sizeof(float)); // RGB + distance
	pipeline.SetMaxAttributeSize(2 * sizeof(float)); // barycentric coordinates
	pipeline.SetMaxRecursionDepth(2);

	m_rtStateObject = pipeline.Generate();
	ThrowIfFailed(m_rtStateObject->QueryInterface(IID_PPV_ARGS(&m_rtStateObjectProps)));
}

void D3D12HelloTriangle::CreateRaytracingOutputBuffer()
{
	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.DepthOrArraySize = 1;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; 
	resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; 
	resDesc.Width = GetWidth(); 
	resDesc.Height = GetHeight();
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resDesc.MipLevels = 1;
	resDesc.SampleDesc.Count = 1;
	ThrowIfFailed(m_device->CreateCommittedResource(
		&nv_helpers_dx12::kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&m_outputResource)));
}

void D3D12HelloTriangle::CreateShaderResourceHeap()
{
	m_srvUavHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 3, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);
	D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {}; 
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	m_device->CreateUnorderedAccessView(m_outputResource.Get(), nullptr, &uavDesc, srvHandle);

	srvHandle.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location = m_topLevelASBuffers.pResult->GetGPUVirtualAddress();

	m_device->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);

	// #DXR Extra: Perspective Camera
	// Add the constant buffer for the camera after the TLAS
	srvHandle.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	// Describe and create a constant buffer view for the camera
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = m_cameraBufferSize;
	m_device->CreateConstantBufferView(&cbvDesc, srvHandle);
}

void D3D12HelloTriangle::CreateShaderBindingTable()
{
	m_sbtHelper.Reset();
	D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();

	auto heapPointer = reinterpret_cast<uint64_t*>(srvUavHeapHandle.ptr);
	m_sbtHelper.AddRayGenerationProgram(L"RayGen", { heapPointer });
	m_sbtHelper.AddMissProgram(L"Miss", {});

	// #DXR Extra - Another ray type
	m_sbtHelper.AddMissProgram(L"ShadowMiss", {});
	
	// #DXR Extra: Per-Instance Data
	// We have 3 triangles, each of which needs to access its own constant buffer
	// as a root parameter in its primary hit shader. The shadow hit only sets a
	// boolean visibility in the payload, and does not require external data
	//for (int i = 0; i < 3; ++i) {
		m_sbtHelper.AddHitGroup(
        L"HitGroup",
        {(void *)(m_mengerVB->GetGPUVirtualAddress()),
         (void *)(m_mengerIB->GetGPUVirtualAddress()),
         (void *)(m_perInstanceConstantBuffers[0]->GetGPUVirtualAddress())});
		// #DXR Extra - Another ray type
		m_sbtHelper.AddHitGroup(L"ShadowHitGroup", {});
	//}

	// #DXR Extra: Per-Instance Data
	// Adding the plane
// #DXR Extra - Another ray type
m_sbtHelper.AddHitGroup(L"PlaneHitGroup", 
                        {(void*)(m_perInstanceConstantBuffers[0]->GetGPUVirtualAddress()), heapPointer});

	const uint32_t sbtSize = m_sbtHelper.ComputeSBTSize();
	m_sbtStorage = nv_helpers_dx12::CreateBuffer(m_device.Get(), sbtSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
	if (!m_sbtStorage)
	{
		throw std::logic_error("Could not allocate the shader binding table");
	}

	m_sbtHelper.Generate(m_sbtStorage.Get(), m_rtStateObjectProps.Get());
}

void D3D12HelloTriangle::CreateCameraBuffer()
{
	uint32_t nbMatrix = 4; // view, perspective, viewInv, perspectiveInv 
	m_cameraBufferSize = nbMatrix * sizeof(XMMATRIX); 
	
	// Create the constant buffer for all matrices 
	m_cameraBuffer = nv_helpers_dx12::CreateBuffer( m_device.Get(), m_cameraBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps); 
	
	// Create a descriptor heap that will be used by the rasterization shaders 
	m_constHeap = nv_helpers_dx12::CreateDescriptorHeap( m_device.Get(), 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true); 
	
	// Describe and create the constant buffer view. 
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {}; cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress(); cbvDesc.SizeInBytes = m_cameraBufferSize; 

	// Get a handle to the heap memory on the CPU side, to be able to write the 
    // descriptors directly 
	D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_constHeap->GetCPUDescriptorHandleForHeapStart(); m_device->CreateConstantBufferView(&cbvDesc, srvHandle);
}

void D3D12HelloTriangle::UpdateCameraBuffer()
{
	std::vector<XMMATRIX> matrices(4); // Initialize the view matrix, ideally this should be based on user 
									   // interactions The lookat and perspective matrices used for rasterization are 
									   // defined to transform world-space vertices into a [0,1]x[0,1]x[0,1] camera 
									   // space 
	XMVECTOR Eye = XMVectorSet(1.5f, 1.5f, 1.5f, 0.0f); XMVECTOR At = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f); 
	XMVECTOR Up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f); 
	//matrices[0] = XMMatrixLookAtRH(Eye, At, Up);
	const glm::mat4& mat = nv_helpers_dx12::CameraManip.getMatrix();
	memcpy(&matrices[0].r->m128_f32[0], glm::value_ptr(mat), 16 * sizeof(float));
	float fovAngleY = 45.0f * XM_PI / 180.0f; 
	matrices[1] = XMMatrixPerspectiveFovRH(fovAngleY, m_aspectRatio, 0.1f, 1000.0f); // Raytracing has to do the contrary of rasterization: rays are defined in 
																					 // camera space, and are transformed into world space. To do this, we need to 
																					 // store the inverse matrices as well. 
	XMVECTOR det; 
	matrices[2] = XMMatrixInverse(&det, matrices[0]); matrices[3] = XMMatrixInverse(&det, matrices[1]); // Copy the matrix contents 
	uint8_t *pData; 
	ThrowIfFailed(m_cameraBuffer->Map(0, nullptr, (void **)&pData)); 
	memcpy(pData, matrices.data(), m_cameraBufferSize); 
	m_cameraBuffer->Unmap(0, nullptr);
}

void D3D12HelloTriangle::CreatePlaneVB()
{
	// Define the geometry for a plane.
	Vertex planeVertices[] = {
		{{-1.5f, -.8f, 01.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 0
		{{-1.5f, -.8f, -1.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 1
		{{01.5f, -.8f, 01.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 2
		{{01.5f, -.8f, 01.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 2
		{{-1.5f, -.8f, -1.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 1
		{{01.5f, -.8f, -1.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}  // 4
	};

	const UINT planeBufferSize = sizeof(planeVertices);

	// Note: using upload heaps to transfer static data like vert buffers is not
	// recommended. Every time the GPU needs it, the upload heap will be
	// marshalled over. Please read up on Default Heap usage. An upload heap is
	// used here for code simplicity and because there are very few verts to
	// actually transfer.
	CD3DX12_HEAP_PROPERTIES heapProperty =
		CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC bufferResource =
		CD3DX12_RESOURCE_DESC::Buffer(planeBufferSize);
	ThrowIfFailed(m_device->CreateCommittedResource(
		&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&m_planeBuffer)));

	// Copy the triangle data to the vertex buffer.
	UINT8* pVertexDataBegin;
	CD3DX12_RANGE readRange(
		0, 0); // We do not intend to read from this resource on the CPU.
	ThrowIfFailed(m_planeBuffer->Map(
		0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
	memcpy(pVertexDataBegin, planeVertices, sizeof(planeVertices));
	m_planeBuffer->Unmap(0, nullptr);

	// Initialize the vertex buffer view.
	m_planeBufferView.BufferLocation = m_planeBuffer->GetGPUVirtualAddress();
	m_planeBufferView.StrideInBytes = sizeof(Vertex);
	m_planeBufferView.SizeInBytes = planeBufferSize;

}

void D3D12HelloTriangle::CreateGlobalConstantBuffer()
{
	// Due to HLSL packing rules, we create the CB with 9 float4 (each needs to start on a 16-byte
	// boundary)
	XMVECTOR bufferData[] = {
		// A
		XMVECTOR{1.0f, 0.0f, 0.0f, 1.0f},
		XMVECTOR{0.7f, 0.4f, 0.0f, 1.0f},
		XMVECTOR{0.4f, 0.7f, 0.0f, 1.0f},

		// B
		XMVECTOR{0.0f, 1.0f, 0.0f, 1.0f},
		XMVECTOR{0.0f, 0.7f, 0.4f, 1.0f},
		XMVECTOR{0.0f, 0.4f, 0.7f, 1.0f},

		// C
		XMVECTOR{0.0f, 0.0f, 1.0f, 1.0f},
		XMVECTOR{0.4f, 0.0f, 0.7f, 1.0f},
		XMVECTOR{0.7f, 0.0f, 0.4f, 1.0f},
	};

	// Create our buffer
	m_globalConstantBuffer = nv_helpers_dx12::CreateBuffer(
		m_device.Get(), sizeof(bufferData), D3D12_RESOURCE_FLAG_NONE,
		D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

	// Copy CPU memory to GPU
	uint8_t* pData;
	ThrowIfFailed(m_globalConstantBuffer->Map(0, nullptr, (void**)&pData));
	memcpy(pData, bufferData, sizeof(bufferData));
	m_globalConstantBuffer->Unmap(0, nullptr);
}

void D3D12HelloTriangle::CreatePerInstanceConstantBuffers()
{
	// Due to HLSL packing rules, we create the CB with 9 float4 (each needs to start on a 16-byte
  // boundary)
	XMVECTOR bufferData[] = {
		// A
		XMVECTOR{1.0f, 0.0f, 0.0f, 1.0f},
		XMVECTOR{1.0f, 0.4f, 0.0f, 1.0f},
		XMVECTOR{1.f, 0.7f, 0.0f, 1.0f},

		// B
		XMVECTOR{0.0f, 1.0f, 0.0f, 1.0f},
		XMVECTOR{0.0f, 1.0f, 0.4f, 1.0f},
		XMVECTOR{0.0f, 1.0f, 0.7f, 1.0f},

		// C
		XMVECTOR{0.0f, 0.0f, 1.0f, 1.0f},
		XMVECTOR{0.4f, 0.0f, 1.0f, 1.0f},
		XMVECTOR{0.7f, 0.0f, 1.0f, 1.0f},
	};

	m_perInstanceConstantBuffers.resize(3);
	int i(0);
	for (auto& cb : m_perInstanceConstantBuffers)
	{
		const uint32_t bufferSize = sizeof(XMVECTOR) * 3;
		cb = nv_helpers_dx12::CreateBuffer(m_device.Get(), bufferSize, D3D12_RESOURCE_FLAG_NONE,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nv_helpers_dx12::kUploadHeapProps);
		uint8_t* pData;
		ThrowIfFailed(cb->Map(0, nullptr, (void**)&pData));
		memcpy(pData, &bufferData[i * 3] , bufferSize);
		cb->Unmap(0, nullptr);
		++i;
	}
}

void D3D12HelloTriangle::CreateMengerSpongeVB()
{
	std::vector< Vertex > vertices;
	std::vector< UINT > indices;

	nv_helpers_dx12::GenerateMengerSponge(3, 0.75, vertices, indices);
	{
		const UINT mengerVBSize = static_cast<UINT>(vertices.size()) * sizeof(Vertex);

		// Note: using upload heaps to transfer static data like vert buffers is not
		// recommended. Every time the GPU needs it, the upload heap will be
		// marshalled over. Please read up on Default Heap usage. An upload heap is
		// used here for code simplicity and because there are very few verts to
		// actually transfer.
		CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(mengerVBSize);
		ThrowIfFailed(m_device->CreateCommittedResource(
			&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource, //
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_mengerVB)));

		// Copy the triangle data to the vertex buffer.
		UINT8* pVertexDataBegin;
		CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
		ThrowIfFailed(m_mengerVB->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
		memcpy(pVertexDataBegin, vertices.data(), mengerVBSize);
		m_mengerVB->Unmap(0, nullptr);

		// Initialize the vertex buffer view.
		m_mengerVBView.BufferLocation = m_mengerVB->GetGPUVirtualAddress();
		m_mengerVBView.StrideInBytes = sizeof(Vertex);
		m_mengerVBView.SizeInBytes = mengerVBSize;
	}
	{
		const UINT mengerIBSize = static_cast<UINT>(indices.size()) * sizeof(UINT);

		// Note: using upload heaps to transfer static data like vert buffers is not
		// recommended. Every time the GPU needs it, the upload heap will be
		// marshalled over. Please read up on Default Heap usage. An upload heap is
		// used here for code simplicity and because there are very few verts to
		// actually transfer.
		CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(mengerIBSize);
		ThrowIfFailed(m_device->CreateCommittedResource(
			&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource, //
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_mengerIB)));

		// Copy the triangle data to the index buffer.
		UINT8* pIndexDataBegin;
		CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
		ThrowIfFailed(m_mengerIB->Map(0, &readRange, reinterpret_cast<void**>(&pIndexDataBegin)));
		memcpy(pIndexDataBegin, indices.data(), mengerIBSize);
		m_mengerIB->Unmap(0, nullptr);

		// Initialize the index buffer view.
		m_mengerIBView.BufferLocation = m_mengerIB->GetGPUVirtualAddress();
		m_mengerIBView.Format = DXGI_FORMAT_R32_UINT;
		m_mengerIBView.SizeInBytes = mengerIBSize;


		m_mengerIndexCount = static_cast<UINT>(indices.size());
		m_mengerVertexCount = static_cast<UINT>(vertices.size());
	}
}
