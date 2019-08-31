/* 
 * Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//-----------------------------------------------------------------------------
//
// redflash: Raymarching x Pathtracer
//
//-----------------------------------------------------------------------------

#ifdef __APPLE__
#  include <GLUT/glut.h>
#else
#  include <GL/glew.h>
#  if defined( _WIN32 )
#    include <GL/wglew.h>
#    include <GL/freeglut.h>
#  else
#    include <GL/glut.h>
#  endif
#endif

#include <optixu/optixpp_namespace.h>
#include <optixu/optixu_math_stream_namespace.h>

#include "redflash.h"
#include <sutil.h>
#include <Arcball.h>
#include <OptiXMesh.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdint.h>
#include <filesystem>

namespace fs = std::experimental::filesystem;

using namespace optix;

const char* const SAMPLE_NAME = "redflash";

//------------------------------------------------------------------------------
//
// Globals
//
//------------------------------------------------------------------------------

Context        context = 0;
uint32_t       width  = 1920 / 4;
uint32_t       height = 1080 / 4;
int max_depth = 10;
int sample_per_launch = 2;
bool           use_pbo = true;

int            frame_number = 1;
int            rr_begin_depth = 1;
Program        pgram_intersection = 0;
Program        pgram_bounding_box = 0;
Program        pgram_intersection_raymarching = 0;
Program        pgram_bounding_box_raymarching = 0;
Program        pgram_intersection_sphere = 0;
Program        pgram_bounding_box_sphere = 0;


// Camera state
float3         camera_up;
float3         camera_lookat;
float3         camera_eye;
Matrix4x4      camera_rotate;
bool           camera_changed = true;
sutil::Arcball arcball;

Matrix4x4 frame;
Matrix4x4 frame_inv;

// Mouse state
int2           mouse_prev_pos;
int            mouse_button;


//------------------------------------------------------------------------------
//
// Forward decls 
//
//------------------------------------------------------------------------------

Buffer getOutputBuffer();
void destroyContext();
void registerExitHandler();
void createContext();
void loadGeometry();
void setupCamera();
void updateCamera();
void glutInitialize( int* argc, char** argv );
void glutRun();

void glutDisplay();
void glutKeyboardPress( unsigned char k, int x, int y );
void glutMousePress( int button, int state, int x, int y );
void glutMouseMotion( int x, int y);
void glutResize( int w, int h );


//------------------------------------------------------------------------------
//
//  Helper functions
//
//------------------------------------------------------------------------------

std::string resolveDataPath(const char* filename)
{
    std::vector<std::string> source_locations;

    std::string base_dir = std::string(sutil::samplesDir());

    // Potential source locations (in priority order)
    source_locations.push_back(fs::current_path().string() + "/" + filename);
    source_locations.push_back(fs::current_path().string() + "/data/" + filename);
    source_locations.push_back(base_dir + "/data/" + filename);

    for (std::vector<std::string>::const_iterator it = source_locations.begin(); it != source_locations.end(); ++it) {
        std::cout << "[info] resolvePath source_location: " + *it << std::endl;

        // Try to get source code from file
        if (fs::exists(*it))
        {
            return *it;
        }
    }

    // Wasn't able to find or open the requested file
    throw Exception("Couldn't open source file " + std::string(filename));
}

Buffer getOutputBuffer()
{
    return context[ "output_buffer" ]->getBuffer();
}


void destroyContext()
{
    if( context )
    {
        context->destroy();
        context = 0;
    }
}


void registerExitHandler()
{
    // register shutdown handler
#ifdef _WIN32
    glutCloseFunc( destroyContext );  // this function is freeglut-only
#else
    atexit( destroyContext );
#endif
}


void setMaterial(
        GeometryInstance& gi,
        Material material,
        const std::string& color_name,
        const float3& color)
{
    gi->addMaterial(material);
    gi[color_name]->setFloat(color);
}

GeometryInstance createRaymrachingObject(const float3& center, const float3& world_scale, const float3& unit_scale)
{
    Geometry raymarching = context->createGeometry();
    raymarching->setPrimitiveCount(1u);
    raymarching->setIntersectionProgram(pgram_intersection_raymarching);
    raymarching->setBoundingBoxProgram(pgram_bounding_box_raymarching);

    const float3 local_scale = world_scale / unit_scale;
    raymarching["center"]->setFloat(center);
    raymarching["local_scale"]->setFloat(local_scale);
    raymarching["aabb_min"]->setFloat(center - world_scale);
    raymarching["aabb_max"]->setFloat(center + world_scale);

    GeometryInstance gi = context->createGeometryInstance();
    gi->setGeometry(raymarching);
    return gi;
}

GeometryInstance createSphereObject(const float3& center, const float radius)
{
    Geometry sphere = context->createGeometry();
    sphere->setPrimitiveCount(1u);
    sphere->setIntersectionProgram(pgram_intersection_sphere);
    sphere->setBoundingBoxProgram(pgram_bounding_box_sphere);

    sphere["center"]->setFloat(center);
    sphere["radius"]->setFloat(radius);
    sphere["aabb_min"]->setFloat(center - radius);
    sphere["aabb_max"]->setFloat(center + radius);

    GeometryInstance gi = context->createGeometryInstance();
    gi->setGeometry(sphere);
    return gi;
}

GeometryInstance createMesh(
    const std::string& filename,
    Material material,
    Program closest_hit,
    Program any_hit,
    const float3& center,
    const float3& scale)
{
    OptiXMesh mesh;
    mesh.context = context;
    mesh.use_tri_api = true;
    mesh.ignore_mats = false;
    mesh.material = material;
    mesh.closest_hit = closest_hit;
    mesh.any_hit = any_hit;
    Matrix4x4 mat =Matrix4x4::translate(center) *  Matrix4x4::scale(scale);// 行優先っぽいので、右から順番に適用される
    loadMesh(filename, mesh, mat);
    return mesh.geom_instance;
}

void createContext()
{
    context = Context::create();
    context->setRayTypeCount( 2 );
    context->setEntryPointCount( 1 );
    context->setStackSize( 1800 );
    context->setMaxTraceDepth( 2 );

    context[ "scene_epsilon"                  ]->setFloat( 0.001f );
    context[ "rr_begin_depth"                 ]->setUint( rr_begin_depth );
    context["max_depth"]->setUint(max_depth);
    context["sample_per_launch"]->setUint(sample_per_launch);

    Buffer buffer = sutil::createOutputBuffer( context, RT_FORMAT_FLOAT4, width, height, use_pbo );
    context["output_buffer"]->set( buffer );

    // Setup programs
    const char *ptx = sutil::getPtxString( SAMPLE_NAME, "redflash.cu" );
    context->setRayGenerationProgram( 0, context->createProgramFromPTXString( ptx, "pathtrace_camera" ) );
    context->setExceptionProgram( 0, context->createProgramFromPTXString( ptx, "exception" ) );
    context->setMissProgram( 0, context->createProgramFromPTXString( ptx, "envmap_miss" ) );

    context[ "bad_color"        ]->setFloat( 1000000.0f, 0.0f, 1000000.0f ); // Super magenta to make sure it doesn't get averaged out in the progressive rendering.

    const float3 default_color = make_float3(1.0f, 1.0f, 1.0f);
    const std::string texpath = resolveDataPath("GrandCanyon_C_YumaPoint/GCanyon_C_YumaPoint_3k.hdr");
    // const std::string texpath = resolveDataPath("Ice_Lake/Ice_Lake_Ref.hdr");
    // const std::string texpath = resolveDataPath("Desert_Highway/Road_to_MonumentValley_Env.hdr");
    context["envmap"]->setTextureSampler(sutil::loadTexture(context, texpath, default_color));
}

GeometryGroup createGeometryTriangles()
{
    // Set up material
    Material diffuse = context->createMaterial();
    const char *ptx = sutil::getPtxString(SAMPLE_NAME, "redflash.cu");
    Program diffuse_ch = context->createProgramFromPTXString(ptx, "closest_hit");
    Program diffuse_ah = context->createProgramFromPTXString(ptx, "shadow");
    diffuse->setClosestHitProgram(0, diffuse_ch);
    diffuse->setAnyHitProgram(1, diffuse_ah);

    std::vector<GeometryInstance> gis;
    const float3 color = make_float3(0.9f, 0.1f, 0.1f);

    // Mesh
    std::string mesh_file = resolveDataPath("cow.obj");
    gis.push_back(createMesh(mesh_file, diffuse, diffuse_ch, diffuse_ah, make_float3(0.0f, 300.0f, 0.0f), make_float3(500.0f)));
    gis.back()["albedo_color"]->setFloat(color);

    GeometryGroup shadow_group = context->createGeometryGroup(gis.begin(), gis.end());
    shadow_group->setAcceleration(context->createAcceleration("Trbvh"));
    return shadow_group;
}

GeometryGroup createGeometry()
{
    // Set up material
    Material diffuse = context->createMaterial();
    const char *ptx = sutil::getPtxString( SAMPLE_NAME, "redflash.cu" );
    Program diffuse_ch = context->createProgramFromPTXString( ptx, "closest_hit" );
    Program diffuse_ah = context->createProgramFromPTXString( ptx, "shadow" );
    diffuse->setClosestHitProgram( 0, diffuse_ch );
    diffuse->setAnyHitProgram( 1, diffuse_ah );

    // Set up Raymarching programs
    ptx = sutil::getPtxString(SAMPLE_NAME, "intersect_raymarching.cu");
    pgram_bounding_box_raymarching = context->createProgramFromPTXString(ptx, "bounds");
    pgram_intersection_raymarching = context->createProgramFromPTXString(ptx, "intersect");

    // Set up Sphere programs
    ptx = sutil::getPtxString(SAMPLE_NAME, "intersect_sphere.cu");
    pgram_bounding_box_sphere = context->createProgramFromPTXString(ptx, "bounds");
    pgram_intersection_sphere = context->createProgramFromPTXString(ptx, "sphere_intersect");

    // create geometry instances
    std::vector<GeometryInstance> gis;

    const float3 white = make_float3( 0.8f, 0.8f, 0.8f );
    const float3 gray  = make_float3( 0.3f, 0.3f, 0.3f );
    const float3 green = make_float3( 0.05f, 0.8f, 0.05f );
    const float3 red   = make_float3( 0.8f, 0.05f, 0.05f );

    // Raymarcing
    gis.push_back(createRaymrachingObject(
        make_float3(0.0f),
        make_float3(300.0f),
        make_float3(4.3f)));
    setMaterial(gis.back(), diffuse, "albedo_color", white);

    // Sphere
    gis.push_back(createSphereObject(
        make_float3(0.0f, 310.0f, 50.0f), 10.0f));
    setMaterial(gis.back(), diffuse, "albedo_color", green);
    //gis.back()["emission_color"]->setFloat(make_float3(1.0));

    // Create shadow group (no light)
    GeometryGroup shadow_group = context->createGeometryGroup(gis.begin(), gis.end());
    shadow_group->setAcceleration( context->createAcceleration( "Trbvh" ) );
    return shadow_group;
}

optix::Buffer m_bufferLightParameters;

void updateLightParameters(const std::vector<LightParameter> &lightParameters)
{
    LightParameter* dst = static_cast<LightParameter*>(m_bufferLightParameters->map(0, RT_BUFFER_MAP_WRITE_DISCARD));
    for (size_t i = 0; i < lightParameters.size(); ++i, ++dst) {
        LightParameter mat = lightParameters[i];

        dst->position = mat.position;
        dst->emission = mat.emission;
        dst->radius = mat.radius;
        dst->area = mat.area;
        dst->u = mat.u;
        dst->v = mat.v;
        dst->normal = mat.normal;
        dst->lightType = mat.lightType;
    }
    m_bufferLightParameters->unmap();
}

GeometryGroup createGeometryLight()
{
    // Set up material
    const char *ptx = sutil::getPtxString(SAMPLE_NAME, "redflash.cu");
    Material diffuse_light = context->createMaterial();
    Program diffuse_em = context->createProgramFromPTXString(ptx, "light_closest_hit");
    diffuse_light->setClosestHitProgram(0, diffuse_em);

    // Light
    std::vector<LightParameter> lightParameters;
    std::vector<GeometryInstance> gis;

    {
        LightParameter light;
        light.lightType = SPHERE;
        light.position = make_float3(50, 310, 50);
        light.radius = 10.0f;
        light.emission = make_float3(1.0);
        lightParameters.push_back(light);
    }

    {
        LightParameter light;
        light.lightType = SPHERE;
        light.position = make_float3(0.01f, 166.787f, 190.00f);
        light.radius = 2.0f;
        light.emission = make_float3(10.0f, 0.01f, 0.01f);
        lightParameters.push_back(light);
    }

    int index = 0;
    for (auto light = lightParameters.begin(); light != lightParameters.end(); ++light)
    {
        light->area = 4.0f * M_PIf * light->radius * light->radius;
        light->normal = optix::normalize(light->normal);

        gis.push_back(createSphereObject(light->position, light->radius));
        setMaterial(gis.back(), diffuse_light, "emission_color", light->emission);
        gis.back()["lightMaterialId"]->setInt(index);
        ++index;
    }

    // Create geometry group
    GeometryGroup light_group = context->createGeometryGroup(gis.begin(), gis.end());
    light_group->setAcceleration(context->createAcceleration("Trbvh"));

    // Create sysLightParameters
    m_bufferLightParameters = context->createBuffer(RT_BUFFER_INPUT, RT_FORMAT_USER);
    m_bufferLightParameters->setElementSize(sizeof(LightParameter));
    m_bufferLightParameters->setSize(lightParameters.size());
    updateLightParameters(lightParameters);
    context["sysNumberOfLights"]->setInt(lightParameters.size());
    context["sysLightParameters"]->setBuffer(m_bufferLightParameters);

    return light_group;
}

void setupScene()
{
    // Create a GeometryGroup for the GeometryTriangles instances and a separate
    // GeometryGroup for all other primitives.
    GeometryGroup tri_gg = createGeometryTriangles();
    GeometryGroup gg = createGeometry();
    GeometryGroup light_gg = createGeometryLight();

    // Create a top-level Group to contain the two GeometryGroups.
    Group top_group = context->createGroup();
    top_group->setAcceleration(context->createAcceleration("Trbvh"));
    top_group->addChild(gg);
    top_group->addChild(tri_gg);
    context["top_shadower"]->set(top_group);

    Group top_group_light = context->createGroup();
    top_group_light->setAcceleration(context->createAcceleration("Trbvh"));
    top_group_light->addChild(gg);
    top_group_light->addChild(tri_gg);
    top_group_light->addChild(light_gg);
    context["top_object"]->set(top_group_light);
}

void setupCamera()
{
    camera_up = make_float3(0.0f, 1.0f, 0.0f);

    // look at emission
    camera_eye = make_float3(50.4f, 338.1f, -66.82f);
    camera_lookat = make_float3(48.49f, 311.32f, 21.44f);

    // look at center
    camera_eye = make_float3(13.91f, 166.787f, 413.00f);
    camera_lookat = make_float3(-6.59f, 169.94f, -9.11f);

    camera_rotate  = Matrix4x4::identity();
}


void updateCamera()
{
    const float fov  = 35.0f;
    const float aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
    
    float3 camera_u, camera_v, camera_w;
    sutil::calculateCameraVariables(
            camera_eye, camera_lookat, camera_up, fov, aspect_ratio,
            camera_u, camera_v, camera_w, /*fov_is_vertical*/ true );

    frame = Matrix4x4::fromBasis(
            normalize( camera_u ),
            normalize( camera_v ),
            normalize( -camera_w ),
            camera_lookat);
    frame_inv = frame.inverse();
    // Apply camera rotation twice to match old SDK behavior
    const Matrix4x4 trans     = frame*camera_rotate*camera_rotate*frame_inv; 

    camera_eye    = make_float3( trans*make_float4( camera_eye,    1.0f ) );
    camera_lookat = make_float3( trans*make_float4( camera_lookat, 1.0f ) );
    // camera_up     = make_float3( trans*make_float4( camera_up,     0.0f ) );

    sutil::calculateCameraVariables(
            camera_eye, camera_lookat, camera_up, fov, aspect_ratio,
            camera_u, camera_v, camera_w, true );

    camera_rotate = Matrix4x4::identity();

    if( camera_changed ) // reset accumulation
        frame_number = 1;
    camera_changed = false;

    context[ "frame_number" ]->setUint( frame_number++ );
    context[ "eye"]->setFloat( camera_eye );
    context[ "U"  ]->setFloat( camera_u );
    context[ "V"  ]->setFloat( camera_v );
    context[ "W"  ]->setFloat( camera_w );

}


