//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
// Some header #define's Bool as int, which breaks stuff in sdf/types.h.
// Include it first to sidestep the problem. :-/
#include "pxr/usd/sdf/types.h"

// Make sure to include glew first before any header that might include
// gl.h
#include "pxr/imaging/glf/glew.h"
#include "pxrUsdMayaGL/hdRenderer.h"
#include "px_vp20/utils.h"

#include <maya/MDagPath.h>
#include <maya/MDrawData.h>
#include <maya/MDrawRequest.h>
#include <maya/M3dView.h>
#include <maya/MMaterial.h>
#include <maya/MMatrix.h>
#include <maya/MPxSurfaceShape.h>
#include <maya/MStateManager.h>
#include <maya/MViewport2Renderer.h>
#include <maya/MHWGeometryUtilities.h>

#include <GL/glut.h>

void
UsdMayaGLHdRenderer::CheckRendererSetup(
        const UsdPrim& usdPrim,
        const SdfPathVector& excludePaths)
{
    if (usdPrim != _renderedPrim or excludePaths != _excludePrimPaths) {
        _excludePrimPaths = excludePaths;
        boost::scoped_ptr<UsdImagingGL> tmpRenderer( new UsdImagingGL(usdPrim.GetPath(), _excludePrimPaths) );

        _renderer.swap(tmpRenderer);
        _renderedPrim = usdPrim;
    }
}

void UsdMayaGLHdRenderer::GenerateDefaultVp2DrawRequests(
    const MDagPath& objPath,
    const MHWRender::MFrameContext& frameContext,
    const MBoundingBox& bounds,
    UsdMayaGLHdRenderer::RequestDataArray *requestArray
    )
{
    if(requestArray == NULL) {
        return;
    }
    M3dView viewHelper = M3dView::active3dView();

    const MHWRender::DisplayStatus displayStatus =
        MHWRender::MGeometryUtilities::displayStatus(objPath);

    const bool isSelected =
            (displayStatus == MHWRender::kActive) ||
            (displayStatus == MHWRender::kLead)   ||
            (displayStatus == MHWRender::kHilite);

    const MColor mayaWireframeColor =
        MHWRender::MGeometryUtilities::wireframeColor(objPath);
    const GfVec4f wireframeColor(mayaWireframeColor.r,
                                 mayaWireframeColor.g,
                                 mayaWireframeColor.b,
                                 mayaWireframeColor.a);


    requestArray->clear();

    if( !(frameContext.getDisplayStyle() & MHWRender::MFrameContext::DisplayStyle::kWireFrame) &&
        !(frameContext.getDisplayStyle() & MHWRender::MFrameContext::DisplayStyle::kBoundingBox) )
    {
        UsdMayaGLHdRenderer::RequestData shadedRequest;
        shadedRequest.fWireframeColor = wireframeColor;
        shadedRequest.bounds = bounds;
// Maya 2015 lacks MHWRender::MFrameContext::DisplayStyle::kFlatShaded for whatever reason...
#if MAYA_API_VERSION >= 201600
        if( frameContext.getDisplayStyle() & MHWRender::MFrameContext::DisplayStyle::kFlatShaded )
        {
            shadedRequest.drawRequest.setToken( UsdMayaGLHdRenderer::DRAW_SHADED_FLAT );
            shadedRequest.drawRequest.setDisplayStyle(M3dView::kFlatShaded);
        }
        else
#endif
        {
            shadedRequest.drawRequest.setToken( UsdMayaGLHdRenderer::DRAW_SHADED_SMOOTH );
            shadedRequest.drawRequest.setDisplayStyle(M3dView::kGouraudShaded);
        }
        
        requestArray->push_back( shadedRequest );
    }

    if(isSelected || (frameContext.getDisplayStyle() & MHWRender::MFrameContext::DisplayStyle::kWireFrame) )
    {
        UsdMayaGLHdRenderer::RequestData wireRequest;
        wireRequest.bounds = bounds;
        wireRequest.drawRequest.setToken( UsdMayaGLHdRenderer::DRAW_WIREFRAME);
        wireRequest.drawRequest.setDisplayStyle(M3dView::kWireFrame);
        wireRequest.fWireframeColor = wireframeColor;
        requestArray->push_back( wireRequest );
    }
}

