
#include <windows.h>
#include "resource.h"

#include <DXGI1_5.h>
#include <d3d11.h>
#include <D3Dcompiler.h>
#include <d3dx9.h>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "xwin.h"
#include "dxwrap.h"
#include "win32.h"
#include "win32gfx.h"
#include "direct3d.h"
#include "uae.h"
#include "custom.h"

void (*D3D_free)(bool immediate);
const TCHAR* (*D3D_init)(HWND ahwnd, int w_w, int h_h, int depth, int *freq, int mmult);
bool (*D3D_alloctexture)(int, int);
void(*D3D_refresh)(void);
bool(*D3D_renderframe)(bool);
void(*D3D_showframe)(void);
void(*D3D_showframe_special)(int);
uae_u8* (*D3D_locktexture)(int*, int*, bool);
void (*D3D_unlocktexture)(void);
void (*D3D_flushtexture)(int miny, int maxy);
void (*D3D_guimode)(bool);
HDC (*D3D_getDC)(HDC hdc);
int (*D3D_isenabled)(void);
void (*D3D_clear)(void);
int (*D3D_canshaders)(void);
int (*D3D_goodenough)(void);
void (*D3D_setcursor)(int x, int y, int width, int height, bool visible, bool noscale);
bool (*D3D_getvblankpos)(int *vpos);
double (*D3D_getrefreshrate)(void);
void (*D3D_vblank_reset)(double freq);
void(*D3D_restore)(void);

static HANDLE hd3d11, hdxgi, hd3dcompiler;

static const float SCREEN_DEPTH = 1000.0f;
static const float SCREEN_NEAR = 0.1f;

struct d3d11struct
{
	IDXGISwapChain1* m_swapChain;
	ID3D11Device* m_device;
	ID3D11DeviceContext* m_deviceContext;
	ID3D11RenderTargetView* m_renderTargetView;
	ID3D11RasterizerState* m_rasterState;
	D3DXMATRIX m_projectionMatrix;
	D3DXMATRIX m_worldMatrix;
	D3DXMATRIX m_orthoMatrix;
	ID3D11Buffer *m_vertexBuffer, *m_indexBuffer;
	ID3D11Buffer* m_matrixBuffer;
	int m_screenWidth, m_screenHeight;
	int m_bitmapWidth, m_bitmapHeight;
	int m_vertexCount, m_indexCount;
	float m_positionX, m_positionY, m_positionZ;
	float m_rotationX, m_rotationY, m_rotationZ;
	D3DXMATRIX m_viewMatrix;
	ID3D11ShaderResourceView *texture;
	ID3D11Texture2D *texture2d;
	ID3D11VertexShader* m_vertexShader;
	ID3D11PixelShader* m_pixelShader;
	ID3D11SamplerState* m_sampleState;
	ID3D11InputLayout* m_layout;
	int texturelocked;
	DXGI_FORMAT format;
	bool m_tearingSupport;
};

struct VertexType
{
	D3DXVECTOR3 position;
	D3DXVECTOR2 texture;
};

struct MatrixBufferType
{
	D3DXMATRIX world;
	D3DXMATRIX view;
	D3DXMATRIX projection;
};

static struct d3d11struct d3d11data[1];

typedef HRESULT (WINAPI* CREATEDXGIFACTORY1)(REFIID riid, void **ppFactory);
typedef HRESULT (WINAPI* D3DCOMPILEFROMFILE)(LPCWSTR pFileName,
	CONST D3D_SHADER_MACRO* pDefines,
	ID3DInclude* pInclude,
	LPCSTR pEntrypoint,
	LPCSTR pTarget,
	UINT Flags1,
	UINT Flags2,
	ID3DBlob** ppCode,
	ID3DBlob** ppErrorMsgs);

static PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice;
static CREATEDXGIFACTORY1 pCreateDXGIFactory1;
static D3DCOMPILEFROMFILE pD3DCompileFromFile;

static void FreeTexture(void)
{
	struct d3d11struct *d3d = &d3d11data[0];

	if (d3d->texture)
		d3d->texture->Release();
	d3d->texture = NULL;

	if (d3d->texture2d)
		d3d->texture2d->Release();
	d3d->texture2d = NULL;
}

