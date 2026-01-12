#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <cstring>
#include <cmath>

MYMOD(net.gtasa.ssao_complete, GTA SA Complete SSAO, 1.0, YourName)
NEEDGAME(com.rockstargames.gtasa)

// ============================================================================
// RENDERWARE STRUCTURES
// ============================================================================

struct RwV2D {
    float x, y;
};

struct RwV3D {
    float x, y, z;
};

struct RwMatrix {
    RwV3D right;
    unsigned int flags;
    RwV3D up;
    unsigned int pad1;
    RwV3D at;
    unsigned int pad2;
    RwV3D pos;
    unsigned int pad3;
};

struct RwRaster {
    RwRaster* parent;
    unsigned char* pixels;
    unsigned char* palette;
    int width, height, depth;
    int stride;
    short u, v;
    unsigned char type;
    unsigned char flags;
    unsigned char format;
    int origWidth, origHeight;
    void* dbEntry;
    unsigned short privateFlags;
};

struct RwObjectFrame {
    void* object[2];
    void* lFrame[2];
    void* callback;
};

struct RwCameraFrustum {
    float plane[4];
    unsigned char x, y, z;
    unsigned char pad;
};

struct RwCamera {
    RwObjectFrame object;
    unsigned short type;
    unsigned short pad1;
    void* preCallback;
    void* postCallback;
    RwMatrix matrix;
    RwRaster* bufferColor;
    RwRaster* bufferDepth;
    RwV2D screen;
    RwV2D screenInverse;
    RwV2D screenOffset;
    float nearplane;
    float farplane;
    float fog;
    float zScale;
    float zShift;
    RwCameraFrustum frustum4D[6];
};

// ============================================================================
// GAME FUNCTION POINTERS
// ============================================================================

typedef void (*_rwCameraValRender_t)(RwCamera*);
typedef float* (*GetCurrentViewMatrix_t)();
typedef float* (*GetCurrentProjectionMatrix_t)();
typedef void (*SetupScreenSpaceProjection_t)();
typedef int (*RwRasterLock_t)(RwRaster*, unsigned char, int);
typedef int (*RwRasterUnlock_t)(RwRaster*);
typedef int (*RwRasterCreate_t)(int, int, int, int);
typedef int (*RwRasterDestroy_t)(RwRaster*);
typedef void* (*RwMatrixInvert_t)(void*, const void*);
typedef void* (*RwV3dTransformPoint_t)(RwV3D*, const RwV3D*, const void*);
typedef float (*_rwOpenGLGetEngineZBufferDepth_t)();
typedef int (*RwRasterShowRaster_t)(RwRaster*, void*, unsigned int);

_rwCameraValRender_t _rwCameraValRender_orig = nullptr;
GetCurrentViewMatrix_t GetCurrentViewMatrix = nullptr;
GetCurrentProjectionMatrix_t GetCurrentProjectionMatrix = nullptr;
SetupScreenSpaceProjection_t SetupScreenSpaceProjection = nullptr;
RwRasterLock_t RwRasterLock = nullptr;
RwRasterUnlock_t RwRasterUnlock = nullptr;
RwRasterCreate_t RwRasterCreate = nullptr;
RwRasterDestroy_t RwRasterDestroy = nullptr;
RwMatrixInvert_t RwMatrixInvert = nullptr;
RwV3dTransformPoint_t RwV3dTransformPoint = nullptr;
_rwOpenGLGetEngineZBufferDepth_t _rwOpenGLGetEngineZBufferDepth = nullptr;
RwRasterShowRaster_t RwRasterShowRaster = nullptr;

// Global pointers
RwRaster** g_pZBuffer = nullptr;
unsigned char** g_rwRaster_cpPixels = nullptr;
void* g_TheCamera = nullptr;

// ============================================================================
// CONFIG
// ============================================================================

ConfigEntry* pEnabled;
ConfigEntry* pSamples;
ConfigEntry* pRadius;
ConfigEntry* pDensity;
ConfigEntry* pBlurEnabled;
ConfigEntry* pBlurRadius;
ConfigEntry* pDebugMode;
ConfigEntry* pResolutionScale; // 1.0 = full res, 0.5 = half res