void UsdMayaGLHdRenderer::RenderVp2(
    const RequestDataArray &requests,
    const MHWRender::MDrawContext& context,
    UsdImagingGL::RenderParams params) const
{
    using namespace MHWRender;
    
    MStatus status;
	MHWRender::MRenderer* theRenderer = MHWRender::MRenderer::theRenderer();
	if (!theRenderer) return;

    MHWRender::MStateManager* stateMgr = context.getStateManager();
    if (!stateMgr) return;

    const int displayStyle = context.getDisplayStyle();
    if (displayStyle == 0) return;

    if (displayStyle & MDrawContext::kXray) {
        // Viewport 2.0 will call draw() twice when drawing transparent objects
        // (X-Ray mode). We skip the first draw() call.
        const MRasterizerState* rasterState = stateMgr->getRasterizerState();
        if (rasterState && rasterState->desc().cullMode == MRasterizerState::kCullFront) {
            return;
        }
    }

    if (!theRenderer->drawAPIIsOpenGL()) return;


    glPushAttrib(GL_CURRENT_BIT | GL_LIGHTING_BIT);

    MMatrix worldView = context.getMatrix(MHWRender::MDrawContext::kWorldViewMtx, &status);
    GfMatrix4d modelViewMatrix(worldView.matrix);

    MMatrix projection = context.getMatrix(MHWRender::MDrawContext::kProjectionMtx, &status);
    GfMatrix4d projectionMatrix(projection.matrix);

    // get root matrix
    MMatrix root = context.getMatrix(MHWRender::MDrawContext::kWorldMtx, &status);
    GfMatrix4d rootMatrix(root.matrix);

    // Extract camera settings from maya view
    int viewX, viewY, viewWidth, viewHeight;
    context.getViewportDimensions(viewX, viewY, viewWidth, viewHeight);

    GfVec4d viewport(viewX, viewY, viewWidth, viewHeight);

    M3dView::DisplayStyle viewDisplayStyle = displayStyle & MDrawContext::kWireFrame ? 
        M3dView::kWireFrame : M3dView::kGouraudShaded;

    if(viewDisplayStyle == M3dView::kGouraudShaded)
    {
        px_vp20Utils::setupLightingGL(context);
        glEnable(GL_LIGHTING);
    }

    _renderer->SetCameraState(modelViewMatrix, projectionMatrix, viewport);

    _renderer->SetLightingStateFromOpenGL();
    
    
    TF_FOR_ALL(it, requests) {
        RequestData request = *it;
        if(viewDisplayStyle == M3dView::kWireFrame && request.drawRequest.displayStyle() == M3dView::kGouraudShaded) {
            request.drawRequest.setDisplayStyle(viewDisplayStyle);
        }

        switch(request.drawRequest.token()) {
        case UsdMayaGLHdRenderer::DRAW_WIREFRAME:
        case UsdMayaGLHdRenderer::DRAW_POINTS: {

            params.drawMode = request.drawRequest.token() == UsdMayaGLHdRenderer::DRAW_WIREFRAME ? UsdImagingGL::DRAW_WIREFRAME :
                UsdImagingGL::DRAW_POINTS;
            params.enableLighting = false;
            params.cullStyle = UsdImagingEngine::CULL_STYLE_NOTHING;

            params.overrideColor = request.fWireframeColor;

            // Get and render usdPrim
            _renderer->Render(_renderedPrim, params);

            break;
        }
        case UsdMayaGLHdRenderer::DRAW_SHADED_FLAT: 
        case UsdMayaGLHdRenderer::DRAW_SHADED_SMOOTH: {


            params.drawMode = ((request.drawRequest.token() == UsdMayaGLHdRenderer::DRAW_SHADED_FLAT) ?
                UsdImagingGL::DRAW_GEOM_FLAT : UsdImagingGL::DRAW_GEOM_SMOOTH);
            params.enableLighting = true;
            params.cullStyle = UsdImagingEngine::CULL_STYLE_BACK_UNLESS_DOUBLE_SIDED;

            _renderer->Render(_renderedPrim, params);


            break;
        }
        case UsdMayaGLHdRenderer::DRAW_BOUNDING_BOX: {

            MBoundingBox bbox = request.bounds;
            glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
            glDisable(GL_LIGHTING);
            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadMatrixd(projection.matrix[0]);
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadMatrixd(worldView.matrix[0]);

            glColor3fv((float*)&request.fWireframeColor);
            glTranslated( bbox.center()[0],
                          bbox.center()[1],
                          bbox.center()[2] );
            glScaled( bbox.width(), bbox.height(), bbox.depth() );
            glutWireCube(1.0);
            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glMatrixMode(GL_MODELVIEW);
            glPopMatrix();
            glPopAttrib(); // GL_ENABLE_BIT | GL_CURRENT_BIT

            break;
        }
        }
    }

    if(displayStyle == M3dView::kGouraudShaded)
    {
        px_vp20Utils::unsetLightingGL(context);
    }

    glPopAttrib(); // GL_CURRENT_BIT | GL_LIGHTING_BIT
}