static bool CreateTexture(void)
{
	struct d3d11struct *d3d = &d3d11data[0];

	D3D11_TEXTURE2D_DESC desc;

	FreeTexture();

	memset(&desc, 0, sizeof desc);
	desc.Width = d3d->m_bitmapWidth;
	desc.Height = d3d->m_bitmapHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = d3d->format;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	HRESULT hr = d3d->m_device->CreateTexture2D(&desc, NULL, &d3d->texture2d);
	if (FAILED(hr)) {
		write_log(_T("CreateTexture2D failed: %08x\n"), hr);
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = desc.MipLevels;
	srvDesc.Format = d3d->format;

	hr = d3d->m_device->CreateShaderResourceView(d3d->texture2d, &srvDesc, &d3d->texture);
	if (FAILED(hr)) {
		write_log(_T("CreateShaderResourceView failed: %08x\n"), hr);
		return false;
	}

	return true;
}

static void OutputShaderErrorMessage(ID3D10Blob* errorMessage, HWND hwnd, TCHAR* shaderFilename)
{
	char* compileErrors;
	unsigned long bufferSize;

	if (errorMessage) {
		// Get a pointer to the error message text buffer.
		compileErrors = (char*)(errorMessage->GetBufferPointer());

		// Get the length of the message.
		bufferSize = errorMessage->GetBufferSize();

		write_log(_T("D3D11 Shader error: %s"), compileErrors);

		// Release the error message.
		errorMessage->Release();
	} else {

		write_log(_T("D3D11 Shader error: %08x"), GetLastError());

	}
}

static bool TextureShaderClass_InitializeShader(ID3D11Device* device, HWND hwnd)
{
	struct d3d11struct *d3d = &d3d11data[0];

	HRESULT result;
	ID3D10Blob* errorMessage;
	ID3D10Blob* vertexShaderBuffer;
	ID3D10Blob* pixelShaderBuffer;
	D3D11_INPUT_ELEMENT_DESC polygonLayout[2];
	unsigned int numElements;
	D3D11_BUFFER_DESC matrixBufferDesc;
	D3D11_SAMPLER_DESC samplerDesc;
	bool plugin_path;
	TCHAR tmp[MAX_DPATH], tmp2[MAX_DPATH];

	plugin_path = get_plugin_path(tmp, sizeof tmp / sizeof(TCHAR), _T("filtershaders\\direct3d11"));
	if (!plugin_path)
		return false;

	// Initialize the pointers this function will use to null.
	errorMessage = 0;
	vertexShaderBuffer = 0;
	pixelShaderBuffer = 0;

	// Compile the vertex shader code.
	_tcscpy(tmp2, tmp);
	_tcscat(tmp2, _T("_winuae.vs"));
	//result = D3DX11CompileFromFile(vsFilename, NULL, NULL, "TextureVertexShader", "vs_5_0", D3D10_SHADER_ENABLE_STRICTNESS, 0, NULL, &vertexShaderBuffer, &errorMessage, NULL);
	result = pD3DCompileFromFile(tmp2, NULL, NULL, "TextureVertexShader", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexShaderBuffer, &errorMessage);
	if (FAILED(result))
	{
		OutputShaderErrorMessage(errorMessage, hwnd, tmp2);
		return false;
	}

	// Compile the pixel shader code.
	_tcscpy(tmp2, tmp);
	_tcscat(tmp2, _T("_winuae.ps"));
	//result = D3DX11CompileFromFile(psFilename, NULL, NULL, "TexturePixelShader", "ps_5_0", D3D10_SHADER_ENABLE_STRICTNESS, 0, NULL, &pixelShaderBuffer, &errorMessage, NULL);
	result = pD3DCompileFromFile(tmp2, NULL, NULL, "TexturePixelShader", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixelShaderBuffer, &errorMessage);
	if (FAILED(result))
	{
		OutputShaderErrorMessage(errorMessage, hwnd, tmp2);
		return false;
	}

	// Create the vertex shader from the buffer.
	result = device->CreateVertexShader(vertexShaderBuffer->GetBufferPointer(), vertexShaderBuffer->GetBufferSize(), NULL, &d3d->m_vertexShader);
	if (FAILED(result))
	{
		return false;
	}

	// Create the pixel shader from the buffer.
	result = device->CreatePixelShader(pixelShaderBuffer->GetBufferPointer(), pixelShaderBuffer->GetBufferSize(), NULL, &d3d->m_pixelShader);
	if (FAILED(result))
	{
		return false;
	}

	// Create the vertex input layout description.
	// This setup needs to match the VertexType stucture in the ModelClass and in the shader.
	polygonLayout[0].SemanticName = "POSITION";
	polygonLayout[0].SemanticIndex = 0;
	polygonLayout[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	polygonLayout[0].InputSlot = 0;
	polygonLayout[0].AlignedByteOffset = 0;
	polygonLayout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
	polygonLayout[0].InstanceDataStepRate = 0;

	polygonLayout[1].SemanticName = "TEXCOORD";
	polygonLayout[1].SemanticIndex = 0;
	polygonLayout[1].Format = DXGI_FORMAT_R32G32_FLOAT;
	polygonLayout[1].InputSlot = 0;
	polygonLayout[1].AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;
	polygonLayout[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
	polygonLayout[1].InstanceDataStepRate = 0;

	// Get a count of the elements in the layout.
	numElements = sizeof(polygonLayout) / sizeof(polygonLayout[0]);

	// Create the vertex input layout.
	result = device->CreateInputLayout(polygonLayout, numElements, vertexShaderBuffer->GetBufferPointer(), vertexShaderBuffer->GetBufferSize(), &d3d->m_layout);
	if (FAILED(result))
	{
		return false;
	}

	// Release the vertex shader buffer and pixel shader buffer since they are no longer needed.
	vertexShaderBuffer->Release();
	vertexShaderBuffer = 0;

	pixelShaderBuffer->Release();
	pixelShaderBuffer = 0;

	// Setup the description of the dynamic matrix constant buffer that is in the vertex shader.
	matrixBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	matrixBufferDesc.ByteWidth = sizeof(MatrixBufferType);
	matrixBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	matrixBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	matrixBufferDesc.MiscFlags = 0;
	matrixBufferDesc.StructureByteStride = 0;

	// Create the constant buffer pointer so we can access the vertex shader constant buffer from within this class.
	result = device->CreateBuffer(&matrixBufferDesc, NULL, &d3d->m_matrixBuffer);
	if (FAILED(result))
	{
		return false;
	}

	// Create a texture sampler state description.
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	samplerDesc.BorderColor[0] = 0;
	samplerDesc.BorderColor[1] = 0;
	samplerDesc.BorderColor[2] = 0;
	samplerDesc.BorderColor[3] = 0;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

	// Create the texture sampler state.
	result = device->CreateSamplerState(&samplerDesc, &d3d->m_sampleState);
	if (FAILED(result))
	{
		return false;
	}

	return true;
}

static bool InitializeBuffers(ID3D11Device* device)
{
	struct d3d11struct *d3d = &d3d11data[0];

	VertexType* vertices;
	unsigned long* indices;
	D3D11_BUFFER_DESC vertexBufferDesc, indexBufferDesc;
	D3D11_SUBRESOURCE_DATA vertexData, indexData;
	HRESULT result;
	int i;


	// Set the number of vertices in the vertex array.
	d3d->m_vertexCount = 6;

	// Set the number of indices in the index array.
	d3d->m_indexCount = d3d->m_vertexCount;

	// Create the vertex array.
	vertices = new VertexType[d3d->m_vertexCount];
	if (!vertices)
	{
		return false;
	}

	// Create the index array.
	indices = new unsigned long[d3d->m_indexCount];
	if (!indices)
	{
		return false;
	}

	// Initialize vertex array to zeros at first.
	memset(vertices, 0, (sizeof(VertexType) * d3d->m_vertexCount));

	// Load the index array with data.
	for (i = 0; i < d3d->m_indexCount; i++)
	{
		indices[i] = i;
	}

	// Set up the description of the static vertex buffer.
	vertexBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	vertexBufferDesc.ByteWidth = sizeof(VertexType) * d3d->m_vertexCount;
	vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vertexBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	vertexBufferDesc.MiscFlags = 0;
	vertexBufferDesc.StructureByteStride = 0;

	// Give the subresource structure a pointer to the vertex data.
	vertexData.pSysMem = vertices;
	vertexData.SysMemPitch = 0;
	vertexData.SysMemSlicePitch = 0;

	// Now create the vertex buffer.
	result = device->CreateBuffer(&vertexBufferDesc, &vertexData, &d3d->m_vertexBuffer);
	if (FAILED(result))
	{
		return false;
	}

	// Set up the description of the static index buffer.
	indexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	indexBufferDesc.ByteWidth = sizeof(unsigned long) * d3d->m_indexCount;
	indexBufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
	indexBufferDesc.CPUAccessFlags = 0;
	indexBufferDesc.MiscFlags = 0;
	indexBufferDesc.StructureByteStride = 0;

	// Give the subresource structure a pointer to the index data.
	indexData.pSysMem = indices;
	indexData.SysMemPitch = 0;
	indexData.SysMemSlicePitch = 0;

	// Create the index buffer.
	result = device->CreateBuffer(&indexBufferDesc, &indexData, &d3d->m_indexBuffer);
	if (FAILED(result))
	{
		return false;
	}

	// Release the arrays now that the vertex and index buffers have been created and loaded.
	delete[] vertices;
	vertices = 0;

	delete[] indices;
	indices = 0;

	return true;
}


static bool UpdateBuffers(ID3D11DeviceContext* deviceContext, int positionX, int positionY)
{
	struct d3d11struct *d3d = &d3d11data[0];

	float left, right, top, bottom;
	VertexType* vertices;
	D3D11_MAPPED_SUBRESOURCE mappedResource;
	VertexType* verticesPtr;
	HRESULT result;

	positionX = (d3d->m_screenWidth - d3d->m_bitmapWidth) / 2;
	positionY = (d3d->m_screenHeight - d3d->m_bitmapHeight) / 2;


	// Calculate the screen coordinates of the left side of the bitmap.
	left = (float)((d3d->m_screenWidth / 2) * -1) + (float)positionX;

	// Calculate the screen coordinates of the right side of the bitmap.
	right = left + (float)d3d->m_bitmapWidth;

	// Calculate the screen coordinates of the top of the bitmap.
	top = (float)(d3d->m_screenHeight / 2) - (float)positionY;

	// Calculate the screen coordinates of the bottom of the bitmap.
	bottom = top - (float)d3d->m_bitmapHeight;

	// Create the vertex array.
	vertices = new VertexType[d3d->m_vertexCount];
	if (!vertices)
	{
		return false;
	}

	// Load the vertex array with data.
	// First triangle.
	vertices[0].position = D3DXVECTOR3(left, top, 0.0f);  // Top left.
	vertices[0].texture = D3DXVECTOR2(0.0f, 0.0f);

	vertices[1].position = D3DXVECTOR3(right, bottom, 0.0f);  // Bottom right.
	vertices[1].texture = D3DXVECTOR2(1.0f, 1.0f);

	vertices[2].position = D3DXVECTOR3(left, bottom, 0.0f);  // Bottom left.
	vertices[2].texture = D3DXVECTOR2(0.0f, 1.0f);

	// Second triangle.
	vertices[3].position = D3DXVECTOR3(left, top, 0.0f);  // Top left.
	vertices[3].texture = D3DXVECTOR2(0.0f, 0.0f);

	vertices[4].position = D3DXVECTOR3(right, top, 0.0f);  // Top right.
	vertices[4].texture = D3DXVECTOR2(1.0f, 0.0f);

	vertices[5].position = D3DXVECTOR3(right, bottom, 0.0f);  // Bottom right.
	vertices[5].texture = D3DXVECTOR2(1.0f, 1.0f);

	// Lock the vertex buffer so it can be written to.
	result = deviceContext->Map(d3d->m_vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
	if (FAILED(result))
	{
		return false;
	}

	// Get a pointer to the data in the vertex buffer.
	verticesPtr = (VertexType*)mappedResource.pData;

	// Copy the data into the vertex buffer.
	memcpy(verticesPtr, (void*)vertices, (sizeof(VertexType) * d3d->m_vertexCount));

	// Unlock the vertex buffer.
	deviceContext->Unmap(d3d->m_vertexBuffer, 0);

	// Release the vertex array as it is no longer needed.
	delete[] vertices;
	vertices = 0;

	return true;
}

static const bool xxD3D11_init(HWND ahwnd, int w_w, int w_h, int depth, int *freq, int mmult)
{
	struct d3d11struct *d3d = &d3d11data[0];
	struct apmode *apm = picasso_on ? &currprefs.gfx_apmode[APMODE_RTG] : &currprefs.gfx_apmode[APMODE_NATIVE];

	HRESULT result;
	ComPtr<IDXGIFactory2> factory2;
	ComPtr<IDXGIFactory4> factory4;
	ComPtr<IDXGIFactory5> factory5;
	IDXGIAdapter1* adapter;
	IDXGIOutput* adapterOutput;
	unsigned int numModes, i;
	DXGI_MODE_DESC1* displayModeList;
	DXGI_ADAPTER_DESC adapterDesc;
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC  fsSwapChainDesc;
	ID3D11Texture2D* backBufferPtr;
	D3D11_RASTERIZER_DESC rasterDesc;
	D3D11_VIEWPORT viewport;
	float fieldOfView, screenAspect;

	write_log(_T("D3D11 init start\n"));

	if (!hd3d11)
		hd3d11 = LoadLibrary(_T("D3D11.dll"));
	if (!hdxgi)
		hdxgi = LoadLibrary(_T("Dxgi.dll"));
	if (!hd3dcompiler)
		hd3dcompiler = LoadLibrary(_T("D3DCompiler_47.dll"));

	pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(GetModuleHandle(_T("D3D11.dll")), "D3D11CreateDevice");
	pCreateDXGIFactory1 = (CREATEDXGIFACTORY1)GetProcAddress(GetModuleHandle(_T("Dxgi.dll")), "CreateDXGIFactory1");
	pD3DCompileFromFile = (D3DCOMPILEFROMFILE)GetProcAddress(GetModuleHandle(_T("D3DCompiler_47.dll")), "D3DCompileFromFile");

	d3d->m_bitmapWidth = w_w;
	d3d->m_bitmapHeight = w_h;
	d3d->m_screenWidth = w_w;
	d3d->m_screenHeight = w_h;
	d3d->format = DXGI_FORMAT_B8G8R8A8_UNORM;

	struct MultiDisplay *md = getdisplay(&currprefs);

	// Create a DirectX graphics interface factory.
	result = pCreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory4);
	if (FAILED(result))
	{
		write_log(_T("D3D11 CreateDXGIFactory4 %08x\n"), result);
		result = pCreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&factory2);
		if (FAILED(result)) {
			write_log(_T("D3D11 CreateDXGIFactory2 %08x\n"), result);
			return false;
		}
		factory2 = factory4;
	} else {
		BOOL allowTearing = FALSE;
		result = factory4.As(&factory5);
		factory2 = factory5;
		if (!d3d->m_tearingSupport) {
			result = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
			d3d->m_tearingSupport = SUCCEEDED(result) && allowTearing;
			write_log(_T("CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING) = %08x %d"), result, allowTearing);
		}
	}

	// Use the factory to create an adapter for the primary graphics interface (video card).
	result = factory2->EnumAdapters1(0, &adapter);
	if (FAILED(result))
	{
		return false;
	}


	// Enumerate the primary adapter output (monitor).
	result = adapter->EnumOutputs(0, &adapterOutput);
	if (FAILED(result))
	{
		return false;
	}

	ComPtr<IDXGIOutput1> adapterOutput1;
	result = adapterOutput->QueryInterface(__uuidof(IDXGIOutput1), &adapterOutput1);
	if (FAILED(result)) {
		write_log(_T("QueryInterface IDXGIOutput1 %08x\n"), result);
	}


	// Get the number of modes that fit the display format for the adapter output (monitor).
	result = adapterOutput1->GetDisplayModeList1(d3d->format, DXGI_ENUM_MODES_INTERLACED, &numModes, NULL);
	if (FAILED(result))
	{
		return false;
	}

	// Create a list to hold all the possible display modes for this monitor/video card combination.
	displayModeList = new DXGI_MODE_DESC1[numModes];
	if (!displayModeList)
	{
		return false;
	}

	// Now fill the display mode list structures.
	result = adapterOutput1->GetDisplayModeList1(d3d->format, DXGI_ENUM_MODES_INTERLACED, &numModes, displayModeList);
	if (FAILED(result))
	{
		return false;
	}

	ZeroMemory(&fsSwapChainDesc, sizeof(fsSwapChainDesc));

	// Now go through all the display modes and find the one that matches the screen width and height.
	// When a match is found store the numerator and denominator of the refresh rate for that monitor.
	for (i = 0; i < numModes; i++)
	{
		DXGI_MODE_DESC1 *m = &displayModeList[i];
		if (m->Format != d3d->format)
			continue;
		if (m->Width == md->rect.right - md->rect.left && m->Height == md->rect.bottom - md->rect.top) {
			fsSwapChainDesc.RefreshRate.Denominator = m->RefreshRate.Denominator;
			fsSwapChainDesc.RefreshRate.Numerator = m->RefreshRate.Numerator;
			fsSwapChainDesc.ScanlineOrdering = m->ScanlineOrdering;
			fsSwapChainDesc.Scaling = m->Scaling;
			if (apm->gfx_interlaced && !(m->ScanlineOrdering & DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST))
				continue;
		}
	}

	// Get the adapter (video card) description.
	result = adapter->GetDesc(&adapterDesc);
	if (FAILED(result))
	{
		return false;
	}

	// Release the display mode list.
	delete[] displayModeList;
	displayModeList = 0;

	// Release the adapter output.
	adapterOutput->Release();
	adapterOutput = 0;

	// Release the adapter.
	adapter->Release();
	adapter = 0;

	static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	result = pD3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &d3d->m_device, NULL, &d3d->m_deviceContext);
	if (FAILED(result)) {
		write_log(_T("D3D11CreateDevice failed %08x\n"), result);
		return false;
	}

	ComPtr<IDXGIDevice1> dxgiDevice;
	result = d3d->m_device->QueryInterface(__uuidof(IDXGIDevice1), &dxgiDevice);
	if (FAILED(result)) {
		write_log(_T("QueryInterface IDXGIDevice1 %08x\n"), result);
	}
	result = dxgiDevice->SetMaximumFrameLatency(1);

	// Initialize the swap chain description.
	ZeroMemory(&swapChainDesc, sizeof(swapChainDesc));

	// Set the width and height of the back buffer.
	swapChainDesc.Width = w_w;
	swapChainDesc.Height = w_h;

	// Set regular 32-bit surface for the back buffer.
	swapChainDesc.Format = d3d->format;

	// Turn multisampling off.
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.SampleDesc.Quality = 0;

	// Set the usage of the back buffer.
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

	swapChainDesc.BufferCount = 2;

	swapChainDesc.Scaling = DXGI_SCALING_NONE;

	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

	// It is recommended to always use the tearing flag when it is supported.
	swapChainDesc.Flags = d3d->m_tearingSupport ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	fsSwapChainDesc.Windowed = isfullscreen() <= 0;

	// Create the swap chain, Direct3D device, and Direct3D device context.
	result = factory2->CreateSwapChainForHwnd(d3d->m_device, ahwnd, &swapChainDesc, isfullscreen() > 0 ? &fsSwapChainDesc : NULL, NULL, &d3d->m_swapChain);

	// Set the feature level to DirectX 11.
	// featureLevel = D3D_FEATURE_LEVEL_11_0;
	// result = pD3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &featureLevel, 1, D3D11_SDK_VERSION, &swapChainDesc, &d3d->m_swapChain,
	// &d3d->m_device, NULL, &d3d->m_deviceContext);
	if (FAILED(result))
	{
		return false;
	}

	// Get the pointer to the back buffer.
	result = d3d->m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&backBufferPtr);
	if (FAILED(result))
	{
		return false;
	}

	// Create the render target view with the back buffer pointer.
	result = d3d->m_device->CreateRenderTargetView(backBufferPtr, NULL, &d3d->m_renderTargetView);
	if (FAILED(result))
	{
		return false;
	}

	// Release pointer to the back buffer as we no longer need it.
	backBufferPtr->Release();
	backBufferPtr = 0;


	// Setup the raster description which will determine how and what polygons will be drawn.
	rasterDesc.AntialiasedLineEnable = false;
	rasterDesc.CullMode = D3D11_CULL_BACK;
	rasterDesc.DepthBias = 0;
	rasterDesc.DepthBiasClamp = 0.0f;
	rasterDesc.DepthClipEnable = false;
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.FrontCounterClockwise = false;
	rasterDesc.MultisampleEnable = false;
	rasterDesc.ScissorEnable = false;
	rasterDesc.SlopeScaledDepthBias = 0.0f;

	// Create the rasterizer state from the description we just filled out.
	result = d3d->m_device->CreateRasterizerState(&rasterDesc, &d3d->m_rasterState);
	if (FAILED(result))
	{
		return false;
	}

	// Now set the rasterizer state.
	d3d->m_deviceContext->RSSetState(d3d->m_rasterState);

	// Setup the viewport for rendering.
	viewport.Width = (float)w_w;
	viewport.Height = (float)w_h;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;

	// Create the viewport.
	d3d->m_deviceContext->RSSetViewports(1, &viewport);

	// Setup the projection matrix.
	fieldOfView = (float)D3DX_PI / 4.0f;
	screenAspect = (float)w_w / (float)w_h;

	// Create the projection matrix for 3D rendering.
	D3DXMatrixPerspectiveFovLH(&d3d->m_projectionMatrix, fieldOfView, screenAspect, SCREEN_NEAR, SCREEN_DEPTH);

	// Initialize the world matrix to the identity matrix.
	D3DXMatrixIdentity(&d3d->m_worldMatrix);

	// Create an orthographic projection matrix for 2D rendering.
	D3DXMatrixOrthoLH(&d3d->m_orthoMatrix, (float)w_w, (float)w_h, SCREEN_NEAR, SCREEN_DEPTH);

	d3d->m_positionX = 0.0f;
	d3d->m_positionY = 0.0f;
	d3d->m_positionZ = -10.0f;

	d3d->m_rotationX = 0.0f;
	d3d->m_rotationY = 0.0f;
	d3d->m_rotationZ = 0.0f;

	if (!TextureShaderClass_InitializeShader(d3d->m_device, ahwnd))
		return false;
	if (!InitializeBuffers(d3d->m_device))
		return false;
	if (!UpdateBuffers(d3d->m_deviceContext, 0, 0))
		return false;

	write_log(_T("D3D11 init end\n"));
	return true;
}

