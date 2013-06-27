// Copyright (C) 2013 Jérôme Leclercq
// This file is part of the "Nazara Engine - Renderer module"
// For conditions of distribution and use, see copyright notice in Config.hpp

#include <Nazara/Renderer/OpenGL.hpp> // Pour éviter une redéfinition de WIN32_LEAN_AND_MEAN
#include <Nazara/Renderer/Renderer.hpp>
#include <Nazara/Core/Color.hpp>
#include <Nazara/Core/Error.hpp>
#include <Nazara/Core/Log.hpp>
#include <Nazara/Renderer/AbstractShader.hpp>
#include <Nazara/Renderer/Config.hpp>
#include <Nazara/Renderer/Context.hpp>
#include <Nazara/Renderer/DebugDrawer.hpp>
#include <Nazara/Renderer/HardwareBuffer.hpp>
#include <Nazara/Renderer/Material.hpp>
#include <Nazara/Renderer/RenderTarget.hpp>
#include <Nazara/Renderer/Shader.hpp>
#include <Nazara/Renderer/ShaderBuilder.hpp>
#include <Nazara/Renderer/Loaders/Texture.hpp>
#include <Nazara/Utility/AbstractBuffer.hpp>
#include <Nazara/Utility/IndexBuffer.hpp>
#include <Nazara/Utility/Utility.hpp>
#include <Nazara/Utility/VertexBuffer.hpp>
#include <Nazara/Utility/VertexDeclaration.hpp>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <Nazara/Renderer/Debug.hpp>

namespace
{
	enum UpdateFlags
	{
		Update_None = 0,

		Update_Matrices     = 0x01,
		Update_Shader       = 0x02,
		Update_Textures     = 0x04,
		Update_VAO          = 0x08,
	};

	struct MatrixUnit
	{
		NzMatrix4f matrix;
		bool sent;
		bool updated;
		int location;
	};

	struct TextureUnit
	{
		NzTextureSampler sampler;
		const NzTexture* texture = nullptr;
		bool samplerUpdated = false;
		bool textureUpdated = true;
	};

	NzAbstractBuffer* HardwareBufferFunction(NzBuffer* parent, nzBufferType type)
	{
		return new NzHardwareBuffer(parent, type);
	}

	using VAO_Key = std::tuple<const NzContext*, const NzIndexBuffer*, const NzVertexBuffer*, bool>;

	std::map<VAO_Key, unsigned int> s_vaos;
	std::set<unsigned int> s_dirtyTextureUnits;
	std::vector<TextureUnit> s_textureUnits;
	GLuint s_currentVAO = 0;
	NzBuffer* s_instancingBuffer = nullptr;
	NzVertexBuffer* s_fullscreenQuadBuffer = nullptr;
	MatrixUnit s_matrices[nzMatrixType_Max+1];
	NzRenderStates s_states;
	NzVector2ui s_targetSize;
	nzUInt8 s_maxAnisotropyLevel;
	nzUInt32 s_updateFlags;
	const NzIndexBuffer* s_indexBuffer;
	const NzRenderTarget* s_target;
	const NzShader* s_shader;
	const NzVertexBuffer* s_vertexBuffer;
	bool s_capabilities[nzRendererCap_Max+1];
	bool s_instancing;
	bool s_useSamplerObjects;
	bool s_useVertexArrayObjects;
	unsigned int s_maxRenderTarget;
	unsigned int s_maxTextureUnit;
}

void NzRenderer::Clear(unsigned long flags)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	if (flags)
	{
		GLenum mask = 0;

		if (flags & nzRendererClear_Color)
			mask |= GL_COLOR_BUFFER_BIT;

		if (flags & nzRendererClear_Depth)
			mask |= GL_DEPTH_BUFFER_BIT;

		if (flags & nzRendererClear_Stencil)
			mask |= GL_STENCIL_BUFFER_BIT;

		// Les états du rendu sont suceptibles d'influencer glClear
		NzOpenGL::ApplyStates(s_states);

		glClear(mask);
	}
}

void NzRenderer::DrawFullscreenQuad()
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	EnableInstancing(false);
	SetIndexBuffer(nullptr);
	SetVertexBuffer(s_fullscreenQuadBuffer);

	if (!EnsureStateUpdate())
	{
		NazaraError("Failed to update states");
		return;
	}

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void NzRenderer::DrawIndexedPrimitives(nzPrimitiveMode mode, unsigned int firstIndex, unsigned int indexCount)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}

	if (mode > nzPrimitiveMode_Max)
	{
		NazaraError("Primitive mode out of enum");
		return;
	}
	#endif

	#if NAZARA_RENDERER_SAFE
	if (!s_indexBuffer)
	{
		NazaraError("No index buffer");
		return;
	}
	#endif

	EnableInstancing(false);

	if (!EnsureStateUpdate())
	{
		NazaraError("Failed to update states");
		return;
	}

	if (s_indexBuffer->IsSequential())
		glDrawArrays(NzOpenGL::PrimitiveMode[mode], s_indexBuffer->GetStartIndex(), s_indexBuffer->GetIndexCount());
	else
	{
		GLenum type;
		const nzUInt8* ptr = reinterpret_cast<const nzUInt8*>(s_indexBuffer->GetPointer());
		if (s_indexBuffer->HasLargeIndices())
		{
			ptr += firstIndex*sizeof(nzUInt32);
			type = GL_UNSIGNED_INT;
		}
		else
		{
			ptr += firstIndex*sizeof(nzUInt16);
			type = GL_UNSIGNED_SHORT;
		}

		glDrawElements(NzOpenGL::PrimitiveMode[mode], indexCount, type, ptr);
	}

	glBindVertexArray(0);
}