void glutInitialize( int* argc, char** argv )
{
    glutInit( argc, argv );
    glutInitDisplayMode( GLUT_RGB | GLUT_ALPHA | GLUT_DEPTH | GLUT_DOUBLE );
    glutInitWindowSize( width, height );
    glutInitWindowPosition( 100, 100 );                                               
    glutCreateWindow( SAMPLE_NAME );
    glutHideWindow();                                                              
}


void glutRun()
{
    // Initialize GL state                                                            
    glMatrixMode(GL_PROJECTION);                                                   
    glLoadIdentity();                                                              
    glOrtho(0, 1, 0, 1, -1, 1 );                                                   

    glMatrixMode(GL_MODELVIEW);                                                    
    glLoadIdentity();                                                              

    glViewport(0, 0, width, height);                                 

    glutShowWindow();                                                              
    glutReshapeWindow( width, height);

    // register glut callbacks
    glutDisplayFunc( glutDisplay );
    glutIdleFunc( glutDisplay );
    glutReshapeFunc( glutResize );
    glutKeyboardFunc( glutKeyboardPress );
    glutMouseFunc( glutMousePress );
    glutMotionFunc( glutMouseMotion );

    registerExitHandler();

    glutMainLoop();
}


//------------------------------------------------------------------------------
//
//  GLUT callbacks
//
//------------------------------------------------------------------------------

