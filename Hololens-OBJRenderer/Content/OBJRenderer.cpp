#include "pch.h"
#include "OBJRenderer.h"
#include "Common\DirectXHelper.h"

using namespace Hololens_OBJRenderer;
using namespace Concurrency;
using namespace DirectX;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI::Input::Spatial;

// Loads vertex and pixel shaders from files and instantiates the obj geometry.
OBJRenderer::OBJRenderer(const std::shared_ptr<DX::DeviceResources>& deviceResources, std::string fileName) :
	m_deviceResources(deviceResources)
{
	Platform::String^ localfolder = Windows::Storage::ApplicationData::Current->LocalFolder->Path;	//for local saving for future

	//convert folder name from wchar to ascii
	std::wstring folderNameW(localfolder->Begin());
	std::string folderNameA(folderNameW.begin(), folderNameW.end());
	std::string name = folderNameA + "\\" + fileName;

	std::ifstream in(name);
	parseOBJ(in);
	CreateDeviceDependentResources();
}

// This function uses a SpatialPointerPose to position the world-locked hologram
// two meters in front of the user's heading
void OBJRenderer::PositionHologram(SpatialPointerPose^ pointerPose)
{
	if (pointerPose != nullptr)
	{
		// Get the gaze direction relative to the given coordinate system
		const float3 headPosition = pointerPose->Head->Position;
		const float3 headDirection = pointerPose->Head->ForwardDirection;

		// The hologram is positioned two meters along the user's gaze direction.
		constexpr float distanceFromUser = 2.0f; // meters
		const float3 gazeAtTwoMeters = headPosition + (distanceFromUser * headDirection);
			
		// This will be used as the translation component of the hologram's
		// model transform.
		SetPosition(gazeAtTwoMeters);
	}
}

// Called once per fram. Rotates the obj, and calculates and sets the model matrix
// relative to the position transform indicated by hologramPositionTransform.
void OBJRenderer::Update(const DX::StepTimer& timer)
{
	// Rotate the obj.
	// Conver degrees to radians, then convert seconds to rotation angle.
	const float radiansPerSecond = XMConvertToRadians(m_degreesPerSecond);
	const double totalRotation = timer.GetTotalSeconds() * radiansPerSecond;
	const float radians = static_cast<float>(fmod(totalRotation, XM_2PI));
	const XMMATRIX modelRotation = XMMatrixRotationY(-radians);

	// Position the obj
	const XMMATRIX modelTranslation = XMMatrixTranslationFromVector(XMLoadFloat3(&m_position));

	// Multiply to get the transform matrix.
	// Note that this transform does not enforce a particular coordinate system. The calling
	// class is responsible for rendering this content in a consistent manner.
	const XMMATRIX modelTransform = XMMatrixMultiply(modelRotation, modelTranslation);

	// The view and projection matrices are provided by the system; they are associated
	// with holographic camera, and updated on a per-camera basis.
	// Here, we provide the model transform for the smaple hologram. The model transform
	// matrix is transposed to prepare it for the shader.
	XMStoreFloat4x4(&m_modelConstantBufferData.model, XMMatrixTranspose(modelTransform));

	// Loading is asynchronous. Resources must be created before they can be updated.
	if (!m_loadingComplete) 
	{
		return;
	}

	// Use the D3D device context to update Direct3D device-based resources.
	const auto context = m_deviceResources->GetD3DDeviceContext();

	// Updated the model transform buffer for the hologram
	context->UpdateSubresource(
		m_modelConstantBuffer.Get(),
		0,
		nullptr,
		&m_modelConstantBufferData,
		0,
		0
		);
}

