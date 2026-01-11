#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES2/gl2.h>
#include <string.h>
#include <stdio.h>

MYMOD(net.ssao.gtasa, SSAO_v200, 1.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

#define SHADER_LEN 32768

// ============================================================================
// Game Variables (from your symbols)
// ============================================================================
uintptr_t pGTASA = 0;
void* hGTASA = NULL;

float* m_VectorToSun;           // 0x0096b258
uint32_t* m_snTimeInMilliseconds; // 0x00953138
float* UnderWaterness;          // 0x00a7d158
float* WetRoads;                // 0x00a7d144
void* TheCamera;                // 0x00951fa8
uint32_t* curShaderStateFlags;  // 0x006b7094

// GL Functions
typedef int (*glGetUniformLocation_t)(int, const char*);
typedef void (*glUniform1i_t)(int, int);
typedef void (*glUniform1fv_t)(int, int, const float*);
typedef void (*glUniform2fv_t)(int, int, const float*);

glGetUniformLocation_t _glGetUniformLocation;
glUniform1i_t _glUniform1i;
glUniform1fv_t _glUniform1fv;
glUniform2fv_t _glUniform2fv;

// ============================================================================
// SSAO Shader Code (GLSL ES 2.0)
// ============================================================================

const char* ssaoFragmentShader = R"(
precision mediump float;
varying vec2 v_texCoord0;
uniform sampler2D uColorTex;
uniform vec2 uPixelSize;
uniform float uIntensity;
uniform float uTime;

// Pseudo-depth from luminance
float getDepth(vec2 uv) {
    vec3 col = texture2D(uColorTex, uv).rgb;
    return dot(col, vec3(0.299, 0.587, 0.114));
}

// Dither pattern
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec3 color = texture2D(uColorTex, v_texCoord0).rgb;
    float depth = getDepth(v_texCoord0);
    
    // SSAO sampling
    float ao = 0.0;
    float radius = 2.5;
    int samples = 8;
    float angleOffset = hash(v_texCoord0) * 6.28318;
    
    for(int i = 0; i < 8; i++) {
        float angle = (float(i) / 8.0) * 6.28318 + angleOffset;
        vec2 offset = vec2(cos(angle), sin(angle)) * radius * uPixelSize;
        
        float sampleDepth = getDepth(v_texCoord0 + offset);
        float diff = depth - sampleDepth;
        
        // Accumulate occlusion
        ao += max(0.0, diff) * smoothstep(0.0, radius * 0.5, abs(diff));
    }
    
    ao = clamp(ao / float(samples), 0.0, 1.0);
    float occlusion = 1.0 - (ao * uIntensity);
    
    // Apply AO
    gl_FragColor = vec4(color * occlusion, 1.0);
}
)";

// Custom shader storage
char customFragShader[SHADER_LEN];
bool bSSAOInjected = false;

// ============================================================================
// Shader Injection Hook
// ============================================================================

// RQShader::BuildSource hook (0x001cfa38)
DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    // Inject SSAO into specific shader flags
    // 0x10 = basic 2D shader, good for post-process
    if(flags == 0x10 && !bSSAOInjected) {
        logger->Info("Injecting SSAO shader into flag 0x%X", flags);
        strncpy(customFragShader, ssaoFragmentShader, SHADER_LEN - 1);
        customFragShader[SHADER_LEN - 1] = '\0';
        *pxlsrc = customFragShader;
        bSSAOInjected = true;
    }
    
    return ret;
}

// ============================================================================
// ES2Shader Extended Structure
// ============================================================================

struct ES2ShaderExtended {
    char padding[256]; // ES2Shader base size (estimate)
    
    // Custom uniform locations
    int uid_uPixelSize;
    int uid_uIntensity;
    int uid_uTime;
};

// ES2Shader::Select hook (0x001cd368)
DECL_HOOKv(ES2Shader_Select, ES2ShaderExtended* self) {
    ES2Shader_Select(self);
    
    // Set SSAO uniforms if shader has them
    if(self->uid_uPixelSize == -1) {
        // Initialize custom uniforms (first time)
        self->uid_uPixelSize = _glGetUniformLocation(*(int*)self, "uPixelSize");
        self->uid_uIntensity = _glGetUniformLocation(*(int*)self, "uIntensity");
        self->uid_uTime = _glGetUniformLocation(*(int*)self, "uTime");
        
        if(self->uid_uPixelSize >= 0) {
            logger->Info("SSAO uniforms found in shader!");
        }
    }
    
    // Update uniforms each frame
    if(self->uid_uPixelSize >= 0) {
        float pixelSize[2] = {1.0f / 1280.0f, 1.0f / 720.0f}; // TODO: get real screen size
        _glUniform2fv(self->uid_uPixelSize, 1, pixelSize);
    }
    
    if(self->uid_uIntensity >= 0) {
        float intensity = 0.6f; // AO strength
        _glUniform1fv(self->uid_uIntensity, 1, &intensity);
    }
    
    if(self->uid_uTime >= 0 && m_snTimeInMilliseconds) {
        float time = (float)(*m_snTimeInMilliseconds) / 1000.0f;
        _glUniform1fv(self->uid_uTime, 1, &time);
    }
}

// ES2Shader::InitializeAfterCompile hook (0x001cc7cc) - initialize custom uniforms
DECL_HOOKv(ES2Shader_InitAfterCompile, ES2ShaderExtended* self) {
    ES2Shader_InitAfterCompile(self);
    
    // Initialize to -1 (not found)
    self->uid_uPixelSize = -1;
    self->uid_uIntensity = -1;
    self->uid_uTime = -1;
}

// ============================================================================
// Mod Entry Point
// ============================================================================

extern "C" void OnModLoad() {
    logger->SetTag("SSAO");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA not found!");
        return;
    }
    
    // Get game variables
    m_VectorToSun = (float*)(pGTASA + 0x0096b258);
    m_snTimeInMilliseconds = (uint32_t*)(pGTASA + 0x00953138);
    UnderWaterness = (float*)(pGTASA + 0x00a7d158);
    WetRoads = (float*)(pGTASA + 0x00a7d144);
    TheCamera = (void*)(pGTASA + 0x00951fa8);
    curShaderStateFlags = (uint32_t*)(pGTASA + 0x006b7094);
    
    // Get GL functions (PLT imports)
    _glGetUniformLocation = (glGetUniformLocation_t)(*(void**)(pGTASA + 0x0019f070));
    _glUniform1i = (glUniform1i_t)(*(void**)(pGTASA + 0x0019bc38));
    _glUniform1fv = (glUniform1fv_t)(*(void**)(pGTASA + 0x00195944));
    _glUniform2fv = (glUniform2fv_t)(*(void**)(pGTASA + 0x001986e0));
    
    // Hook shader system
    HOOKPLT(RQShaderBuildSource, pGTASA + 0x001cfa38);
    HOOK(ES2Shader_Select, pGTASA + 0x001cd368);
    HOOK(ES2Shader_InitAfterCompile, pGTASA + 0x001cc7cc);
    
    logger->Info("SSAO v2.00 loaded successfully!");
    logger->Info("Waiting for shader injection...");
}