void NzRenderer::DrawIndexedPrimitivesInstanced(unsigned int instanceCount, nzPrimitiveMode mode, unsigned int firstIndex, unsigned int indexCount)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}

	if (mode > nzPrimitiveMode_Max)
	{
		NazaraError("Primitive mode out of enum");
		return;
	}
	#endif

	#if NAZARA_RENDERER_SAFE
	if (!s_capabilities[nzRendererCap_Instancing])
	{
		NazaraError("Instancing not supported");
		return;
	}

	if (!s_indexBuffer)
	{
		NazaraError("No index buffer");
		return;
	}

	if (instanceCount == 0)
	{
		NazaraError("Instance count must be over 0");
		return;
	}

	if (instanceCount > NAZARA_RENDERER_MAX_INSTANCES)
	{
		NazaraError("Instance count is over maximum instance count (" + NzString::Number(instanceCount) + " >= " NazaraStringifyMacro(NAZARA_RENDERER_MAX_INSTANCES) ")" );
		return;
	}
	#endif

	EnableInstancing(true);

	if (!EnsureStateUpdate())
	{
		NazaraError("Failed to update states");
		return;
	}

	if (s_indexBuffer->IsSequential())
		glDrawArraysInstanced(NzOpenGL::PrimitiveMode[mode], s_indexBuffer->GetStartIndex(), s_indexBuffer->GetIndexCount(), instanceCount);
	else
	{
		GLenum type;
		const nzUInt8* ptr = reinterpret_cast<const nzUInt8*>(s_indexBuffer->GetPointer());

		if (s_indexBuffer->HasLargeIndices())
		{
			ptr += firstIndex*sizeof(nzUInt32);
			type = GL_UNSIGNED_INT;
		}
		else
		{
			ptr += firstIndex*sizeof(nzUInt16);
			type = GL_UNSIGNED_SHORT;
		}

		glDrawElementsInstanced(NzOpenGL::PrimitiveMode[mode], indexCount, type, ptr, instanceCount);
	}

	glBindVertexArray(0);
}

void NzRenderer::DrawPrimitives(nzPrimitiveMode mode, unsigned int firstVertex, unsigned int vertexCount)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}

	if (mode > nzPrimitiveMode_Max)
	{
		NazaraError("Primitive mode out of enum");
		return;
	}
	#endif

	EnableInstancing(false);

	if (!EnsureStateUpdate())
	{
		NazaraError("Failed to update states");
		return;
	}

	glDrawArrays(NzOpenGL::PrimitiveMode[mode], firstVertex, vertexCount);
	glBindVertexArray(0);
}

void NzRenderer::DrawPrimitivesInstanced(unsigned int instanceCount, nzPrimitiveMode mode, unsigned int firstVertex, unsigned int vertexCount)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}

	if (mode > nzPrimitiveMode_Max)
	{
		NazaraError("Primitive mode out of enum");
		return;
	}
	#endif

	#if NAZARA_RENDERER_SAFE
	if (!s_capabilities[nzRendererCap_Instancing])
	{
		NazaraError("Instancing not supported");
		return;
	}

	if (instanceCount == 0)
	{
		NazaraError("Instance count must be over 0");
		return;
	}

	if (instanceCount > NAZARA_RENDERER_MAX_INSTANCES)
	{
		NazaraError("Instance count is over maximum instance count (" + NzString::Number(instanceCount) + " >= " NazaraStringifyMacro(NAZARA_RENDERER_MAX_INSTANCES) ")" );
		return;
	}
	#endif

	EnableInstancing(true);

	if (!EnsureStateUpdate())
	{
		NazaraError("Failed to update states");
		return;
	}

	glDrawArraysInstanced(NzOpenGL::PrimitiveMode[mode], firstVertex, vertexCount, instanceCount);
	glBindVertexArray(0);
}

void NzRenderer::Enable(nzRendererParameter parameter, bool enable)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}

	if (parameter > nzRendererParameter_Max)
	{
		NazaraError("Renderer parameter out of enum");
		return;
	}
	#endif

	s_states.parameters[parameter] = enable;
}

void NzRenderer::Flush()
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	glFlush();
}

float NzRenderer::GetLineWidth()
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return 0.f;
	}
	#endif

	return s_states.lineWidth;
}

NzMatrix4f NzRenderer::GetMatrix(nzMatrixType type)
{
	#ifdef NAZARA_DEBUG
	if (type > nzMatrixType_Max)
	{
		NazaraError("Matrix type out of enum");
		return NzMatrix4f();
	}
	#endif

	if (!s_matrices[type].updated)
		UpdateMatrix(type);

	return s_matrices[type].matrix;
}