void glutDisplay()
{
    updateCamera();
    context->launch( 0, width, height );

    sutil::displayBufferGL( getOutputBuffer() );

    {
      static unsigned frame_count = 0;
      sutil::displayFps( frame_count++ );
    }

    {
        static char frame_number_text[32];
        sprintf(frame_number_text, "frame_number:   %d", frame_number);
        sutil::displayText(frame_number_text, 10, 80);
    }

    {
        static char camera_eye_text[32];
        sprintf(camera_eye_text, "camera_eye:    %7.2f, %7.2f, %7.2f", camera_eye.x, camera_eye.y, camera_eye.z);
        sutil::displayText(camera_eye_text, 10, 60);
    }

    {
        static char camera_lookat_text[32];
        sprintf(camera_lookat_text, "camera_lookat: %7.2f, %7.2f, %7.2f", camera_lookat.x, camera_lookat.y, camera_lookat.z);
        sutil::displayText(camera_lookat_text, 10, 40);
    }

    glutSwapBuffers();
}


void glutKeyboardPress( unsigned char k, int x, int y )
{

    switch( k )
    {
        case( 'q' ):
        case( 27 ): // ESC
        {
            destroyContext();
            exit(0);
        }
        case( 's' ):
        {
            const std::string outputImage = std::string(SAMPLE_NAME) + ".png";
            std::cerr << "Saving current frame to '" << outputImage << "'\n";
            sutil::displayBufferPNG( outputImage.c_str(), getOutputBuffer(), false );
            break;
        }
    }
}