// ============================================================================
// OPENGL STATE
// ============================================================================

GLuint aoProgram = 0;
GLuint blurProgram = 0;
GLuint compositeProgram = 0;

GLuint aoFBO = 0, blurFBO = 0, compositeFBO = 0;
GLuint aoTexture = 0, blurTexture = 0;
GLuint depthTexture = 0;
GLuint sceneTexture = 0; // Copy of scene before AO

GLuint quadVAO = 0, quadVBO = 0;

struct AOUniforms {
    GLint depthTex;
    GLint viewMatrix, projMatrix;
    GLint invViewMatrix, invProjMatrix;
    GLint nearPlane, farPlane;
    GLint screenSize;
    GLint samples, radius, density;
} aoUniforms;

struct BlurUniforms {
    GLint aoTex, depthTex;
    GLint direction;
    GLint radius;
    GLint screenSize;
} blurUniforms;

struct CompositeUniforms {
    GLint sceneTex, aoTex;
    GLint debugMode;
} compositeUniforms;

float quadVertices[] = {
    -1.0f,  1.0f,  0.0f, 1.0f,
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f
};

// ============================================================================
// SHADER: AO COMPUTATION
// ============================================================================

const char* aoVertShader = R"(
#version 300 es
precision highp float;

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

const char* aoFragShader = R"(
#version 300 es
precision highp float;

in vec2 vTexCoord;
out float FragColor;

uniform sampler2D uDepthTex;

uniform mat4 uViewMatrix;
uniform mat4 uProjMatrix;
uniform mat4 uInvViewMatrix;
uniform mat4 uInvProjMatrix;

uniform float uNearPlane;
uniform float uFarPlane;
uniform vec2 uScreenSize;

uniform float uSamples;
uniform float uRadius;
uniform float uDensity;

// Linearize depth
float linearizeDepth(float depth) {
    float z = depth * 2.0 - 1.0;
    return (2.0 * uNearPlane * uFarPlane) / (uFarPlane + uNearPlane - z * (uFarPlane - uNearPlane));
}

// Reconstruct view space position
vec3 getViewPosition(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = uInvProjMatrix * ndc;
    viewPos /= viewPos.w;
    return viewPos.xyz;
}

// Reconstruct world position
vec3 getWorldPosition(vec2 uv, float depth) {
    vec3 viewPos = getViewPosition(uv, depth);
    vec4 worldPos = uInvViewMatrix * vec4(viewPos, 1.0);
    return worldPos.xyz;
}

// Compute normal from depth derivatives
vec3 computeNormal(vec2 uv, float depth) {
    vec2 texelSize = 1.0 / uScreenSize;
    
    float depthL = texture(uDepthTex, uv + vec2(-texelSize.x, 0.0)).r;
    float depthR = texture(uDepthTex, uv + vec2( texelSize.x, 0.0)).r;
    float depthU = texture(uDepthTex, uv + vec2(0.0,  texelSize.y)).r;
    float depthD = texture(uDepthTex, uv + vec2(0.0, -texelSize.y)).r;
    
    vec3 posC = getViewPosition(uv, depth);
    vec3 posL = getViewPosition(uv + vec2(-texelSize.x, 0.0), depthL);
    vec3 posR = getViewPosition(uv + vec2( texelSize.x, 0.0), depthR);
    vec3 posU = getViewPosition(uv + vec2(0.0,  texelSize.y), depthU);
    vec3 posD = getViewPosition(uv + vec2(0.0, -texelSize.y), depthD);
    
    vec3 ddx = (posR - posL);
    vec3 ddy = (posU - posD);
    
    vec3 normal = normalize(cross(ddy, ddx));
    return normal;
}

