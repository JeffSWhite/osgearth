/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2020 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include <osgEarth/InstanceCloud>

#if OSG_VERSION_GREATER_OR_EQUAL(3,6,0)

#include <osgEarth/VirtualProgram>
#include <osg/Program>
#include <osg/GLExtensions>

#ifndef GL_DYNAMIC_STORAGE_BIT
#define GL_DYNAMIC_STORAGE_BIT 0x0100
#endif

using namespace osgEarth;

#undef LC
#define LC "[InstanceCloud] "

//...................................................................

#define BINDING_COMMAND_BUFFER 0
#define BINDING_RENDER_BUFFER 1
#define BINDING_POINTS_BUFFER 2

namespace
{
    //const char* cull_CS =
    //    "#version 430\n"

    //    "layout(local_size_x=1, local_size_y=1, local_size_z=1) in; \n"

    //    "struct DrawElementsIndirectCommand { \n"
    //    "    uint count; \n"
    //    "    uint instanceCount; \n"
    //    "    uint firstIndex; \n"
    //    "    uint baseVertex; \n"
    //    "    uint baseInstance; \n"
    //    "}; \n"

    //    "layout(std430, binding=0) buffer DrawCommandsBuffer { \n"
    //    "    DrawElementsIndirectCommand cmd[]; \n"
    //    "}; \n"

    //    "layout(std430, binding=2) buffer PointsBuffer { \n"
    //    "    vec4 points[]; \n"
    //    "}; \n"

    //    "layout(std430, binding=1) buffer RenderBuffer { \n"
    //    "    vec4 render[]; \n"
    //    "}; \n"

    //    "bool visible(in vec4 model) \n"
    //    "{ \n"
    //    "    vec4 clip = gl_ModelViewProjectionMatrix * model; \n"
    //    "    clip.xyz = abs(clip.xyz/clip.w); \n"
    //    "    const float f = 1.0; \n"
    //    "    return clip.x <= f && clip.y <= f; \n"
    //    "} \n"

    //    "void main() { \n"
    //    "    const uint i = gl_GlobalInvocationID.x; \n"
    //    "    if (visible(points[i])) // frustum cull \n"
    //    "    { \n"
    //    "        uint slot = atomicAdd(cmd[0].instanceCount, 1); \n"
    //    "        render[slot] = points[i]; \n"
    //    "    } \n"
    //    "} \n";

    const char* cull_CS =
        "#version 430\n"

        "layout(local_size_x=1, local_size_y=1, local_size_z=1) in; \n"

        "struct DrawElementsIndirectCommand { \n"
        "    uint count; \n"
        "    uint instanceCount; \n"
        "    uint firstIndex; \n"
        "    uint baseVertex; \n"
        "    uint baseInstance; \n"
        "}; \n"

        "layout(binding=0, std430) buffer DrawCommandsBuffer { \n"
        "    DrawElementsIndirectCommand cmd[]; \n"
        "}; \n"

        "struct RenderData { \n"
        "    vec4 vertex; \n"
        "    vec2 tilec; \n"
        "    vec2 _padding; \n"
        "}; \n"

        "layout(binding=1, std430) writeonly buffer RenderBuffer { \n"
        "    RenderData render[]; \n"
        "}; \n"

        "bool inFrustum(in vec4 vertex_view) \n"
        "{ \n"
        "    vec4 clip = gl_ProjectionMatrix * vertex_view; \n"
        "    clip.xyz /= clip.w; \n"
        "    return abs(clip.x) <= 1.01 && clip.y < 1.0; \n"
        "} \n"

        "uniform vec3 oe_GroundCover_LL, oe_GroundCover_UR; \n"
        "uniform sampler2D oe_GroundCover_noiseTex; \n"
        "uniform vec2 oe_tile_elevTexelCoeff; \n"
        "uniform sampler2D oe_tile_elevationTex; \n"
        "uniform mat4 oe_tile_elevationTexMatrix; \n"
        "uniform uint oe_GroundCover_tileNum; \n"
        "uniform float oe_GroundCover_maxDistance; \n"
        "uniform vec3 oe_Camera; \n"
        "uniform float oe_GroundCover_colorMinSaturation; \n"

        "#pragma import_defines(OE_GROUNDCOVER_COLOR_SAMPLER) \n"
        "#pragma import_defines(OE_GROUNDCOVER_COLOR_MATRIX) \n"
        "#ifdef OE_GROUNDCOVER_COLOR_SAMPLER \n"
                "uniform sampler2D OE_GROUNDCOVER_COLOR_SAMPLER ; \n"
                "uniform mat4 OE_GROUNDCOVER_COLOR_MATRIX ; \n"
        "#endif \n"

        "#ifdef OE_GROUNDCOVER_COLOR_SAMPLER \n"