void glutMousePress( int button, int state, int x, int y )
{
    if( state == GLUT_DOWN )
    {
        mouse_button = button;
        mouse_prev_pos = make_int2( x, y );
    }
    else
    {
        // nothing
    }
}


void glutMouseMotion( int x, int y)
{
    if( mouse_button == GLUT_RIGHT_BUTTON )
    {
        const float dx = static_cast<float>( x - mouse_prev_pos.x ) /
                         static_cast<float>( width );
        const float dy = static_cast<float>( y - mouse_prev_pos.y ) /
                         static_cast<float>( height );
        const float dmax = fabsf( dx ) > fabs( dy ) ? dx : dy;
        const float scale = std::min<float>( dmax, 0.9f );
        camera_eye = camera_eye + (camera_lookat - camera_eye)*scale;
        camera_changed = true;
    }
    else if( mouse_button == GLUT_LEFT_BUTTON )
    {
        const float2 from = { static_cast<float>(mouse_prev_pos.x),
                              static_cast<float>(mouse_prev_pos.y) };
        const float2 to   = { static_cast<float>(x),
                              static_cast<float>(y) };

        const float2 a = { from.x / width, from.y / height };
        const float2 b = { to.x   / width, to.y   / height };

        camera_rotate = arcball.rotate( b, a );
        camera_changed = true;
    }
    else if (mouse_button == GLUT_MIDDLE_BUTTON)
    {
        const float dx = static_cast<float>(x - mouse_prev_pos.x) /
            static_cast<float>(width);
        const float dy = static_cast<float>(y - mouse_prev_pos.y) /
            static_cast<float>(height);
        float4 offset = { -dx, dy, 0, 0 };
        offset = frame * offset;
        float3 offset_v3 = { offset.x, offset.y, offset.z };
        offset_v3 *= 200;
        camera_eye += offset_v3;
        camera_lookat += offset_v3;
        camera_changed = true;
    }

    mouse_prev_pos = make_int2( x, y );
}