static void xD3D11_free(bool immediate)
{
	struct d3d11struct *d3d = &d3d11data[0];

	write_log(_T("D3D11 free start\n"));

	// Before shutting down set to windowed mode or when you release the swap chain it will throw an exception.
	if (d3d->m_swapChain)
	{
		d3d->m_swapChain->SetFullscreenState(false, NULL);
	}
	if (d3d->m_rasterState)
	{
		d3d->m_rasterState->Release();
		d3d->m_rasterState = 0;
	}

	if (d3d->m_renderTargetView)
	{
		d3d->m_renderTargetView->Release();
		d3d->m_renderTargetView = 0;
	}

	if (d3d->m_swapChain)
	{
		d3d->m_swapChain->Release();
		d3d->m_swapChain = 0;
	}
	if (d3d->m_layout) {
		d3d->m_layout->Release();
		d3d->m_layout = NULL;
	}
	if (d3d->m_vertexShader) {
		d3d->m_vertexShader->Release();
		d3d->m_vertexShader = NULL;
	}
	if (d3d->m_pixelShader) {
		d3d->m_pixelShader->Release();
		d3d->m_pixelShader = NULL;
	}

	if (d3d->m_sampleState) {
		d3d->m_sampleState->Release();
		d3d->m_sampleState = NULL;
	}
	if (d3d->m_indexBuffer) {
		d3d->m_indexBuffer->Release();
		d3d->m_indexBuffer = 0;
	}
	if (d3d->m_vertexBuffer) {
		d3d->m_vertexBuffer->Release();
		d3d->m_vertexBuffer = 0;
	}
	if (d3d->m_matrixBuffer) {
		d3d->m_matrixBuffer->Release();
		d3d->m_matrixBuffer = 0;
	}

	FreeTexture();

	if (d3d->m_deviceContext)
	{
		d3d->m_deviceContext->ClearState();
		d3d->m_deviceContext->Flush();
		d3d->m_deviceContext->Release();
		d3d->m_deviceContext = 0;
	}

	if (d3d->m_device)
	{
		d3d->m_device->Release();
		d3d->m_device = 0;
	}

	write_log(_T("D3D11 free end\n"));
}