// Renders one frame using the vertex and pixel shaders.
// On devices that do not support the D3D11_FEATURE_D3D11_OPTIONS3::
// VPAndRTArrayIndexFromAnyShaderFeedingRasterizer optional feature,
// a pass-through geometry shader is also used to set the render
// target array index.
void OBJRenderer::Render() 
{
	// Loading is asynchronous. Resources must be created before drawing can occur.
	if (!m_loadingComplete) 
	{
		return;
	}

	const auto context = m_deviceResources->GetD3DDeviceContext();

	// Each vertex is one instance of the VertexPositionNormalColor struct.
	const UINT stride = sizeof(VertexPositionColor);
	const UINT offset = 0;
	context->IASetVertexBuffers(
		0,
		1,
		m_vertexBuffer.GetAddressOf(),
		&stride,
		&offset
		);
	context->IASetIndexBuffer(
		m_indexBuffer.Get(),
		DXGI_FORMAT_R32_UINT, // Each index is one 32-bit unsigned integer (short).
		0
		);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetInputLayout(m_inputLayout.Get());

	// Attach the vertex shader.
	context->VSSetShader(
		m_vertexShader.Get(),
		nullptr,
		0
		);
	// Apply the model constant buffer to the vertex shader.
	context->VSSetConstantBuffers(
		0,
		1,
		m_modelConstantBuffer.GetAddressOf()
		);

	if (!m_usingVprtShaders)
	{
		// On devices that do not support the D3D11_FEATURE_D3D11_OPTIONS3::
		// VPAndRTArrayIndexFromAnyShaderFeedingRasterizer optional feature,
		// a pass-through geometry shader is used ton set the render target
		// array index.
		context->GSSetShader(
			m_geometryShader.Get(),
			nullptr,
			0
			);
	}

	// Attach the pixel shader.
	context->PSSetShader(
		m_pixelShader.Get(),
		nullptr,
		0
		);

	// Draw the objects.
	context->DrawIndexedInstanced(
		m_indexCount,	// Index count per instance
		2,				// Instance count.
		0,				// Start index location
		0,				// Base vertex location
		0				// Start instance location.
		);
}