nzUInt8 NzRenderer::GetMaxAnisotropyLevel()
{
	return s_maxAnisotropyLevel;
}

unsigned int NzRenderer::GetMaxRenderTargets()
{
	return s_maxRenderTarget;
}

unsigned int NzRenderer::GetMaxTextureUnits()
{
	return s_maxTextureUnit;
}

float NzRenderer::GetPointSize()
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return 0.f;
	}
	#endif

	return s_states.pointSize;
}

const NzRenderStates& NzRenderer::GetRenderStates()
{
	return s_states;
}

NzRectui NzRenderer::GetScissorRect()
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return NzRectui();
	}
	#endif

	GLint params[4];
	glGetIntegerv(GL_SCISSOR_BOX, &params[0]);

	return NzRectui(params[0], params[1], params[2], params[3]);
}

const NzShader* NzRenderer::GetShader()
{
	return s_shader;
}

const NzRenderTarget* NzRenderer::GetTarget()
{
	return s_target;
}

NzRectui NzRenderer::GetViewport()
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return NzRectui();
	}
	#endif

	GLint params[4];
	glGetIntegerv(GL_VIEWPORT, &params[0]);

	return NzRectui(params[0], params[1], params[2], params[3]);
}

bool NzRenderer::HasCapability(nzRendererCap capability)
{
	#ifdef NAZARA_DEBUG
	if (capability > nzRendererCap_Max)
	{
		NazaraError("Renderer capability out of enum");
		return false;
	}
	#endif

	return s_capabilities[capability];
}

bool NzRenderer::Initialize()
{
	if (s_moduleReferenceCounter++ != 0)
		return true; // Déjà initialisé

	// Initialisation des dépendances
	if (!NzUtility::Initialize())
	{
		NazaraError("Failed to initialize utility module");
		return false;
	}

	// Initialisation du module
	if (!NzOpenGL::Initialize())
	{
		NazaraError("Failed to initialize OpenGL");
		return false;
	}

	NzContext::EnsureContext();

	NzBuffer::SetBufferFunction(nzBufferStorage_Hardware, HardwareBufferFunction);

	for (unsigned int i = 0; i <= nzMatrixType_Max; ++i)
	{
		MatrixUnit& unit = s_matrices[i];
		unit.location = -1;
		unit.matrix.MakeIdentity();
		unit.sent = false;
		unit.updated = true;
	}

	// Récupération des capacités d'OpenGL
	s_capabilities[nzRendererCap_AnisotropicFilter] = NzOpenGL::IsSupported(nzOpenGLExtension_AnisotropicFilter);
	s_capabilities[nzRendererCap_FP64] = NzOpenGL::IsSupported(nzOpenGLExtension_FP64);
	s_capabilities[nzRendererCap_HardwareBuffer] = true; // Natif depuis OpenGL 1.5
	s_capabilities[nzRendererCap_Instancing] = NzOpenGL::IsSupported(nzOpenGLExtension_DrawInstanced) && NzOpenGL::IsSupported(nzOpenGLExtension_InstancedArray);
	s_capabilities[nzRendererCap_MultipleRenderTargets] = (glBindFragDataLocation != nullptr); // Natif depuis OpenGL 2.0 mais inutile sans glBindFragDataLocation
	s_capabilities[nzRendererCap_OcclusionQuery] = true; // Natif depuis OpenGL 1.5
	s_capabilities[nzRendererCap_PixelBufferObject] = NzOpenGL::IsSupported(nzOpenGLExtension_PixelBufferObject);
	s_capabilities[nzRendererCap_RenderTexture] = NzOpenGL::IsSupported(nzOpenGLExtension_FrameBufferObject);
	s_capabilities[nzRendererCap_Texture3D] = true; // Natif depuis OpenGL 1.2
	s_capabilities[nzRendererCap_TextureCubemap] = true; // Natif depuis OpenGL 1.3
	s_capabilities[nzRendererCap_TextureMulti] = true; // Natif depuis OpenGL 1.3
	s_capabilities[nzRendererCap_TextureNPOT] = true; // Natif depuis OpenGL 2.0

	if (s_capabilities[nzRendererCap_AnisotropicFilter])
	{
		GLfloat maxAnisotropy;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy);

		s_maxAnisotropyLevel = static_cast<nzUInt8>(maxAnisotropy);
	}
	else
		s_maxAnisotropyLevel = 1;

	if (s_capabilities[nzRendererCap_Instancing])
	{
		s_instancingBuffer = new NzBuffer(nzBufferType_Vertex);
		if (!s_instancingBuffer->Create(NAZARA_RENDERER_MAX_INSTANCES, sizeof(InstancingData), nzBufferStorage_Hardware, nzBufferUsage_Dynamic))
		{
			s_capabilities[nzRendererCap_Instancing] = false;

			delete s_instancingBuffer;
			s_instancingBuffer = nullptr;

			NazaraWarning("Failed to create instancing buffer, disabled instancing.");
		}
	}

	if (s_capabilities[nzRendererCap_MultipleRenderTargets])
	{
		GLint maxDrawBuffers;
		glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);

		s_maxRenderTarget = static_cast<unsigned int>(maxDrawBuffers);
	}
	else
		s_maxRenderTarget = 1;

	if (s_capabilities[nzRendererCap_TextureMulti])
	{
		GLint maxTextureUnits;
		glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTextureUnits);

		GLint maxVertexAttribs;
		glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxVertexAttribs);

		// Impossible de binder plus de texcoords que d'attributes (en sachant qu'un certain nombre est déjà pris par les autres attributs)
		s_maxTextureUnit = static_cast<unsigned int>(std::min(maxTextureUnits, maxVertexAttribs-NzOpenGL::AttributeIndex[nzElementUsage_TexCoord]));
	}
	else
		s_maxTextureUnit = 1;

	s_states = NzRenderStates();

	s_indexBuffer = nullptr;
	s_shader = nullptr;
	s_target = nullptr;
	s_textureUnits.resize(s_maxTextureUnit);
	s_useSamplerObjects = NzOpenGL::IsSupported(nzOpenGLExtension_SamplerObjects);
	s_useVertexArrayObjects = NzOpenGL::IsSupported(nzOpenGLExtension_VertexArrayObjects);
	s_vertexBuffer = nullptr;
	s_updateFlags = (Update_Matrices | Update_Shader | Update_VAO);

	NzVertexElement element;
	element.offset = 0;
	element.type = nzElementType_Float2;
	element.usage = nzElementUsage_Position;

	std::unique_ptr<NzVertexDeclaration> declaration(new NzVertexDeclaration);
	if (!declaration->Create(&element, 1))
	{
		NazaraError("Failed to create fullscreen quad declaration");
		Uninitialize();

		return false;
	}

	declaration->SetPersistent(false);

	s_fullscreenQuadBuffer = new NzVertexBuffer(declaration.get(), 4, nzBufferStorage_Hardware, nzBufferUsage_Static);
	declaration.release();

	float vertices[4*2] =
	{
		-1.f, -1.f,
		1.f, -1.f,
		-1.f, 1.f,
		1.f, 1.f,
	};

	if (!s_fullscreenQuadBuffer->Fill(vertices, 0, 4))
	{
		NazaraError("Failed to fill fullscreen quad buffer");
		Uninitialize();

		return false;
	}

	if (!NzMaterial::Initialize())
	{
		NazaraError("Failed to initialize materials");
		Uninitialize();

		return false;
	}

	if (!NzShaderBuilder::Initialize())
	{
		NazaraError("Failed to initialize shader builder");
		Uninitialize();

		return false;
	}

	if (!NzTextureSampler::Initialize())
	{
		NazaraError("Failed to initialize texture sampler");
		Uninitialize();

		return false;
	}

	// Loaders
	NzLoaders_Texture_Register();

	NazaraNotice("Initialized: Renderer module");

	return true;
}