// SAO Algorithm (from _AO.fx)
float computeAO(vec3 worldPos, float curDepth, vec3 normal) {
    if(curDepth >= 0.9999) return 1.0;
    
    float d0 = curDepth;
    float linDepth = 1.0 * 0.05 / (1.0 + d0 * (0.05 - 1.0));
    
    vec3 pos = worldPos;
    vec3 n = normal;
    
    float ao = 0.0;
    float sr = -2.5;
    float samples = uSamples;
    float d = uRadius / (samples * (n.z + sr));
    
    vec2 dir = vec2(sin(42.528), cos(42.528)) * d;
    
    float radius = linDepth;
    float aoRadius = 0.5 * linDepth;
    aoRadius = 1.0 - aoRadius;
    aoRadius = min(aoRadius, 0.14);
    radius = mix(0.7 * aoRadius, 0.16 * aoRadius, radius);
    
    mat2 rot = mat2(0.76465, -0.64444, 0.64444, 0.76465);
    
    for(float i = 1.0; i < samples; i += 1.0) {
        vec2 uv = vTexCoord + (dir * i / 0.5) * radius;
        
        if(uv.x > 1.0 || uv.x < 0.0 || uv.y > 1.0 || uv.y < 0.0)
            break;
        
        float sampleDepth = texture(uDepthTex, uv).r;
        vec3 sampleWorldPos = getWorldPosition(uv, sampleDepth);
        
        vec3 occlusionVec = sampleWorldPos - pos;
        float dist = length(occlusionVec);
        occlusionVec = normalize(occlusionVec) * 2.0;
        
        float aoValue = clamp(-dot(n, occlusionVec), 0.0, 1.0);
        
        ao += 3.0 * (aoValue * (1.0 - clamp(dist / 2.0, 0.0, 1.0))) * 
              clamp(1.0 - abs(sampleDepth - curDepth) * 100.0, 0.0, 1.0);
        
        dir = rot * dir;
    }
    
    return pow(1.0 - pow(ao / samples, 1.0) * uDensity, 1.0);
}

void main() {
    float depth = texture(uDepthTex, vTexCoord).r;
    
    if(depth >= 0.9999) {
        FragColor = 1.0;
        return;
    }
    
    vec3 worldPos = getWorldPosition(vTexCoord, depth);
    vec3 normal = computeNormal(vTexCoord, depth);
    
    float ao = computeAO(worldPos, depth, normal);
    
    FragColor = ao;
}
)";

// ============================================================================
// SHADER: BILATERAL BLUR
// ============================================================================

const char* blurFragShader = R"(
#version 300 es
precision highp float;

in vec2 vTexCoord;
out float FragColor;

uniform sampler2D uAOTex;
uniform sampler2D uDepthTex;
uniform int uDirection;
uniform float uRadius;
uniform vec2 uScreenSize;

const float BLUR_SHARPNESS = 50.0;
const float BLUR_FALLOFF = 1.0 / (2.0 * 2.0); // 1/(2*sigma^2)

float gaussian(float x, float sigma) {
    return exp(-(x * x) * BLUR_FALLOFF);
}

void main() {
    vec2 texelSize = 1.0 / uScreenSize;
    vec2 dir = (uDirection == 0) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    
    float centerDepth = texture(uDepthTex, vTexCoord).r;
    float centerAO = texture(uAOTex, vTexCoord).r;
    
    if(centerDepth >= 0.9999) {
        FragColor = 1.0;
        return;
    }
    
    float totalWeight = gaussian(0.0, uRadius);
    float totalAO = centerAO * totalWeight;
    
    int radius = int(uRadius);
    for(int i = 1; i <= radius; i++) {
        vec2 offset = dir * texelSize * float(i);
        
        // Positive direction
        vec2 uvPos = vTexCoord + offset;
        if(uvPos.x >= 0.0 && uvPos.x <= 1.0 && uvPos.y >= 0.0 && uvPos.y <= 1.0) {
            float sampleDepth = texture(uDepthTex, uvPos).r;
            float sampleAO = texture(uAOTex, uvPos).r;
            
            float depthDiff = abs(centerDepth - sampleDepth);
            float weight = gaussian(float(i), uRadius) * exp(-depthDiff * BLUR_SHARPNESS);
            
            totalAO += sampleAO * weight;
            totalWeight += weight;
        }
        
        // Negative direction
        vec2 uvNeg = vTexCoord - offset;
        if(uvNeg.x >= 0.0 && uvNeg.x <= 1.0 && uvNeg.y >= 0.0 && uvNeg.y <= 1.0) {
            float sampleDepth = texture(uDepthTex, uvNeg).r;
            float sampleAO = texture(uAOTex, uvNeg).r;
            
            float depthDiff = abs(centerDepth - sampleDepth);
            float weight = gaussian(float(i), uRadius) * exp(-depthDiff * BLUR_SHARPNESS);
            
            totalAO += sampleAO * weight;
            totalWeight += weight;
        }
    }
    
    FragColor = totalAO / totalWeight;
}
)";