void OBJRenderer::CreateDeviceDependentResources()
{
	m_usingVprtShaders = m_deviceResources->GetDeviceSupportsVprt();

	// On devices that do support the D3D11_FEATURE_D3D11_OPTIONS3::
	// VPAndRTArrayIndexFromAnyShaderFeedingRasterizer optional feature
	// we can avoid using a pass-throguh geometry shader to set the render
	// target array index, thus avoiding any overhead that would be
	// incurred by setting the geometry shader stage.
	std::wstring vertexShaderFileName = m_usingVprtShaders ? L"ms-appx:///VprtVertexShader.cso" : L"ms-appx:///VertexShader.cso";

	// Load shaders asynchronously.
	task<std::vector<byte>> loadVSTask = DX::ReadDataAsync(vertexShaderFileName);
	task<std::vector<byte>> loadPSTask = DX::ReadDataAsync(L"ms-appx:///PixelShader.cso");

	task<std::vector<byte>> loadGSTask;
	if (!m_usingVprtShaders)
	{
		// Load the pass-through geometry shader.
		loadGSTask = DX::ReadDataAsync(L"ms-appx:///GeometryShader.cso");
	}

	// After the vertex shade file is loaded, create the shader and input layout.
	task<void> createVSTask = loadVSTask.then([this](const std::vector<byte>& fileData)
	{	
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateVertexShader(
				fileData.data(),
				fileData.size(),
				nullptr,
				&m_vertexShader
				)
			);

		constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 2> vertexDesc = 
		{{
			{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
		} };

		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateInputLayout(
				vertexDesc.data(),
				vertexDesc.size(),
				fileData.data(),
				fileData.size(),
				&m_inputLayout
				)
			);
	});

	// After the pixel shader file is loaded, create the shader and constant buffer.
	task<void> createPSTask = loadPSTask.then([this](const std::vector<byte>& fileData) {
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreatePixelShader(
				fileData.data(),
				fileData.size(),
				nullptr,
				&m_pixelShader
				)
			);

		const CD3D11_BUFFER_DESC constantBufferDesc(sizeof(ModelConstantBuffer), D3D11_BIND_CONSTANT_BUFFER);
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateBuffer(
				&constantBufferDesc,
				nullptr,
				&m_modelConstantBuffer
				)
			);
	});

	task<void> createGSTask;
	if (!m_usingVprtShaders) 
	{
		// After the pass-through geometry shader file is loaded, create the shader
		createGSTask = loadGSTask.then([this](const std::vector<byte>& fileData)
		{
			DX::ThrowIfFailed(
				m_deviceResources->GetD3DDevice()->CreateGeometryShader(
					fileData.data(),
					fileData.size(),
					nullptr,
					&m_geometryShader
					)
				);
		});
	}

	// Once all the shaders are loaded, create the mesh.
	task<void> shaderTaskGroup = m_usingVprtShaders ? (createPSTask && createVSTask) : (createPSTask && createVSTask && createGSTask);
	task<void> createOBJTask = shaderTaskGroup.then([this]()
	{
		// Load mesh vertices. Each vertex has a positiin and a color.
		// Note that the obj size has changed from the default DirectX app
		// template. Windows Holographic is scaled in meteres, so to draw the 
		// obj at a comfortable size we made the cube width 0.2 m (20 cm).
		D3D11_SUBRESOURCE_DATA vertexBufferData = { 0 };
		vertexBufferData.pSysMem = vertices.data();
		vertexBufferData.SysMemPitch = 0;
		vertexBufferData.SysMemSlicePitch = 0;
		const CD3D11_BUFFER_DESC vertexBufferDesc(sizeof(VertexPositionColor) * vertices.size(), D3D11_BIND_VERTEX_BUFFER);
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateBuffer(
				&vertexBufferDesc,
				&vertexBufferData,
				&m_vertexBuffer
				)
			);

		// Load mesh indices. Each trio of indices represents
		// a triangle to be rendered on the screen.
		// For example: 2,1,0 means that the vertices with indexes
		// 2, 1, and 0 from the vertex buffer compose the first traingle of this mesh.
		// Note that the winding order is clockwise by default
		m_indexCount = indices.size();
		D3D11_SUBRESOURCE_DATA indexBufferData = { 0 };
		indexBufferData.pSysMem = indices.data();
		indexBufferData.SysMemPitch = 0;
		indexBufferData.SysMemSlicePitch = 0;
		CD3D11_BUFFER_DESC indexBufferDesc(sizeof(UINT) * indices.size(), D3D11_BIND_INDEX_BUFFER);
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateBuffer(
				&indexBufferDesc,
				&indexBufferData,
				&m_indexBuffer
				)
			);
	});

	// Once the obj is loaded, the object is ready to be rendered
	createOBJTask.then([this]() 
	{
		m_loadingComplete = true;
	});
}

void OBJRenderer::ReleaseDeviceDependentResources() {
	m_loadingComplete = false;
	m_usingVprtShaders = false;
	m_vertexShader.Reset();
	m_inputLayout.Reset();
	m_pixelShader.Reset();
	m_geometryShader.Reset();
	m_modelConstantBuffer.Reset();
	m_vertexBuffer.Reset();
	m_indexBuffer.Reset();
}