bool NzRenderer::IsEnabled(nzRendererParameter parameter)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return false;
	}

	if (parameter > nzRendererParameter_Max)
	{
		NazaraError("Renderer parameter out of enum");
		return false;
	}
	#endif

	return s_states.parameters[parameter];
}

bool NzRenderer::IsInitialized()
{
	return s_moduleReferenceCounter != 0;
}

void NzRenderer::SetBlendFunc(nzBlendFunc srcBlend, nzBlendFunc dstBlend)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	s_states.srcBlend = srcBlend;
	s_states.dstBlend = dstBlend;
}

void NzRenderer::SetClearColor(const NzColor& color)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	glClearColor(color.r/255.f, color.g/255.f, color.b/255.f, color.a/255.f);
}

void NzRenderer::SetClearColor(nzUInt8 r, nzUInt8 g, nzUInt8 b, nzUInt8 a)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	glClearColor(r/255.f, g/255.f, b/255.f, a/255.f);
}

void NzRenderer::SetClearDepth(double depth)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	glClearDepth(depth);
}

void NzRenderer::SetClearStencil(unsigned int value)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	glClearStencil(value);
}

void NzRenderer::SetDepthFunc(nzRendererComparison compareFunc)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	s_states.depthFunc = compareFunc;
}

void NzRenderer::SetFaceCulling(nzFaceCulling cullingMode)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	s_states.faceCulling = cullingMode;
}

void NzRenderer::SetFaceFilling(nzFaceFilling fillingMode)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	s_states.faceFilling = fillingMode;
}

void NzRenderer::SetIndexBuffer(const NzIndexBuffer* indexBuffer)
{
	#if NAZARA_RENDERER_SAFE
	if (indexBuffer && !indexBuffer->IsSequential() && !indexBuffer->IsHardware())
	{
		NazaraError("Buffer must be hardware");
		return;
	}
	#endif

	if (s_indexBuffer != indexBuffer)
	{
		s_indexBuffer = indexBuffer;
		s_updateFlags |= Update_VAO;
	}
}

