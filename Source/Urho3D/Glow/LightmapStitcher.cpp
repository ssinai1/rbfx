//
// Copyright (c) 2008-2019 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Glow/LightmapStitcher.h"

#include "../Core/Context.h"
#include "../IO/Log.h"
#include "../Graphics/Camera.h"
#include "../Graphics/Geometry.h"
#include "../Graphics/Graphics.h"
#include "../Graphics/Material.h"
#include "../Graphics/Model.h"
#include "../Graphics/Octree.h"
#include "../Graphics/StaticModel.h"
#include "../Graphics/VertexBuffer.h"
#include "../Graphics/View.h"
#include "../Resource/ResourceCache.h"
#include "../Scene/Scene.h"

namespace Urho3D
{

namespace
{

/// Return texture format for given amount of channels.
unsigned GetStitchTextureFormat(unsigned numChannels)
{
    switch (numChannels)
    {
    case 1: return Graphics::GetFloat32Format();
    case 2: return Graphics::GetRGFloat32Format();
    case 4: return Graphics::GetRGBAFloat32Format();
    default:
        assert(0);
        return 0;
    }
}

/// Create scene for ping-pong stitching.
SharedPtr<Scene> CreateStitchingScene(Context* context,
    const LightmapStitchingSettings& settings, Texture2D* inputTexture, Model* seamsModel)
{
    auto cache = context->GetCache();

    auto scene = MakeShared<Scene>(context);
    auto octree = scene->CreateComponent<Octree>();

    if (Node* cameraNode = scene->CreateChild("Camera"))
    {
        cameraNode->SetPosition(Vector3::UP);
        cameraNode->SetDirection(Vector3::DOWN);

        auto camera = cameraNode->CreateComponent<Camera>();
        camera->SetOrthographic(true);
        camera->SetOrthoSize(1.0f);
        camera->SetNearClip(0.1f);
        camera->SetNearClip(10.0f);
    }

    if (Node* backgroundNode = scene->CreateChild("Background"))
    {
        auto material = MakeShared<Material>(context);
        auto technique = cache->GetResource<Technique>(settings.stitchBackgroundTechniqueName_);
        material->SetTechnique(0, technique);
        material->SetTexture(TU_DIFFUSE, inputTexture);
        material->SetRenderOrder(0);

        auto staticModel = backgroundNode->CreateComponent<StaticModel>();
        staticModel->SetModel(cache->GetResource<Model>(settings.stitchBackgroundModelName_));
        staticModel->SetMaterial(material);
    }

    if (Node* seamsNode = scene->CreateChild("Seams"))
    {
        seamsNode->SetPosition({ -0.5f, 0.1f, -0.5f });

        auto material = MakeShared<Material>(context);
        auto technique = cache->GetResource<Technique>(settings.stitchSeamsTechniqueName_);
        material->SetTechnique(0, technique);
        material->SetTexture(TU_DIFFUSE, inputTexture);
        material->SetShaderParameter("MatDiffColor", Color(1.0f, 1.0f, 1.0f, settings.blendFactor_));
        material->SetRenderOrder(1);

        auto staticModel = seamsNode->CreateComponent<StaticModel>();
        staticModel->SetModel(seamsModel);
        staticModel->SetMaterial(material);
    }

    octree->Update({});
    return scene;
}

/// Create View and Viewport for stitching.
ea::pair<SharedPtr<View>, SharedPtr<Viewport>> CreateStitchingViewAndViewport(
    Scene* scene, RenderPath* renderPath, Texture2D* outputTexture)
{
    Context* context = scene->GetContext();

    // Setup viewport
    auto viewport = MakeShared<Viewport>(context);
    viewport->SetCamera(scene->GetComponent<Camera>(true));
    viewport->SetRect(IntRect::ZERO);
    viewport->SetRenderPath(renderPath);
    viewport->SetScene(scene);

    // Setup scene
    auto view = MakeShared<View>(context);
    view->Define(outputTexture->GetRenderSurface(), viewport);
    view->Update({});

    return { view, viewport };
}

/// Create vertex buffer for lightmap seams.
SharedPtr<VertexBuffer> CreateSeamsVertexBuffer(Context* context, const LightmapSeamVector& seams)
{
    static const ea::vector<VertexElement> vertexElements =
    {
        VertexElement{ TYPE_VECTOR3, SEM_POSITION },
        VertexElement{ TYPE_VECTOR2, SEM_TEXCOORD },
    };

    ea::vector<float> vertexData;
    vertexData.reserve(seams.size() * 2 * 5);
    for (const LightmapSeam& seam : seams)
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            vertexData.push_back(seam.positions_[i].x_);
            vertexData.push_back(0.0f);
            vertexData.push_back(1.0f - seam.positions_[i].y_);
            vertexData.push_back(seam.otherPositions_[i].x_);
            vertexData.push_back(seam.otherPositions_[i].y_);
        }
    }