void 
UsdMayaGLHdRenderer::Render(
        const MDrawRequest& request, 
        M3dView& view,
        UsdImagingGL::RenderParams params) const
{
    if (not _renderedPrim.IsValid()) {
        return;
    }
    view.beginGL();

    // Extract camera settings from maya view
    MMatrix mayaViewMatrix;
    MMatrix mayaProjMatrix;
    unsigned int viewX, viewY, viewWidth, viewHeight;

    // Have to pull out as MMatrix
    view.modelViewMatrix(mayaViewMatrix);
    view.projectionMatrix(mayaProjMatrix);
    view.viewport(viewX, viewY, viewWidth, viewHeight);
    // Convert MMatrix to GfMatrix. It's a shame because
    // the memory layout is identical
    GfMatrix4d modelViewMatrix(mayaViewMatrix.matrix);
    GfMatrix4d projectionMatrix(mayaProjMatrix.matrix);
    GfVec4d viewport(viewX, viewY, viewWidth, viewHeight);
    
    _renderer->SetCameraState(modelViewMatrix, projectionMatrix, viewport);
    _renderer->SetLightingStateFromOpenGL();


    glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
    glEnable(GL_FRAMEBUFFER_SRGB_EXT);
    glEnable(GL_LIGHTING);

    int drawMode = request.token();
    switch (drawMode) {
        case DRAW_WIREFRAME:
        case DRAW_POINTS: {


            params.drawMode = drawMode == DRAW_WIREFRAME ? UsdImagingGL::DRAW_WIREFRAME :
                                                                   UsdImagingGL::DRAW_POINTS;
            params.enableLighting = false;
            glGetFloatv(GL_CURRENT_COLOR, &params.overrideColor[0]);

            // Get and render usdPrim
            _renderer->Render(_renderedPrim, params);


            break;
        }
        case DRAW_SHADED_FLAT: 
        case DRAW_SHADED_SMOOTH: {

            //
            // setup the material
            //



            params.drawMode = drawMode == DRAW_SHADED_FLAT ?
                UsdImagingGL::DRAW_SHADED_FLAT : UsdImagingGL::DRAW_SHADED_SMOOTH;

            _renderer->Render(_renderedPrim, params);

            break;
        }
        case DRAW_BOUNDING_BOX: {
            MDrawData drawData = request.drawData();
            const MPxSurfaceShape* shape = static_cast<const MPxSurfaceShape*>(
                    drawData.geometry());

            if (not shape) {
                break;
            }
            if( !shape->isBounded() )
                break;
            
            MBoundingBox bbox = shape->boundingBox();
            
            glPushAttrib( GL_ENABLE_BIT );
            // Make sure we are not using lighitng when drawing
            glDisable(GL_LIGHTING);
            glPushMatrix();
            glTranslated( bbox.center()[0],
                          bbox.center()[1],
                          bbox.center()[2] );
            glScaled( bbox.width(), bbox.height(), bbox.depth() );
            glutWireCube(1.0);
            glPopMatrix();
            glPopAttrib(); // GL_ENABLE_BIT
            
            break;
        }
    }
    
    glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    glPopAttrib(); // GL_ENABLE_BIT | GL_CURRENT_BIT
    view.endGL();
}

bool 
UsdMayaGLHdRenderer::TestIntersection(
        M3dView& view,
        UsdImagingGL::RenderParams params,
        GfVec3d* hitPoint) const
{
    // Guard against user clicking in viewer before renderer is setup
    if (not _renderer) {
        return false;
    }

    if (not _renderedPrim.IsValid()) {
        return false;
    }

    // We need to get the view and projection matrices for the
    // area of the view that the user has clicked or dragged.
    // Unfortunately the view does not give us that in an easy way..
    // If we extract the view and projection matrices from the view object,
    // it is just for the regular camera. The selectInfo also gives us the
    // selection box, so we could use that to construct the correct view
    // and projection matrixes, but if we call beginSelect on the view as
    // if we were going to use the selection buffer, maya will do all the
    // work for us and we can just extract the matrices from opengl.
    GfMatrix4d viewMatrix;
    GfMatrix4d projectionMatrix;
    GLuint glHitRecord;
    // Hit record can just be one because we are not going to draw
    // anything anyway. We only want the matrices :)
    view.beginSelect(&glHitRecord, 1);
    glGetDoublev(GL_MODELVIEW_MATRIX, viewMatrix.GetArray());
    glGetDoublev(GL_PROJECTION_MATRIX, projectionMatrix.GetArray());
    view.endSelect();

    params.drawMode = UsdImagingGL::DRAW_GEOM_ONLY;

    return _renderer->TestIntersection(
        viewMatrix, projectionMatrix,
        GfMatrix4d().SetIdentity(), 
        _renderedPrim, params,
        hitPoint);
}

/* static */
float
UsdMayaGLHdRenderer::SubdLevelToComplexity(int subdLevel)
{
    // Here is how to map subdivision level to the RenderParameter complexity
    // It is done this way for historical reasons
    //
    // For complexity->subdLevel:
    //   (int)(TfMax(0.0f,TfMin(1.0f,complexity-1.0f))*5.0f+0.1f);
    // 
    // complexity usd
    //    1.0      0
    //    1.1      1
    //    1.2      2
    //    1.3      3
    //    1.4      3  (not 4, because of floating point precision)
    //    1.5      5
    //    1.6      6
    //    1.7      7
    //    1.8      8
    //    1.9      8
    //    2.0      8
    //
    return 1.0+(float(subdLevel)*0.1f);
}