void glutResize( int w, int h )
{
    if ( w == (int)width && h == (int)height ) return;

    camera_changed = true;

    width  = w;
    height = h;
    sutil::ensureMinimumSize(width, height);

    sutil::resizeBuffer( getOutputBuffer(), width, height );

    glViewport(0, 0, width, height);                                               

    glutPostRedisplay();
}


//------------------------------------------------------------------------------
//
// Main
//
//------------------------------------------------------------------------------

void printUsageAndExit( const std::string& argv0 )
{
    std::cerr << "\nUsage: " << argv0 << " [options]\n";
    std::cerr <<
        "App Options:\n"
        "  -h | --help               Print this usage message and exit.\n"
        "  -f | --file               Save single frame to file and exit.\n"
        "  -n | --nopbo              Disable GL interop for display buffer.\n"
        "  -s | --sample             Sample number.\n"
        "  -t | --time               Time limit(ssc).\n"
        "App Keystrokes:\n"
        "  q  Quit\n" 
        "  s  Save image to '" << SAMPLE_NAME << ".png'\n"
        << std::endl;

    exit(1);
}


int main( int argc, char** argv )
 {
    double launch_time = sutil::currentTime();

    std::string out_file;
    int sample = 20;
    double time_limit = 60 * 60;// 1 hour
    bool use_time_limit = false;

    for( int i=1; i<argc; ++i )
    {
        const std::string arg( argv[i] );

        if( arg == "-h" || arg == "--help" )
        {
            printUsageAndExit( argv[0] );
        }
        else if( arg == "-f" || arg == "--file"  )
        {
            if( i == argc-1 )
            {
                std::cerr << "Option '" << arg << "' requires additional argument.\n";
                printUsageAndExit( argv[0] );
            }
            out_file = argv[++i];
            use_pbo = false;
        }
        else if( arg == "-n" || arg == "--nopbo"  )
        {
            use_pbo = false;
        }
        else if (arg == "-s" || arg == "--sample")
        {
            if (i == argc - 1)
            {
                std::cerr << "Option '" << arg << "' requires additional argument.\n";
                printUsageAndExit(argv[0]);
            }
            sample = atoi(argv[++i]);
        }
        else if (arg == "-t" || arg == "--time")
        {
            if (i == argc - 1)
            {
                std::cerr << "Option '" << arg << "' requires additional argument.\n";
                printUsageAndExit(argv[0]);
            }
            time_limit = atof(argv[++i]);
            use_time_limit = true;
        }
        else if (arg == "-W" || arg == "--width")
        {
            if (i == argc - 1)
            {
                std::cerr << "Option '" << arg << "' requires additional argument.\n";
                printUsageAndExit(argv[0]);
            }
            width = atoi(argv[++i]);
        }
        else if (arg == "-H" || arg == "--height")
        {
            if (i == argc - 1)
            {
                std::cerr << "Option '" << arg << "' requires additional argument.\n";
                printUsageAndExit(argv[0]);
            }
            height = atoi(argv[++i]);
        }
        else
        {
            std::cerr << "Unknown option '" << arg << "'\n";
            printUsageAndExit( argv[0] );
        }
    }

    try
    {
        if ( use_pbo && out_file.empty() ) {
            glutInitialize(&argc, argv);

#ifndef __APPLE__
            glewInit();
#endif
        }

        createContext();
        setupCamera();
        setupScene();

        context->validate();

        if ( out_file.empty() )
        {
            glutRun();
        }
        else
        {
            updateCamera();

            // print config
            std::cout << "resolution: " << width << "x" << height << " px" << std::endl;
            std::cout << "time_limit: " << time_limit << " sec." << std::endl;

            if (use_time_limit)
            {
                std::cout << "sample: INF(" << sample << ")" << std::endl;
            }
            else
            {
                std::cout << "sample: " << sample << std::endl;
            }

            double last_time = sutil::currentTime();

            // NOTE: time_limit が指定されていたら、サンプル数は無制限にする
            for (int i = 0; i < sample || use_time_limit; ++i)
            {
                double now = sutil::currentTime();
                double used_time = now - launch_time;
                double delta_time = now - last_time;
                last_time = now;

                // NOTE: 前フレームの所要時間から次のフレームが制限時間内に終るかを予測する。時間超過を防ぐために1.1倍に見積もる
                if (used_time + delta_time * 1.1 > time_limit)
                {
                    std::cout << "reached time limit! used_time: " << used_time << " sec. remain_time: " << (time_limit - used_time) << " sec." << std::endl;
                    std::cout << "sampled: " << i << std::endl;
                    break;
                }

                context->launch(0, width, height);
                context["frame_number"]->setUint(frame_number++);
            }

            sutil::displayBufferPNG(out_file.c_str(), getOutputBuffer(), false);
            destroyContext();

            double finish_time = sutil::currentTime();
            double total_time = finish_time - launch_time;
            std::cout << "total_time: " << total_time << " sec." << std::endl;
        }

        return 0;
    }
    SUTIL_CATCH( context->get() )
}