static const TCHAR *xD3D11_init(HWND ahwnd, int w_w, int w_h, int depth, int *freq, int mmult)
{
	if (xxD3D11_init(ahwnd, w_w, w_h, depth, freq, mmult))
		return NULL;
	xD3D11_free(true);
	return _T("D3D11 ERROR!");
}

static bool TextureShaderClass_SetShaderParameters(ID3D11DeviceContext* deviceContext, D3DXMATRIX worldMatrix, D3DXMATRIX viewMatrix,
	D3DXMATRIX projectionMatrix, ID3D11ShaderResourceView* texture)
{
	struct d3d11struct *d3d = &d3d11data[0];

	HRESULT result;
	D3D11_MAPPED_SUBRESOURCE mappedResource;
	MatrixBufferType* dataPtr;
	unsigned int bufferNumber;


	// Transpose the matrices to prepare them for the shader.
	D3DXMatrixTranspose(&worldMatrix, &worldMatrix);
	D3DXMatrixTranspose(&viewMatrix, &viewMatrix);
	D3DXMatrixTranspose(&projectionMatrix, &projectionMatrix);

	// Lock the constant buffer so it can be written to.
	result = deviceContext->Map(d3d->m_matrixBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
	if (FAILED(result))
	{
		return false;
	}

	// Get a pointer to the data in the constant buffer.
	dataPtr = (MatrixBufferType*)mappedResource.pData;

	// Copy the matrices into the constant buffer.
	dataPtr->world = worldMatrix;
	dataPtr->view = viewMatrix;
	dataPtr->projection = projectionMatrix;

	// Unlock the constant buffer.
	deviceContext->Unmap(d3d->m_matrixBuffer, 0);

	// Set the position of the constant buffer in the vertex shader.
	bufferNumber = 0;

	// Now set the constant buffer in the vertex shader with the updated values.
	deviceContext->VSSetConstantBuffers(bufferNumber, 1, &d3d->m_matrixBuffer);

	// Set shader texture resource in the pixel shader.
	deviceContext->PSSetShaderResources(0, 1, &texture);

	return true;
}

static void RenderBuffers(ID3D11DeviceContext* deviceContext)
{
	struct d3d11struct *d3d = &d3d11data[0];

	unsigned int stride;
	unsigned int offset;


	// Set vertex buffer stride and offset.
	stride = sizeof(VertexType);
	offset = 0;

	// Set the vertex buffer to active in the input assembler so it can be rendered.
	deviceContext->IASetVertexBuffers(0, 1, &d3d->m_vertexBuffer, &stride, &offset);

	// Set the index buffer to active in the input assembler so it can be rendered.
	deviceContext->IASetIndexBuffer(d3d->m_indexBuffer, DXGI_FORMAT_R32_UINT, 0);

	// Set the type of primitive that should be rendered from this vertex buffer, in this case triangles.
	deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

static void BeginScene(float red, float green, float blue, float alpha)
{
	struct d3d11struct *d3d = &d3d11data[0];

	float color[4];


	// Setup the color to clear the buffer to.
	color[0] = red;
	color[1] = green;
	color[2] = blue;
	color[3] = alpha;

	// Bind the render target view and depth stencil buffer to the output render pipeline.
	d3d->m_deviceContext->OMSetRenderTargets(1, &d3d->m_renderTargetView, NULL);

	// Clear the back buffer.
	d3d->m_deviceContext->ClearRenderTargetView(d3d->m_renderTargetView, color);

	return;
}

static void EndScene(void)
{
	struct d3d11struct *d3d = &d3d11data[0];

	HRESULT hr;

	// Present the back buffer to the screen since rendering is complete.
	struct apmode *apm = picasso_on ? &currprefs.gfx_apmode[APMODE_RTG] : &currprefs.gfx_apmode[APMODE_NATIVE];
	if (isfullscreen() > 0) {
		hr = d3d->m_swapChain->Present(apm->gfx_vflip == 0 ? 0 : 1, 0);
	} else {
		UINT presentFlags = d3d->m_tearingSupport ? DXGI_PRESENT_ALLOW_TEARING : 0;
		// Present as fast as possible.
		hr = d3d->m_swapChain->Present(0, 0);
	}
	if (FAILED(hr))
		write_log(_T("D3D11 Present %08x\n"), hr);

	return;
}

static void TextureShaderClass_RenderShader(ID3D11DeviceContext* deviceContext, int indexCount)
{
	struct d3d11struct *d3d = &d3d11data[0];

	// Set the vertex input layout.
	deviceContext->IASetInputLayout(d3d->m_layout);

	// Set the vertex and pixel shaders that will be used to render this triangle.
	deviceContext->VSSetShader(d3d->m_vertexShader, NULL, 0);
	deviceContext->PSSetShader(d3d->m_pixelShader, NULL, 0);

	// Set the sampler state in the pixel shader.
	deviceContext->PSSetSamplers(0, 1, &d3d->m_sampleState);

	// Render the triangle.
	deviceContext->DrawIndexed(indexCount, 0, 0);

	return;
}

static bool TextureShaderClass_Render(ID3D11DeviceContext* deviceContext, int indexCount, D3DXMATRIX worldMatrix, D3DXMATRIX viewMatrix,
	D3DXMATRIX projectionMatrix, ID3D11ShaderResourceView* texture)
{
	struct d3d11struct *d3d = &d3d11data[0];

	bool result;


	// Set the shader parameters that it will use for rendering.
	result = TextureShaderClass_SetShaderParameters(deviceContext, worldMatrix, viewMatrix, projectionMatrix, texture);
	if (!result)
	{
		return false;
	}

	// Now render the prepared buffers with the shader.
	TextureShaderClass_RenderShader(deviceContext, indexCount);

	return true;
}

static void CameraClass_Render()
{
	struct d3d11struct *d3d = &d3d11data[0];

	D3DXVECTOR3 up, position, lookAt;
	float yaw, pitch, roll;
	D3DXMATRIX rotationMatrix;


	// Setup the vector that points upwards.
	up.x = 0.0f;
	up.y = 1.0f;
	up.z = 0.0f;

	// Setup the position of the camera in the world.
	position.x = d3d->m_positionX;
	position.y = d3d->m_positionY;
	position.z = d3d->m_positionZ;

	// Setup where the camera is looking by default.
	lookAt.x = 0.0f;
	lookAt.y = 0.0f;
	lookAt.z = 1.0f;

	// Set the yaw (Y axis), pitch (X axis), and roll (Z axis) rotations in radians.
	pitch = d3d->m_rotationX * 0.0174532925f;
	yaw = d3d->m_rotationY * 0.0174532925f;
	roll = d3d->m_rotationZ * 0.0174532925f;

	// Create the rotation matrix from the yaw, pitch, and roll values.
	D3DXMatrixRotationYawPitchRoll(&rotationMatrix, yaw, pitch, roll);

	// Transform the lookAt and up vector by the rotation matrix so the view is correctly rotated at the origin.
	D3DXVec3TransformCoord(&lookAt, &lookAt, &rotationMatrix);
	D3DXVec3TransformCoord(&up, &up, &rotationMatrix);

	// Translate the rotated camera position to the location of the viewer.
	lookAt = position + lookAt;

	// Finally create the view matrix from the three updated vectors.
	D3DXMatrixLookAtLH(&d3d->m_viewMatrix, &position, &lookAt, &up);

	return;
}

static bool GraphicsClass_Render(float rotation)
{
	struct d3d11struct *d3d = &d3d11data[0];

	bool result;


	// Clear the buffers to begin the scene.
	BeginScene(0.0f, 0.0f, 0.0f, 1.0f);

	// Generate the view matrix based on the camera's position.
	CameraClass_Render();

	// Put the bitmap vertex and index buffers on the graphics pipeline to prepare them for drawing.
	RenderBuffers(d3d->m_deviceContext);

	// Render the bitmap with the texture shader.
	result = TextureShaderClass_Render(d3d->m_deviceContext, d3d->m_indexCount, d3d->m_worldMatrix, d3d->m_viewMatrix, d3d->m_orthoMatrix, d3d->texture);
	if (!result)
	{
		return false;
	}

	return true;
}

static bool xD3D11_renderframe(bool immediate)
{
	struct d3d11struct *d3d = &d3d11data[0];

	GraphicsClass_Render(0);

	return true;
}


static void xD3D11_showframe(void)
{
	// Present the rendered scene to the screen.
	EndScene();
}

static void xD3D11_clear(void)
{
	BeginScene(0, 0, 0, 0);
}

static void xD3D11_refresh(void)
{
}

static bool xD3D11_alloctexture(int w, int h)
{
	struct d3d11struct *d3d = &d3d11data[0];

	d3d->m_bitmapWidth = w;
	d3d->m_bitmapHeight = h;
	UpdateBuffers(d3d->m_deviceContext, 0, 0);
	return CreateTexture();
}

static uae_u8 *xD3D11_locktexture(int *pitch, int *height, bool fullupdate)
{
	struct d3d11struct *d3d = &d3d11data[0];

	D3D11_MAPPED_SUBRESOURCE map;
	HRESULT hr = d3d->m_deviceContext->Map(d3d->texture2d, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
	if (FAILED(hr)) {
		write_log(_T("D3D11 Map() %08x\n"), hr);
		return false;
	}
	*pitch = map.RowPitch;
	if (height)
		*height = d3d->m_bitmapHeight;
	d3d->texturelocked++;
	return (uae_u8*)map.pData;
}

static void xD3D11_unlocktexture(void)
{
	struct d3d11struct *d3d = &d3d11data[0];

	if (!d3d->texturelocked)
		return;
	d3d->texturelocked--;

	d3d->m_deviceContext->Unmap(d3d->texture2d, 0);
}

static void xD3D11_flushtexture(int miny, int maxy)
{
	struct d3d11struct *d3d = &d3d11data[0];
}

static void xD3D11_restore(void)
{
	
}

static void xD3D11_guimode(bool guion)
{

}

static void xD3D11_vblank_reset(double freq)
{

}

static int xD3D11_canshaders(void)
{
	return 0;
}
static int xD3D11_goodenough(void)
{
	return 0;
}

void d3d11_select(void)
{
	D3D_free = xD3D11_free;
	D3D_init = xD3D11_init;
	D3D_renderframe = xD3D11_renderframe;
	D3D_alloctexture = xD3D11_alloctexture;
	D3D_refresh = xD3D11_refresh;
	D3D_restore = xD3D11_restore;

	D3D_locktexture = xD3D11_locktexture;
	D3D_unlocktexture = xD3D11_unlocktexture;
	D3D_flushtexture = xD3D11_flushtexture;

	D3D_showframe = xD3D11_showframe;
	D3D_showframe_special = NULL;
	D3D_guimode = xD3D11_guimode;
	D3D_getDC = NULL;
	D3D_isenabled = NULL;
	D3D_clear = xD3D11_clear;
	D3D_canshaders = xD3D11_canshaders;
	D3D_goodenough = xD3D11_goodenough;
	D3D_setcursor = NULL;
	D3D_getvblankpos = NULL;
	D3D_getrefreshrate = NULL;
	D3D_vblank_reset = xD3D11_vblank_reset;
}

void d3d_select(struct uae_prefs *p)
{
	if (p->gfx_api == 2)
		d3d11_select();
	else
		d3d9_select();
}