    auto vertexBuffer = MakeShared<VertexBuffer>(context);
    vertexBuffer->SetShadowed(true);
    vertexBuffer->SetSize(seams.size() * 2, vertexElements);
    vertexBuffer->SetData(vertexData.data());
    return vertexBuffer;
}

}

LightmapStitchingContext InitializeStitchingContext(Context* context, unsigned lightmapSize, unsigned numChannels)
{
    LightmapStitchingContext result;
    result.context_ = context;
    result.lightmapSize_ = lightmapSize;

    const unsigned textureFormat = GetStitchTextureFormat(numChannels);
    result.pingTexture_ = MakeShared<Texture2D>(context);
    result.pongTexture_ = MakeShared<Texture2D>(context);

    for (Texture2D* texture : { result.pingTexture_, result.pongTexture_ })
    {
        const int size = static_cast<int>(lightmapSize);
        texture->SetNumLevels(1);
        texture->SetSize(size, size, textureFormat, TEXTURE_RENDERTARGET);
    }

    return result;
}

SharedPtr<Model> CreateSeamsModel(Context* context, const LightmapSeamVector& seams)
{
    SharedPtr<VertexBuffer> vertexBuffer = CreateSeamsVertexBuffer(context, seams);

    auto model = MakeShared<Model>(context);
    model->SetBoundingBox({ -Vector3::ONE, Vector3::ONE });
    model->SetNumGeometries(1);
    model->SetNumGeometryLodLevels(0, 1);
    model->SetVertexBuffers({ vertexBuffer }, {}, {});

    auto geometry = MakeShared<Geometry>(context);
    geometry->SetNumVertexBuffers(1);
    geometry->SetVertexBuffer(0, vertexBuffer);
    geometry->SetDrawRange(LINE_LIST, 0, 0, 0, seams.size() * 2, false);
    model->SetGeometry(0, 0, geometry);

    return model;
}

void StitchLightmapSeams(LightmapStitchingContext& stitchingContext, ea::vector<Vector4>& imageData,
    const LightmapStitchingSettings& settings, Model* seamsModel)
{
    Context* context = stitchingContext.context_;
    auto graphics = context->GetGraphics();

    // Initialize scenes and render path
    SharedPtr<RenderPath> renderPath = LoadRenderPath(context, settings.renderPathName_);
    auto pingScene = CreateStitchingScene(context, settings, stitchingContext.pongTexture_, seamsModel);
    auto pongScene = CreateStitchingScene(context, settings, stitchingContext.pingTexture_, seamsModel);
    auto pingViewViewport = CreateStitchingViewAndViewport(pingScene, renderPath, stitchingContext.pingTexture_);
    auto pongViewViewport = CreateStitchingViewAndViewport(pongScene, renderPath, stitchingContext.pongTexture_);

    if (!graphics->BeginFrame())
    {
        URHO3D_LOGERROR("Failed to begin lightmap geometry buffer rendering \"{}\"");
        return;
    }

    // Prepare for ping-pong
    Texture2D* currentTexture = stitchingContext.pongTexture_;
    Texture2D* swapTexture = stitchingContext.pingTexture_;
    View* currentView = pingViewViewport.first;
    View* swapView = pongViewViewport.first;

    const int size = static_cast<int>(stitchingContext.lightmapSize_);
    currentTexture->SetData(0, 0, 0, size, size, imageData.data());

    // Ping-pong rendering
    for (unsigned i = 0; i < settings.numIterations_; ++i)
    {
        currentView->Render();
        ea::swap(currentTexture, swapTexture);
        ea::swap(currentView, swapView);
    }

    // Finish
    currentTexture->GetData(0, imageData.data());
    graphics->EndFrame();
}

}