        "// https://stackoverflow.com/a/17897228/4218920 \n"
        "vec3 rgb2hsv(vec3 c) { \n"
        "    const vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0); \n"
        "    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g)); \n"
        "    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r)); \n"
        "    float d = q.x - min(q.w, q.y); \n"
        "    const float e = 1.0e-10; \n"
        "    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x); \n"
        "} \n"

        "bool isLegalColor(in vec2 tilec) { \n"
        "    vec4 c = texture(OE_GROUNDCOVER_COLOR_SAMPLER, (OE_GROUNDCOVER_COLOR_MATRIX*vec4(tilec,0,1)).st); \n"
        "    vec3 hsv = rgb2hsv(c.rgb); \n"
        "    return hsv[1] > oe_GroundCover_colorMinSaturation; \n"
        "} \n"

        "#endif // OE_GROUNDCOVER_COLOR_SAMPLER \n"

        "bool inRange(in vec4 vertex_view) { \n"
        "    float maxRange = oe_GroundCover_maxDistance / oe_Camera.z; \n"
        "    return (-vertex_view.z <= oe_GroundCover_maxDistance); \n"
        "} \n"

        "float getElevation(in vec2 tilec) { \n"        
        "    vec2 elevc = tilec \n"
        "       * oe_tile_elevTexelCoeff.x * oe_tile_elevationTexMatrix[0][0] // scale \n"
        "       + oe_tile_elevTexelCoeff.x * oe_tile_elevationTexMatrix[3].st // bias \n"
        "       + oe_tile_elevTexelCoeff.y; \n"
        "    return texture(oe_tile_elevationTex, elevc).r; \n"
        "} \n"

        "void main() { \n"
        "    const uint x = gl_GlobalInvocationID.x; \n"
        "    const uint y = gl_GlobalInvocationID.y; \n"

        "    vec2 offset = vec2(float(x), float(y)); \n"
        "    vec2 halfSpacing = 0.5 / vec2(gl_NumWorkGroups.xy); \n"
        "    vec2 tilec = halfSpacing + offset / vec2(gl_NumWorkGroups.xy); \n"

        "    vec4 noise = textureLod(oe_GroundCover_noiseTex, tilec, 0); \n"

        "    vec2 shift = vec2(fract(noise[1]*1.5), fract(noise[2]*1.5))*2.0-1.0; \n"
        "    tilec += shift * halfSpacing; \n"

        "#ifdef OE_GROUNDCOVER_COLOR_SAMPLER \n"
        "    if (!isLegalColor(tilec)) \n"
        "         return; \n"
        "#endif \n"

        "    vec4 vertex_model = vec4( \n"
        "        mix(oe_GroundCover_LL.xy, oe_GroundCover_UR.xy, tilec), \n"
        "        getElevation(tilec), 1); \n"

        "    vec4 vertex_view = gl_ModelViewMatrix * vertex_model; \n"

        "    if (!inRange(vertex_view)) \n"
        "        return; \n"

        "    if (!inFrustum(vertex_view)) \n"
        "        return; \n"

        "    uint slot = atomicAdd(cmd[oe_GroundCover_tileNum].instanceCount, 1); \n"
        "    uint start = oe_GroundCover_tileNum * gl_NumWorkGroups.y * gl_NumWorkGroups.x; \n"
        "    render[start + slot].vertex = vertex_model; \n"
        "    render[start + slot].tilec = tilec; \n"
        "} \n";

    const char* oe_IC_renderVS =
        "#version 430 \n"

        "struct RenderData { \n"
        "    vec4 vertex; \n"
        "    vec2 tilec; \n"
        "    vec2 _padding; \n"
        "}; \n"

        "layout(binding=1, std430) buffer RenderBuffer { \n"
        "    RenderData render[]; \n"
        "}; \n"

        "out vec4 oe_layer_tilec; \n"

        "void oe_InstanceCloud_renderVS(inout vec4 vertex) { \n"
        "    vertex = render[gl_InstanceID].vertex; \n"
        "    oe_layer_tilec = vec4(render[gl_InstanceID].tilec, 0, 1); \n"
        "} \n";
}

//...................................................................

InstanceCloud::InstancingData::InstancingData() :
    commands(NULL),
    points(NULL),
    commandBuffer(-1),
    pointsBuffer(-1),
    renderBuffer(-1),
    numTilesAllocated(0u)
{
    //nop
}

InstanceCloud::InstancingData::~InstancingData()
{
    if (commands)
        delete [] commands;
}