// parses the obj file and loads the vertices
void OBJRenderer::parseOBJ(std::ifstream& in)
{
	// Check if the file was successfully opened
	if (!in.is_open()) {
		return;
	}

	std::string vn("vn");
	std::string v("v");
	std::string f("f");

	long long lines = 0;

	while (in) {
		std::string line;

		// get a line:
		if (!getline(in, line)) { break; }

		lines++;
		if (lines % 1000 == 0) { std::cout << lines << " lines parsed.\n";  }

		// continue if there is something wrong with this line or if it is a comment
		if (line.empty() || line.find_first_of("#") != std::string::npos) { continue; }

		// parse the line
		std::stringstream ss(line);
		std::vector<std::string> rec;
		while (ss) 
		{
			std::string data;
			if (!getline(ss, data, ' ')) { break; }
			rec.push_back(data);
		}

		// skip empty records
		if (rec.empty()) 
		{
			continue;
		}

		// check if the line contains a vertex, vertex normal, or face indices
		if (rec[0] == vn)
		{
			// If record has an incorrect number of elements for a vertex normal, skip it
			if (rec.size() != 4) { continue; }
			FLOAT nx = std::stof(rec[1]);
			FLOAT ny = std::stof(rec[2]);
			FLOAT nz = std::stof(rec[3]);

			FLOAT length = sqrt(nx * nx + ny * ny + nz * nz);

			VertexPositionColor v;
			v.color = XMFLOAT3(nx / length, ny / length, nz / length);
			vertices.push_back(v);
		}
		else if (rec[0] == v)
		{
			// If a record has an incorrect number of elements for a vertex, skip it
			if (rec.size() != 4 && rec.size() != 7) { continue; }

			if (rec.size() == 7) 
			{
				FLOAT x = std::stof(rec[1]);
				FLOAT y = std::stof(rec[2]);
				FLOAT z = std::stof(rec[3]);
				FLOAT r = std::stof(rec[4]);
				FLOAT g = std::stof(rec[5]);
				FLOAT b = std::stof(rec[6]);
				vertices.back().pos = XMFLOAT3(x, y, z);
				//vertices.back().color = XMFLOAT3(r, g, b);
			}
			else 
			{
				FLOAT x = std::stof(rec[1]);
				FLOAT y = std::stof(rec[2]);
				FLOAT z = std::stof(rec[3]);
				vertices.back().pos = XMFLOAT3(x, y, z);
			}
		}
		else if (rec[0] == f) 
		{
			// If a record has an incorrect number of elements for a set of face indices, skip it
			if (rec.size() != 4) { continue; }

			// subtract 1 from each face index because they are in the range of 1 to n, when
			// it should be from 0 to n - 1
			UINT v1 = (UINT)std::stoi(rec[1].substr(0, rec[1].find_first_of('/'))) - 1;
			UINT v2 = (UINT)std::stoi(rec[2].substr(0, rec[2].find_first_of('/'))) - 1;
			UINT v3 = (UINT)std::stoi(rec[3].substr(0, rec[3].find_first_of('/'))) - 1;
			indices.push_back(v3);
			indices.push_back(v2);
			indices.push_back(v1);
		}
	}

	in.close();

	// Center and scale down obj to fit in a 0.2m x 0.2m x 0.2m cube
	FLOAT maxX = (std::max_element(vertices.begin(), vertices.end(), [](VertexPositionColor v1, VertexPositionColor v2)->bool {return v1.pos.x < v2.pos.x; }))->pos.x;
	FLOAT maxY = (std::max_element(vertices.begin(), vertices.end(), [](VertexPositionColor v1, VertexPositionColor v2)->bool {return v1.pos.y < v2.pos.y; }))->pos.y;
	FLOAT maxZ = (std::max_element(vertices.begin(), vertices.end(), [](VertexPositionColor v1, VertexPositionColor v2)->bool {return v1.pos.z < v2.pos.z; }))->pos.z;

	FLOAT minX = (std::min_element(vertices.begin(), vertices.end(), [](VertexPositionColor v1, VertexPositionColor v2)->bool {return v1.pos.x < v2.pos.x; }))->pos.x;
	FLOAT minY = (std::min_element(vertices.begin(), vertices.end(), [](VertexPositionColor v1, VertexPositionColor v2)->bool {return v1.pos.y < v2.pos.y; }))->pos.y;
	FLOAT minZ = (std::min_element(vertices.begin(), vertices.end(), [](VertexPositionColor v1, VertexPositionColor v2)->bool {return v1.pos.z < v2.pos.z; }))->pos.z;

	FLOAT delX = fabs(maxX - minX);
	FLOAT delY = fabs(maxY - minY);
	FLOAT delZ = fabs(maxZ - minZ);
	FLOAT avgX = (maxX + minX) / 2.0f;
	FLOAT avgY = (maxY + minY) / 2.0f;
	FLOAT avgZ = (maxZ + minZ) / 2.0f;

	for (uint32 i = 0; i < vertices.size(); i++) {
		vertices[i].pos.x -= avgX;
		vertices[i].pos.x /= 5.0f * delX;
		vertices[i].pos.y -= avgY;
		vertices[i].pos.y /= 5.0f * delY;
		vertices[i].pos.z -= avgZ;
		vertices[i].pos.z /= 5.0f * delZ;
	}
}