// ============================================================================
// SHADER: COMPOSITE
// ============================================================================

const char* compositeFragShader = R"(
#version 300 es
precision highp float;

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uSceneTex;
uniform sampler2D uAOTex;
uniform int uDebugMode; // 0=normal, 1=AO only, 2=split screen

void main() {
    vec3 sceneColor = texture(uSceneTex, vTexCoord).rgb;
    float ao = texture(uAOTex, vTexCoord).r;
    
    if(uDebugMode == 1) {
        // AO only
        FragColor = vec4(ao, ao, ao, 1.0);
    } else if(uDebugMode == 2) {
        // Split screen
        if(vTexCoord.x < 0.5) {
            FragColor = vec4(sceneColor, 1.0);
        } else {
            FragColor = vec4(sceneColor * ao, 1.0);
        }
    } else {
        // Normal composite
        FragColor = vec4(sceneColor * ao, 1.0);
    }
}
)";

// ============================================================================
// SHADER COMPILATION
// ============================================================================

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if(!success) {
        char log[1024];
        glGetShaderInfoLog(shader, 1024, nullptr, log);
        logger->Error("Shader compile error:\n%s", log);
        return 0;
    }
    
    return shader;
}

GLuint CreateProgram(const char* vertSrc, const char* fragSrc) {
    GLuint vert = CompileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint frag = CompileShader(GL_FRAGMENT_SHADER, fragSrc);
    
    if(!vert || !frag) return 0;
    
    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);
    
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if(!success) {
        char log[1024];
        glGetProgramInfoLog(program, 1024, nullptr, log);
        logger->Error("Program link error:\n%s", log);
        return 0;
    }
    
    glDeleteShader(vert);
    glDeleteShader(frag);
    
    return program;
}

// ============================================================================
// MATRIX UTILITIES
// ============================================================================

void Matrix4x4Invert(const float* m, float* out) {
    // 4x4 matrix inversion
    float inv[16];
    
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + 
             m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
             m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
             m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
              m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
             m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
             m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
             m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
              m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
             m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
             m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
              m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
              m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
             m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
             m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
              m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
              m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    
    if(fabs(det) < 0.0001f) {
        // Singular matrix, return identity
        memset(out, 0, 16 * sizeof(float));
        out[0] = out[5] = out[10] = out[15] = 1.0f;
        return;
    }
    
    det = 1.0f / det;
    
    for(int i = 0; i < 16; i++)
        out[i] = inv[i] * det;
}