void NzRenderer::SetInstancingData(const NzRenderer::InstancingData* instancingData, unsigned int instanceCount)
{
	#if NAZARA_RENDERER_SAFE
	if (!s_capabilities[nzRendererCap_Instancing])
	{
		NazaraError("Instancing not supported");
		return;
	}

	if (!instancingData)
	{
		NazaraError("Instancing data must be valid");
		return;
	}

	if (instanceCount == 0)
	{
		NazaraError("Instance count must be over 0");
		return;
	}

	if (instanceCount > NAZARA_RENDERER_MAX_INSTANCES)
	{
		NazaraError("Instance count is over maximum instance count (" + NzString::Number(instanceCount) + " >= " NazaraStringifyMacro(NAZARA_RENDERER_MAX_INSTANCES) ")");
		return;
	}
	#endif

	if (!s_instancingBuffer->Fill(instancingData, 0, instanceCount, true))
		NazaraError("Failed to fill instancing buffer");
}

void NzRenderer::SetLineWidth(float width)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	#if NAZARA_RENDERER_SAFE
	if (width <= 0.f)
	{
		NazaraError("Width must be over zero");
		return;
	}
	#endif

	s_states.lineWidth = width;
}

void NzRenderer::SetMatrix(nzMatrixType type, const NzMatrix4f& matrix)
{
	#ifdef NAZARA_DEBUG
	if (type > nzMatrixType_Max)
	{
		NazaraError("Matrix type out of enum");
		return;
	}
	#endif

	s_matrices[type].matrix = matrix;
	s_matrices[type].updated = true;

	// Invalidation des combinaisons
	switch (type)
	{
		case nzMatrixType_Projection:
			s_matrices[nzMatrixType_ViewProj].updated = false;
			s_matrices[nzMatrixType_WorldViewProj].updated = false;
			break;

		case nzMatrixType_View:
			s_matrices[nzMatrixType_ViewProj].updated = false;
			s_matrices[nzMatrixType_World].updated = false;
			s_matrices[nzMatrixType_WorldViewProj].updated = false;
			break;

		case nzMatrixType_World:
			s_matrices[nzMatrixType_WorldView].updated = false;
			s_matrices[nzMatrixType_WorldViewProj].updated = false;
			break;

		case nzMatrixType_ViewProj:
			break;

		case nzMatrixType_WorldView:
			s_matrices[nzMatrixType_WorldViewProj].updated = false;
			break;

		case nzMatrixType_WorldViewProj:
			break;
	}

	s_updateFlags |= Update_Matrices;
}

void NzRenderer::SetPointSize(float size)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	#if NAZARA_RENDERER_SAFE
	if (size <= 0.f)
	{
		NazaraError("Size must be over zero");
		return;
	}
	#endif

	s_states.pointSize = size;
}

void NzRenderer::SetRenderStates(const NzRenderStates& states)
{
	s_states = states;
}

void NzRenderer::SetScissorRect(const NzRectui& rect)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	unsigned int height = s_target->GetHeight();

	#if NAZARA_RENDERER_SAFE
	if (!s_target)
	{
		NazaraError("Renderer has no target");
		return;
	}

	unsigned int width = s_target->GetWidth();
	if (rect.x+rect.width > width || rect.y+rect.height > height)
	{
		NazaraError("Rectangle dimensions are out of bounds");
		return;
	}
	#endif

	glScissor(rect.x, height-rect.height-rect.y, rect.width, rect.height);
}

void NzRenderer::SetShader(const NzShader* shader)
{
	#if NAZARA_RENDERER_SAFE
	if (shader && !shader->IsCompiled())
	{
		NazaraError("Shader is not compiled");
		return;
	}
	#endif

	if (s_shader != shader)
	{
		s_shader = shader;
		s_updateFlags |= Update_Shader;
	}
}

void NzRenderer::SetStencilCompareFunction(nzRendererComparison compareFunc)
{
	#ifdef NAZARA_DEBUG
	if (compareFunc > nzRendererComparison_Max)
	{
		NazaraError("Renderer comparison out of enum");
		return;
	}
	#endif

	s_states.stencilCompare = compareFunc;
}

void NzRenderer::SetStencilFailOperation(nzStencilOperation failOperation)
{
	#ifdef NAZARA_DEBUG
	if (failOperation > nzStencilOperation_Max)
	{
		NazaraError("Stencil fail operation out of enum");
		return;
	}
	#endif

	s_states.stencilFail = failOperation;
}

void NzRenderer::SetStencilMask(nzUInt32 mask)
{
	s_states.stencilMask = mask;
}

void NzRenderer::SetStencilPassOperation(nzStencilOperation passOperation)
{
	#ifdef NAZARA_DEBUG
	if (passOperation > nzStencilOperation_Max)
	{
		NazaraError("Stencil pass operation out of enum");
		return;
	}
	#endif

	s_states.stencilPass = passOperation;
}

void NzRenderer::SetStencilReferenceValue(unsigned int refValue)
{
	s_states.stencilReference = refValue;
}