void
InstanceCloud::InstancingData::allocateGLObjects(osg::State* state, unsigned numTiles)
{
    if (numTilesAllocated < numTiles)
    {
        OE_DEBUG << LC << "Reallocate: " << numTiles << " tiles" << std::endl;

        osg::GLExtensions* ext = state->get<osg::GLExtensions>();

        numTilesAllocated = numTiles;

        if (commands)
            delete[] commands;
        if (commandBuffer >= 0)
            ext->glDeleteBuffers(1, &commandBuffer);
        if (renderBuffer >= 0)
            ext->glDeleteBuffers(1, &renderBuffer);

        commands = new DrawElementsIndirectCommand[numTilesAllocated];
        for(unsigned i=0; i<numTilesAllocated; ++i)
        {
            commands[i].count = numIndices;
            commands[i].instanceCount = 0;
            commands[i].firstIndex = 0;
            commands[i].baseVertex = 0;
            commands[i].baseInstance = 0;
        }

        GLuint numInstances = numX * numY;
        GLuint instanceSize = sizeof(GLfloat)*8; // vec4 vertex; vec2 tilec; vec2 _padding;

        ext->glGenBuffers(1, &commandBuffer);
        ext->glBindBuffer(GL_SHADER_STORAGE_BUFFER, commandBuffer);
        ext->glBufferStorage(
            GL_SHADER_STORAGE_BUFFER,
            numTilesAllocated * sizeof(DrawElementsIndirectCommand),
            NULL,                    // uninitialized memory
            GL_DYNAMIC_STORAGE_BIT); // so we can reset each frame

        // buffer for the output data (culled points, written by compute shader)
        renderBufferTileSize = numInstances*instanceSize;

        ext->glGenBuffers(1, &renderBuffer);
        ext->glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderBuffer);
        ext->glBufferStorage(
            GL_SHADER_STORAGE_BUFFER, 
            numTilesAllocated * renderBufferTileSize,
            NULL,   // uninitialized memory
            0);     // only GPU will write to this buffer
    }
}

InstanceCloud::InstanceCloud()
{
    // install our compute shader in a stateset
    _computeStateSet = new osg::StateSet();
    osg::Shader* s = new osg::Shader(osg::Shader::COMPUTE, cull_CS);
    osg::Program* p = new osg::Program();
    p->addShader(s);
    _computeStateSet->setAttribute(p, osg::StateAttribute::ON);
}

void
InstanceCloud::setGeometry(osg::Geometry* geom)
{
    _geom = geom;

    if (geom)
    {
        Installer installer(&_data);
        geom->accept(installer);
    }
}

//void
//InstanceCloud::setPositions(osg::Vec4Array* value)
//{
//    _data.points = value;
//}

void
InstanceCloud::setNumInstances(unsigned x, unsigned y)
{
    _data.numX = x;
    _data.numY = y;
}

osg::BoundingBox
InstanceCloud::computeBoundingBox() const
{
    osg::BoundingBox box;
    float radius = _geom.valid() ? _geom->getBound().radius() : 0.0f;
    osg::Vec3 radius3(radius, radius, radius);
    if (_data.points)
    {
        for(int i=0; i<_data.points->size(); ++i)
        {
            const osg::Vec4& v = (*_data.points)[i];
            osg::Vec3 center(v.x()/v.w(), v.y()/v.w(), v.z()/v.w());
            osg::BoundingBox pbox(center-radius3, center+radius3);
            box.expandBy(pbox);
        }
    }
    return box;
}

void
InstanceCloud::allocateGLObjects(osg::RenderInfo& ri, unsigned numTiles)
{
    _data.allocateGLObjects(ri.getState(), numTiles);
}

void
InstanceCloud::preCull(osg::RenderInfo& ri)
{
    osg::State* state = ri.getState();
    osg::GLExtensions* ext = state->get<osg::GLExtensions>();

    if (state->getUseModelViewAndProjectionUniforms()) 
        state->applyModelViewAndProjectionUniformsIfRequired();

    // Reset all the instance counts to zero.
    ext->glBindBuffer(GL_SHADER_STORAGE_BUFFER, _data.commandBuffer);
    ext->glBufferSubData(
        GL_SHADER_STORAGE_BUFFER, 
        0,
        _data.numTilesAllocated * sizeof(DrawElementsIndirectCommand),
        &_data.commands[0]);

    ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING_COMMAND_BUFFER, _data.commandBuffer);

    ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING_RENDER_BUFFER, _data.renderBuffer);
}

void
InstanceCloud::cullTile(osg::RenderInfo& ri, unsigned tileNum)
{
    osg::State* state = ri.getState();
    osg::GLExtensions* ext = state->get<osg::GLExtensions>();

    ext->glDispatchCompute(_data.numX, _data.numY, 1);
}

void
InstanceCloud::postCull(osg::RenderInfo& ri)
{
    //nop
}