void ConvertRwMatrixToGL(const RwMatrix* rw, float* gl) {
    // RenderWare uses row-major, OpenGL uses column-major
    gl[0]  = rw->right.x; gl[4]  = rw->up.x; gl[8]   = rw->at.x; gl[12] = rw->pos.x;
    gl[1]  = rw->right.y; gl[5]  = rw->up.y; gl[9]   = rw->at.y; gl[13] = rw->pos.y;
    gl[2]  = rw->right.z; gl[6]  = rw->up.z; gl[10]  = rw->at.z; gl[14] = rw->pos.z;
    gl[3]  = 0.0f;        gl[7]  = 0.0f;     gl[11]  = 0.0f;     gl[15] = 1.0f;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

bool InitAddresses() {
    logger->Info("Resolving addresses...");
    
    void* lib = dlopen("libGTASA.so", RTLD_NOW);
    if(!lib) {
        logger->Error("Failed to load libGTASA.so");
        return false;
    }
    
    #define RESOLVE(name, offset) \
        name = (decltype(name))((uintptr_t)lib + offset); \
        if(!name) logger->Error("Failed to resolve " #name);
    
    RESOLVE(_rwCameraValRender_orig, 0x001d7140);
    RESOLVE(GetCurrentViewMatrix, 0x001ba6e0);
    RESOLVE(GetCurrentProjectionMatrix, 0x001ba6f8);
    RESOLVE(SetupScreenSpaceProjection, 0x001ada2c);
    RESOLVE(RwRasterLock, 0x001daaf4);
    RESOLVE(RwRasterUnlock, 0x001da738);
    RESOLVE(RwRasterCreate, 0x001daa50);
    RESOLVE(RwRasterDestroy, 0x001da850);
    RESOLVE(RwMatrixInvert, 0x001e3a28);
    RESOLVE(RwV3dTransformPoint, 0x001e698c);
    RESOLVE(_rwOpenGLGetEngineZBufferDepth, 0x001b13bc);
    RESOLVE(RwRasterShowRaster, 0x001da9bc);
    
    g_pZBuffer = (RwRaster**)((uintptr_t)lib + 0x00a5a138);
    g_rwRaster_cpPixels = (unsigned char**)((uintptr_t)lib + 0x0067a044);
    g_TheCamera = (void*)((uintptr_t)lib + 0x00951fa8);
    
    #undef RESOLVE
    
    logger->Info("Addresses resolved");
    return true;
}

bool InitShaders() {
    logger->Info("Compiling shaders...");
    
    // AO shader
    aoProgram = CreateProgram(aoVertShader, aoFragShader);
    if(!aoProgram) return false;
    
    aoUniforms.depthTex = glGetUniformLocation(aoProgram, "uDepthTex");
    aoUniforms.viewMatrix = glGetUniformLocation(aoProgram, "uViewMatrix");
    aoUniforms.projMatrix = glGetUniformLocation(aoProgram, "uProjMatrix");
    aoUniforms.invViewMatrix = glGetUniformLocation(aoProgram, "uInvViewMatrix");
    aoUniforms.invProjMatrix = glGetUniformLocation(aoProgram, "uInvProjMatrix");
    aoUniforms.nearPlane = glGetUniformLocation(aoProgram, "uNearPlane");
    aoUniforms.farPlane = glGetUniformLocation(aoProgram, "uFarPlane");
    aoUniforms.screenSize = glGetUniformLocation(aoProgram, "uScreenSize");
    aoUniforms.samples = glGetUniformLocation(aoProgram, "uSamples");
    aoUniforms.radius = glGetUniformLocation(aoProgram, "uRadius");
    aoUniforms.density = glGetUniformLocation(aoProgram, "uDensity");
    
    // Blur shader
    blurProgram = CreateProgram(aoVertShader, blurFragShader);
    if(!blurProgram) return false;
    
    blurUniforms.aoTex = glGetUniformLocation(blurProgram, "uAOTex");
    blurUniforms.depthTex = glGetUniformLocation(blurProgram, "uDepthTex");
    blurUniforms.direction = glGetUniformLocation(blurProgram, "uDirection");
    blurUniforms.radius = glGetUniformLocation(blurProgram, "uRadius");
    blurUniforms.screenSize = glGetUniformLocation(blurProgram, "uScreenSize");
    
    // Composite shader
    compositeProgram = CreateProgram(aoVertShader, compositeFragShader);
    if(!compositeProgram) return false;
    
    compositeUniforms.sceneTex = glGetUniformLocation(compositeProgram, "uSceneTex");
    compositeUniforms.aoTex = glGetUniformLocation(compositeProgram, "uAOTex");
    compositeUniforms.debugMode = glGetUniformLocation(compositeProgram, "uDebugMode");
    
    logger->Info("Shaders compiled");
    return true;
}

bool InitGeometry() {
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 
                         (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
    
    return true;
}

bool InitRenderTargets(int width, int height) {
    // Apply resolution scale
    float scale = pResolutionScale->GetFloat();
    int aoWidth = (int)(width * scale);
    int aoHeight = (int)(height * scale);
    
    logger->Info("Creating render targets: scene=%dx%d, AO=%dx%d", 
                 width, height, aoWidth, aoHeight);
    
    // Create textures
    auto CreateTexture = [](GLuint& tex, int w, int h, GLint format, GLenum type) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, 
                     (format == GL_RGBA8) ? GL_RGBA : GL_RED, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    
    CreateTexture(aoTexture, aoWidth, aoHeight, GL_R16F, GL_FLOAT);
    CreateTexture(blurTexture, aoWidth, aoHeight, GL_R16F, GL_FLOAT);
    CreateTexture(sceneTexture, width, height, GL_RGBA8, GL_UNSIGNED_BYTE);
    
    // Create FBOs
    auto CreateFBO = [](GLuint& fbo, GLuint colorTex) {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 
                              GL_TEXTURE_2D, colorTex, 0);
        
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            logger->Error("Framebuffer incomplete!");
            return false;
        }
        return true;
    };
    
    if(!CreateFBO(aoFBO, aoTexture)) return false;
    if(!CreateFBO(blurFBO, blurTexture)) return false;
    if(!CreateFBO(compositeFBO, sceneTexture)) return false;
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    logger->Info("Render targets created");
    return true;
}

bool InitSSAO() {
    logger->Info("Initializing Complete SSAO...");
    
    if(!InitAddresses()) return false;
    if(!InitShaders()) return false;
    if(!InitGeometry()) return false;
    
    logger->Info("Complete SSAO initialized");
    return true;
}

// ============================================================================
// DEPTH EXTRACTION
// ============================================================================

GLuint ExtractDepthTexture(RwRaster* zBuffer) {
    if(!zBuffer || !zBuffer->pixels) {
        logger->Error("Invalid Z-buffer");
        return 0;
    }
    
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    
    // Lock raster to get pixel data
    if(RwRasterLock) {
        RwRasterLock(zBuffer, 0, 2); // RASTER_LOCK_READ
    }
    
    // Determine format
    GLenum format = GL_DEPTH_COMPONENT;
    GLenum type = GL_UNSIGNED_INT;
    
    if(zBuffer->depth == 24) {
        format = GL_DEPTH_COMPONENT;
        type = GL_UNSIGNED_INT;
    } else if(zBuffer->depth == 16) {
        format = GL_DEPTH_COMPONENT;
        type = GL_UNSIGNED_SHORT;
    } else if(zBuffer->depth == 32) {
        format = GL_DEPTH_COMPONENT;
        type = GL_FLOAT;
    }
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 
                 zBuffer->width, zBuffer->height, 
                 0, format, type, zBuffer->pixels);
    
    if(RwRasterUnlock) {
        RwRasterUnlock(zBuffer);
    }
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    return tex;
}