void NzRenderer::SetStencilZFailOperation(nzStencilOperation zfailOperation)
{
	#ifdef NAZARA_DEBUG
	if (zfailOperation > nzStencilOperation_Max)
	{
		NazaraError("Stencil zfail operation out of enum");
		return;
	}
	#endif

	s_states.stencilZFail = zfailOperation;
}

bool NzRenderer::SetTarget(const NzRenderTarget* target)
{
	if (s_target == target)
		return true;

	if (s_target)
	{
		if (!s_target->HasContext())
			s_target->Desactivate();

		s_target = nullptr;
	}

	if (target)
	{
		#if NAZARA_RENDERER_SAFE
		if (!target->IsRenderable())
		{
			NazaraError("Target not renderable");
			return false;
		}
		#endif

		if (!target->Activate())
		{
			NazaraError("Failed to activate target");
			return false;
		}

		s_target = target;
		s_targetSize.Set(target->GetWidth(), target->GetHeight());
	}

	return true;
}

void NzRenderer::SetTexture(nzUInt8 unit, const NzTexture* texture)
{
	#if NAZARA_RENDERER_SAFE
	if (unit >= s_textureUnits.size())
	{
		NazaraError("Texture unit out of range (" + NzString::Number(unit) + " >= " + NzString::Number(s_textureUnits.size()) + ')');
		return;
	}
	#endif

	if (s_textureUnits[unit].texture != texture)
	{
		s_textureUnits[unit].texture = texture;
		s_textureUnits[unit].textureUpdated = false;

		if (texture)
		{
			if (s_textureUnits[unit].sampler.UseMipmaps(texture->HasMipmaps()))
				s_textureUnits[unit].samplerUpdated = false;
		}

		s_dirtyTextureUnits.insert(unit);
		s_updateFlags |= Update_Textures;
	}
}

void NzRenderer::SetTextureSampler(nzUInt8 unit, const NzTextureSampler& sampler)
{
	#if NAZARA_RENDERER_SAFE
	if (unit >= s_textureUnits.size())
	{
		NazaraError("Texture unit out of range (" + NzString::Number(unit) + " >= " + NzString::Number(s_textureUnits.size()) + ')');
		return;
	}
	#endif

	s_textureUnits[unit].sampler = sampler;
	s_textureUnits[unit].samplerUpdated = false;

	if (s_textureUnits[unit].texture)
		s_textureUnits[unit].sampler.UseMipmaps(s_textureUnits[unit].texture->HasMipmaps());

	s_dirtyTextureUnits.insert(unit);
	s_updateFlags |= Update_Textures;
}

void NzRenderer::SetVertexBuffer(const NzVertexBuffer* vertexBuffer)
{
	#if NAZARA_RENDERER_SAFE
	if (vertexBuffer && !vertexBuffer->IsHardware())
	{
		NazaraError("Buffer must be hardware");
		return;
	}
	#endif

	if (vertexBuffer && s_vertexBuffer != vertexBuffer)
	{
		s_vertexBuffer = vertexBuffer;
		s_updateFlags |= Update_VAO;
	}
}

void NzRenderer::SetViewport(const NzRectui& viewport)
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return;
	}
	#endif

	unsigned int height = s_target->GetHeight();

	#if NAZARA_RENDERER_SAFE
	if (!s_target)
	{
		NazaraError("Renderer has no target");
		return;
	}

	unsigned int width = s_target->GetWidth();
	if (viewport.x+viewport.width > width || viewport.y+viewport.height > height)
	{
		NazaraError("Rectangle dimensions are out of bounds");
		return;
	}
	#endif

	glViewport(viewport.x, height-viewport.height-viewport.y, viewport.width, viewport.height);
}

void NzRenderer::Uninitialize()
{
	if (s_moduleReferenceCounter != 1)
	{
		// Le module est soit encore utilisé, soit pas initialisé
		if (s_moduleReferenceCounter > 1)
			s_moduleReferenceCounter--;

		return;
	}

	NzContext::EnsureContext();

	// Libération du module
	s_moduleReferenceCounter = 0;

	s_textureUnits.clear();

	// Loaders
	NzLoaders_Texture_Unregister();

	NzDebugDrawer::Uninitialize();
	NzMaterial::Uninitialize();
	NzShaderBuilder::Uninitialize();
	NzTextureSampler::Uninitialize();

	// Libération des buffers
	delete s_fullscreenQuadBuffer;

	if (s_instancingBuffer)
	{
		delete s_instancingBuffer;
		s_instancingBuffer = nullptr;
	}

	// Libération des VAOs
	for (auto it = s_vaos.begin(); it != s_vaos.end(); ++it)
	{
		GLuint vao = static_cast<GLuint>(it->second);
		glDeleteVertexArrays(1, &vao);
	}

	NzOpenGL::Uninitialize();

	NazaraNotice("Uninitialized: Renderer module");

	// Libération des dépendances
	NzUtility::Uninitialize();
}

void NzRenderer::EnableInstancing(bool instancing)
{
	if (s_instancing != instancing)
	{
		s_updateFlags |= Update_VAO;
		s_instancing = instancing;
	}
}