void
InstanceCloud::drawTile(osg::RenderInfo& ri, unsigned tileNum)
{
    _data.tileToDraw = tileNum;
    _geom->draw(ri);
}


InstanceCloud::Renderer::Renderer(InstancingData* data) :
    _data(data)
{
    //nop
}

void
InstanceCloud::Renderer::drawImplementation(osg::RenderInfo& ri, const osg::Drawable* drawable) const
{
    osg::State& state = *ri.getState();

    osg::GLExtensions* ext = state.get<osg::GLExtensions>();

    if (_data->tileToDraw == 0)
    {
        osg::VertexArrayState* vas = state.getCurrentVertexArrayState();
        vas->setVertexBufferObjectSupported(true);

        const osg::Geometry* geom = drawable->asGeometry();
        geom->drawVertexArraysImplementation(ri);

        // TODO: support multiple primtsets....?
        osg::GLBufferObject* ebo = geom->getPrimitiveSet(0)->getOrCreateGLBufferObject(state.getContextID());
        state.getCurrentVertexArrayState()->bindElementBufferObject(ebo);

        ext->glBindBufferBase(
            GL_SHADER_STORAGE_BUFFER, 
            BINDING_COMMAND_BUFFER, 
            _data->commandBuffer);

        ext->glBindBuffer(GL_DRAW_INDIRECT_BUFFER, _data->commandBuffer);

        ext->glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    }

    // activate the "nth" tile in the render buffer:
    ext->glBindBufferRange(
        GL_SHADER_STORAGE_BUFFER, 
        BINDING_RENDER_BUFFER,
        _data->renderBuffer, 
        _data->tileToDraw * _data->renderBufferTileSize,
        _data->renderBufferTileSize);

    ext->glDrawElementsIndirect(
        GL_TRIANGLES, 
        GL_UNSIGNED_SHORT,
        (const void*)(_data->tileToDraw * sizeof(DrawElementsIndirectCommand)) );
}


InstanceCloud::Installer::Installer(InstancingData* data) :
    osg::NodeVisitor(TRAVERSE_ALL_CHILDREN),
    _data(data)
{
    _callback = new Renderer(data);
}

void 
InstanceCloud::Installer::apply(osg::Drawable& drawable)
{
    osg::Geometry* geom = drawable.asGeometry();
    if (geom)
    {
        geom->setDrawCallback(_callback.get());
        geom->setCullingActive(false);
        _data->numIndices = geom->getPrimitiveSet(0)->getNumIndices();
    }
}

#if 0

//...................................................................

InstanceCloudDrawable::InstanceCloudDrawable()
{
    setCullingActive(false);
    _cloud = new InstanceCloud();

    VirtualProgram* vp = VirtualProgram::getOrCreate(getOrCreateStateSet());
    //vp->setFunction("oe_IC_renderVS", oe_IC_renderVS, ShaderComp::LOCATION_VERTEX_MODEL, -FLT_MAX);
}

osg::BoundingBox
InstanceCloudDrawable::computeBoundingBox() const
{
    return _cloud->computeBoundingBox();
}

void 
InstanceCloudDrawable::drawImplementation(osg::RenderInfo& ri) const
{
    _cloud->cull(ri);
    _cloud->drawTile(ri, 0u);
}


#include <osgViewer/Viewer>
#include <osgEarth/ExampleResources>
#include <osgEarth/AnnotationUtils>
#include <osgEarth/InstanceCloud>
#include <osgEarth/NodeUtils>

#define DIM 100
#define SPACING 3

osg::Geometry* makeInstance()
{
    return osgEarth::findTopMostNodeOfType<osg::Geometry>(
        AnnotationUtils::createSphere(-0.5, osg::Vec4(1, .5, 0, 1), 15.0));
}

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);
    osgViewer::Viewer viewer(arguments);
    osg::Group* group = new osg::Group();

    osg::Vec4Array* points = new osg::Vec4Array();
    for (int i = 0; i < DIM; ++i)
        for (int j = 0; j < DIM; ++j)
            points->push_back(osg::Vec4(SPACING * (double)i, SPACING * (double)j, 0, 1));

    osg::Geometry* geom = makeInstance();

    InstanceCloudDrawable* drawable = new InstanceCloudDrawable();
    drawable->cloud()->setGeometry(geom);
    drawable->cloud()->setPositions(points);
    group->addChild(drawable);

    MapNodeHelper().parse(NULL, arguments, &viewer, group, (Controls::Container*)NULL);
    MapNodeHelper().configureView(&viewer);

    viewer.getCamera()->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    viewer.getCamera()->setProjectionMatrixAsPerspective(30.0, 1.0, 1.0, 1e6);

    viewer.setSceneData(group);
    return viewer.run();
}

#endif

#endif // OSG 3.6.0+