// ============================================================================
// SCENE CAPTURE
// ============================================================================

void CaptureSceneTexture(RwRaster* frameBuffer) {
    if(!frameBuffer) return;
    
    // Read framebuffer into texture
    glBindTexture(GL_TEXTURE_2D, sceneTexture);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 
                     0, 0, frameBuffer->width, frameBuffer->height, 0);
}

// ============================================================================
// MAIN RENDERING
// ============================================================================

void RenderSSAO(RwCamera* camera) {
    if(!pEnabled->GetBool()) return;
    if(!camera || !camera->bufferColor) return;
    
    RwRaster* frameBuffer = camera->bufferColor;
    RwRaster* zBuffer = (g_pZBuffer && *g_pZBuffer) ? *g_pZBuffer : camera->bufferDepth;
    
    if(!zBuffer) {
        logger->Error("No Z-buffer available");
        return;
    }
    
    int width = frameBuffer->width;
    int height = frameBuffer->height;
    
    float scale = pResolutionScale->GetFloat();
    int aoWidth = (int)(width * scale);
    int aoHeight = (int)(height * scale);
    
    // Recreate render targets if resolution changed
    static int lastWidth = 0, lastHeight = 0;
    if(width != lastWidth || height != lastHeight) {
        if(aoTexture) glDeleteTextures(1, &aoTexture);
        if(blurTexture) glDeleteTextures(1, &blurTexture);
        if(sceneTexture) glDeleteTextures(1, &sceneTexture);
        if(aoFBO) glDeleteFramebuffers(1, &aoFBO);
        if(blurFBO) glDeleteFramebuffers(1, &blurFBO);
        if(compositeFBO) glDeleteFramebuffers(1, &compositeFBO);
        
        if(!InitRenderTargets(width, height)) return;
        
        lastWidth = width;
        lastHeight = height;
    }
    
    // Step 1: Capture scene
    CaptureSceneTexture(frameBuffer);
    
    // Step 2: Extract depth
    if(depthTexture) glDeleteTextures(1, &depthTexture);
    depthTexture = ExtractDepthTexture(zBuffer);
    if(!depthTexture) return;
    
    // Step 3: Get matrices
    float* viewMat = GetCurrentViewMatrix();
    float* projMat = GetCurrentProjectionMatrix();
    
    if(!viewMat || !projMat) {
        logger->Error("Failed to get matrices");
        return;
    }
    
    // Convert to GL format if needed
    float viewMatGL[16], projMatGL[16];
    memcpy(viewMatGL, viewMat, 16 * sizeof(float));
    memcpy(projMatGL, projMat, 16 * sizeof(float));
    
    // Invert matrices
    float invViewMat[16], invProjMat[16];
    Matrix4x4Invert(viewMatGL, invViewMat);
    Matrix4x4Invert(projMatGL, invProjMat);
    
    // Get camera properties
    float nearPlane = camera->nearplane;
    float farPlane = camera->farplane;
    
    // Save GL state
    GLint lastFBO, lastViewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &lastFBO);
    glGetIntegerv(GL_VIEWPORT, lastViewport);
    
    // === PASS 1: Compute AO ===
    glBindFramebuffer(GL_FRAMEBUFFER, aoFBO);
    glViewport(0, 0, aoWidth, aoHeight);
    glClear(GL_COLOR_BUFFER_BIT);
    
    glUseProgram(aoProgram);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depthTexture);
    glUniform1i(aoUniforms.depthTex, 0);
    
    glUniformMatrix4fv(aoUniforms.viewMatrix, 1, GL_FALSE, viewMatGL);
    glUniformMatrix4fv(aoUniforms.projMatrix, 1, GL_FALSE, projMatGL);
    glUniformMatrix4fv(aoUniforms.invViewMatrix, 1, GL_FALSE, invViewMat);
    glUniformMatrix4fv(aoUniforms.invProjMatrix, 1, GL_FALSE, invProjMat);
    
    glUniform1f(aoUniforms.nearPlane, nearPlane);
    glUniform1f(aoUniforms.farPlane, farPlane);
    glUniform2f(aoUniforms.screenSize, (float)width, (float)height);
    glUniform1f(aoUniforms.samples, (float)pSamples->GetInt());
    glUniform1f(aoUniforms.radius, pRadius->GetFloat());
    glUniform1f(aoUniforms.density, pDensity->GetFloat());
    
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    
    // === PASS 2: Bilateral Blur ===
    if(pBlurEnabled->GetBool()) {
        glUseProgram(blurProgram);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, depthTexture);
        glUniform1i(blurUniforms.depthTex, 1);
        glUniform1f(blurUniforms.radius, (float)pBlurRadius->GetInt());
        glUniform2f(blurUniforms.screenSize, (float)aoWidth, (float)aoHeight);
        
        // Horizontal pass
        glBindFramebuffer(GL_FRAMEBUFFER, blurFBO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, aoTexture);
        glUniform1i(blurUniforms.aoTex, 0);
        glUniform1i(blurUniforms.direction, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        
        // Vertical pass
        glBindFramebuffer(GL_FRAMEBUFFER, aoFBO);
        glBindTexture(GL_TEXTURE_2D, blurTexture);
        glUniform1i(blurUniforms.direction, 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    
    // === PASS 3: Composite ===
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)lastFBO);
    glViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);
    
    glUseProgram(compositeProgram);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneTexture);
    glUniform1i(compositeUniforms.sceneTex, 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, aoTexture);
    glUniform1i(compositeUniforms.aoTex, 1);
    
    glUniform1i(compositeUniforms.debugMode, pDebugMode->GetInt());
    
    glDrawArrays(GL_TRIANGLES, 0, 6);
    
    // Cleanup
    glBindVertexArray(0);
    glUseProgram(0);
    
    // Delete temporary depth texture
    if(depthTexture) {
        glDeleteTextures(1, &depthTexture);
        depthTexture = 0;
    }
}