bool NzRenderer::EnsureStateUpdate()
{
	#ifdef NAZARA_DEBUG
	if (NzContext::GetCurrent() == nullptr)
	{
		NazaraError("No active context");
		return false;
	}
	#endif

	#if NAZARA_RENDERER_SAFE
	if (!s_shader)
	{
		NazaraError("No shader");
		return false;
	}
	#endif

	NzAbstractShader* shaderImpl = s_shader->m_impl;
	shaderImpl->Bind();

	// Si le shader a été changé depuis la dernière fois
	if (s_updateFlags & Update_Shader)
	{
		// Récupération des indices des variables uniformes (-1 si la variable n'existe pas)
		s_matrices[nzMatrixType_Projection].location = shaderImpl->GetUniformLocation(nzShaderUniform_ProjMatrix);
		s_matrices[nzMatrixType_View].location = shaderImpl->GetUniformLocation(nzShaderUniform_ViewMatrix);
		s_matrices[nzMatrixType_World].location = shaderImpl->GetUniformLocation(nzShaderUniform_WorldMatrix);

		s_matrices[nzMatrixType_ViewProj].location = shaderImpl->GetUniformLocation(nzShaderUniform_ViewProjMatrix);
		s_matrices[nzMatrixType_WorldView].location = shaderImpl->GetUniformLocation(nzShaderUniform_WorldViewMatrix);
		s_matrices[nzMatrixType_WorldViewProj].location = shaderImpl->GetUniformLocation(nzShaderUniform_WorldViewProjMatrix);

		s_updateFlags |= Update_Matrices;
		for (unsigned int i = 0; i <= nzMatrixType_Max; ++i)
			s_matrices[i].sent = false; // Changement de shader, on renvoie toutes les matrices demandées

		s_updateFlags &= ~Update_Shader;
	}

	shaderImpl->BindTextures();

	if (s_updateFlags != Update_None)
	{
		if (s_updateFlags & Update_Textures)
		{
			if (s_useSamplerObjects)
			{
				for (unsigned int i : s_dirtyTextureUnits)
				{
					TextureUnit& unit = s_textureUnits[i];

					if (!unit.textureUpdated)
					{
						NzOpenGL::SetTextureUnit(i);
						unit.texture->Bind();

						unit.textureUpdated = true;
					}

					if (!unit.samplerUpdated)
					{
						unit.sampler.Bind(i);
						unit.samplerUpdated = true;
					}
				}
			}
			else
			{
				for (unsigned int i : s_dirtyTextureUnits)
				{
					TextureUnit& unit = s_textureUnits[i];

					NzOpenGL::SetTextureUnit(i);

					unit.texture->Bind();
					unit.textureUpdated = true;

					unit.sampler.Apply(unit.texture);
					unit.samplerUpdated = true;
				}
			}

			s_dirtyTextureUnits.clear(); // Ne change pas la capacité
			s_updateFlags &= ~Update_Textures;
		}

		if (s_updateFlags & Update_Matrices)
		{
			for (unsigned int i = 0; i <= nzMatrixType_Max; ++i)
			{
				MatrixUnit& unit = s_matrices[i];
				if (unit.location != -1) // On ne traite que les matrices existant dans le shader
				{
					if (!unit.updated)
						UpdateMatrix(static_cast<nzMatrixType>(i));

					shaderImpl->SendMatrix(unit.location, unit.matrix);
					unit.sent = true;
				}
			}

			s_updateFlags &= ~Update_Matrices;
		}

		if (s_updateFlags & Update_VAO)
		{
			#if NAZARA_RENDERER_SAFE
			if (!s_vertexBuffer)
			{
				NazaraError("No vertex buffer");
				return false;
			}
			#endif

			bool update;

			// Si les VAOs sont supportés, on entoure nos appels par ceux-ci
			if (s_useVertexArrayObjects)
			{
				// On recherche si un VAO existe déjà avec notre configuration
				// Note: Les VAOs ne sont pas partagés entre les contextes, ces derniers font donc partie de notre configuration

				VAO_Key key(NzContext::GetCurrent(), s_indexBuffer, s_vertexBuffer, s_instancing);
				auto it = s_vaos.find(key);

				if (it == s_vaos.end())
				{
					// On créé notre VAO
					glGenVertexArrays(1, &s_currentVAO);
					glBindVertexArray(s_currentVAO);

					// On l'ajoute à notre liste
					s_vaos.insert(std::make_pair(key, static_cast<unsigned int>(s_currentVAO)));

					// Et on indique qu'on veut le programmer
					update = true;
				}
				else
				{
					// Notre VAO existe déjà, il est donc inutile de le reprogrammer
					s_currentVAO = it->second;

					update = false;
				}
			}
			else
				update = true; // Fallback si les VAOs ne sont pas supportés

			if (update)
			{
				NzHardwareBuffer* vertexBufferImpl = static_cast<NzHardwareBuffer*>(s_vertexBuffer->GetBuffer()->GetImpl());
				vertexBufferImpl->Bind();

				const NzVertexDeclaration* vertexDeclaration = s_vertexBuffer->GetVertexDeclaration();

				const nzUInt8* buffer = static_cast<const nzUInt8*>(s_vertexBuffer->GetPointer());
				unsigned int stride = vertexDeclaration->GetStride(nzElementStream_VertexData);
				for (unsigned int i = 0; i <= nzElementUsage_Max; ++i)
				{
					nzElementUsage usage = static_cast<nzElementUsage>(i);
					if (vertexDeclaration->HasElement(nzElementStream_VertexData, usage))
					{
						const NzVertexElement* element = vertexDeclaration->GetElement(nzElementStream_VertexData, usage);

						glEnableVertexAttribArray(NzOpenGL::AttributeIndex[i]);
						glVertexAttribPointer(NzOpenGL::AttributeIndex[i],
											  NzVertexDeclaration::GetElementCount(element->type),
											  NzOpenGL::ElementType[element->type],
											  (element->type == nzElementType_Color) ? GL_TRUE : GL_FALSE,
											  stride,
											  &buffer[element->offset]);
					}
					else
						glDisableVertexAttribArray(NzOpenGL::AttributeIndex[i]);
				}

				unsigned int instanceMatrixIndex = NzOpenGL::AttributeIndex[nzElementUsage_TexCoord] + 8;
				if (s_instancing)
				{
					static_cast<NzHardwareBuffer*>(s_instancingBuffer->GetImpl())->Bind();

					for (unsigned int i = 0; i < 4; ++i)
					{
						glEnableVertexAttribArray(instanceMatrixIndex);
						glVertexAttribPointer(instanceMatrixIndex, 4, GL_FLOAT, GL_FALSE, sizeof(InstancingData), reinterpret_cast<GLvoid*>(offsetof(InstancingData, worldMatrix) + i*sizeof(float)*4));
						glVertexAttribDivisor(instanceMatrixIndex, 1);

						instanceMatrixIndex++;
					}
				}
				else
				{
					for (unsigned int i = 0; i < 4; ++i)
						glDisableVertexAttribArray(instanceMatrixIndex++);
				}

				if (s_indexBuffer && !s_indexBuffer->IsSequential())
				{
					NzHardwareBuffer* indexBufferImpl = static_cast<NzHardwareBuffer*>(s_indexBuffer->GetBuffer()->GetImpl());
					indexBufferImpl->Bind();
				}
				else
					NzOpenGL::BindBuffer(nzBufferType_Index, 0);
			}

			if (s_useVertexArrayObjects)
			{
				// Si nous venons de définir notre VAO, nous devons le débinder pour indiquer la fin de sa construction
				if (update)
					glBindVertexArray(0);

				// En cas de non-support des VAOs, les attributs doivent être respécifiés à chaque frame
				s_updateFlags &= ~Update_VAO;
			}
		}

		#ifdef NAZARA_DEBUG
		if (s_updateFlags != Update_None)
			NazaraWarning("Update flags not fully cleared");
		#endif
	}

	// On bind notre VAO
	if (s_useVertexArrayObjects)
		glBindVertexArray(s_currentVAO);

	// On vérifie que les textures actuellement bindées sont bien nos textures
	for (unsigned int i = 0; i < s_maxTextureUnit; ++i)
	{
		const NzTexture* texture = s_textureUnits[i].texture;
		if (texture)
			NzOpenGL::BindTexture(i, texture->GetType(), texture->GetOpenGLID());
	}

	NzOpenGL::ApplyStates(s_states);

	return true;
}

void NzRenderer::UpdateMatrix(nzMatrixType type)
{
	#ifdef NAZARA_DEBUG
	if (type > nzMatrixType_Max)
	{
		NazaraError("Matrix type out of enum");
		return;
	}
	#endif

	switch (type)
	{
		case nzMatrixType_Projection:
		case nzMatrixType_View:
		case nzMatrixType_World:
			break;

		case nzMatrixType_ViewProj:
			s_matrices[nzMatrixType_ViewProj].matrix = s_matrices[nzMatrixType_View].matrix * s_matrices[nzMatrixType_Projection].matrix;
			s_matrices[nzMatrixType_ViewProj].updated = true;
			break;

		case nzMatrixType_WorldView:
			s_matrices[nzMatrixType_WorldView].matrix = s_matrices[nzMatrixType_World].matrix;
			s_matrices[nzMatrixType_WorldView].matrix.ConcatenateAffine(s_matrices[nzMatrixType_View].matrix);
			s_matrices[nzMatrixType_WorldView].updated = true;
			break;

		case nzMatrixType_WorldViewProj:
			if (!s_matrices[nzMatrixType_WorldView].updated)
				UpdateMatrix(nzMatrixType_WorldView);

			s_matrices[nzMatrixType_WorldViewProj].matrix = s_matrices[nzMatrixType_WorldView].matrix;
			s_matrices[nzMatrixType_WorldViewProj].matrix.Concatenate(s_matrices[nzMatrixType_Projection].matrix);
			s_matrices[nzMatrixType_WorldViewProj].updated = true;
			break;
	}
}

unsigned int NzRenderer::s_moduleReferenceCounter = 0;