// ============================================================================
// HOOK
// ============================================================================

void _rwCameraValRender_Hook(RwCamera* camera) {
    // Call original
    _rwCameraValRender_orig(camera);
    
    // Apply SSAO
    RenderSSAO(camera);
}

// ============================================================================
// MOD LIFECYCLE
// ============================================================================

extern "C" void OnModPreLoad() {
    logger->SetTag("CompleteSSAO");
    
    pEnabled = cfg->Bind("Enabled", true, "Enable/Disable SSAO");
    pSamples = cfg->Bind("Samples", 16, "Number of AO samples (8-32)");
    pRadius = cfg->Bind("Radius", 1.5f, "AO sampling radius");
    pDensity = cfg->Bind("Density", 1.0f, "AO effect intensity");
    pBlurEnabled = cfg->Bind("BlurEnabled", true, "Enable bilateral blur");
    pBlurRadius = cfg->Bind("BlurRadius", 3, "Blur kernel radius (1-5)");
    pDebugMode = cfg->Bind("DebugMode", 0, "0=Normal, 1=AO only, 2=Split");
    pResolutionScale = cfg->Bind("ResolutionScale", 0.75f, "AO resolution scale (0.5-1.0)");
    
    cfg->Save();
}

extern "C" void OnModLoad() {
    logger->Info("===========================================");
    logger->Info("  Complete SSAO for GTA SA Android");
    logger->Info("===========================================");
    
    if(!InitSSAO()) {
        logger->Error("Failed to initialize SSAO!");
        return;
    }
    
    // Install hook
    if(_rwCameraValRender_orig) {
        aml->Redirect((uintptr_t)_rwCameraValRender_orig, 
                     (uintptr_t)_rwCameraValRender_Hook);
        logger->Info("Render hook installed at 0x001d7140");
    } else {
        logger->Error("Failed to hook render function!");
        return;
    }
    
    logger->Info("===========================================");
    logger->Info("  SSAO loaded successfully!");
    logger->Info("  Config: samples=%d, radius=%.2f, blur=%s",
                 pSamples->GetInt(), 
                 pRadius->GetFloat(),
                 pBlurEnabled->GetBool() ? "enabled" : "disabled");
    logger->Info("===========================================");
}

extern "C" void OnModUnload() {
    logger->Info("Unloading SSAO...");
    
    // Cleanup OpenGL resources
    if(aoProgram) glDeleteProgram(aoProgram);
    if(blurProgram) glDeleteProgram(blurProgram);
    if(compositeProgram) glDeleteProgram(compositeProgram);
    
    if(quadVAO) glDeleteVertexArrays(1, &quadVAO);
    if(quadVBO) glDeleteBuffers(1, &quadVBO);
    
    if(aoFBO) glDeleteFramebuffers(1, &aoFBO);
    if(blurFBO) glDeleteFramebuffers(1, &blurFBO);
    if(compositeFBO) glDeleteFramebuffers(1, &compositeFBO);
    
    if(aoTexture) glDeleteTextures(1, &aoTexture);
    if(blurTexture) glDeleteTextures(1, &blurTexture);
    if(sceneTexture) glDeleteTextures(1, &sceneTexture);
    if(depthTexture) glDeleteTextures(1, &depthTexture);
    
    logger->Info("SSAO unloaded successfully");
}
