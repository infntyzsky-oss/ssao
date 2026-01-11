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
// Game Variables
// ============================================================================
uintptr_t pGTASA = 0;
void* hGTASA = NULL;

uint32_t* m_snTimeInMilliseconds;
float* UnderWaterness;
uint32_t* curShaderStateFlags;

// GL Functions
typedef int (*glGetUniformLocation_t)(int, const char*);
typedef void (*glUniform1fv_t)(int, int, const float*);
typedef void (*glUniform2fv_t)(int, int, const float*);

glGetUniformLocation_t _glGetUniformLocation;
glUniform1fv_t _glUniform1fv;
glUniform2fv_t _glUniform2fv;

// ============================================================================
// SSAO Shader
// ============================================================================

const char* ssaoFragmentShader = R"(
precision mediump float;
varying vec2 v_texCoord0;
uniform sampler2D uColorTex;
uniform vec2 uPixelSize;
uniform float uIntensity;

float getDepth(vec2 uv) {
    vec3 col = texture2D(uColorTex, uv).rgb;
    return dot(col, vec3(0.299, 0.587, 0.114));
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec3 color = texture2D(uColorTex, v_texCoord0).rgb;
    float depth = getDepth(v_texCoord0);
    
    float ao = 0.0;
    float radius = 2.5;
    float angleOffset = hash(v_texCoord0) * 6.28318;
    
    for(int i = 0; i < 8; i++) {
        float angle = (float(i) / 8.0) * 6.28318 + angleOffset;
        vec2 offset = vec2(cos(angle), sin(angle)) * radius * uPixelSize;
        
        float sampleDepth = getDepth(v_texCoord0 + offset);
        float diff = depth - sampleDepth;
        
        ao += max(0.0, diff) * smoothstep(0.0, radius * 0.5, abs(diff));
    }
    
    ao = clamp(ao / 8.0, 0.0, 1.0);
    float occlusion = 1.0 - (ao * uIntensity);
    
    gl_FragColor = vec4(color * occlusion, 1.0);
}
)";

char customFragShader[SHADER_LEN];
bool bSSAOInjected = false;

// ============================================================================
// Shader Hooks
// ============================================================================

// RQShader::BuildSource (0x001cfa38)
DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    // Inject SSAO shader for basic 2D rendering
    if(flags == 0x10 && !bSSAOInjected) {
        logger->Info("Injecting SSAO fragment shader (flags=0x%X)", flags);
        strncpy(customFragShader, ssaoFragmentShader, SHADER_LEN - 1);
        customFragShader[SHADER_LEN - 1] = '\0';
        *pxlsrc = customFragShader;
        bSSAOInjected = true;
    }
    
    return ret;
}

// ES2Shader extended with custom uniforms
struct ES2ShaderEx {
    char base[256];
    int uid_uPixelSize;
    int uid_uIntensity;
};

// ES2Shader::Select (0x001cd368)
DECL_HOOKv(ES2Shader_Select, ES2ShaderEx* self) {
    ES2Shader_Select(self);
    
    // Initialize uniforms if needed
    if(self->uid_uPixelSize == -1) {
        int shaderId = *(int*)self;
        self->uid_uPixelSize = _glGetUniformLocation(shaderId, "uPixelSize");
        self->uid_uIntensity = _glGetUniformLocation(shaderId, "uIntensity");
        
        if(self->uid_uPixelSize >= 0) {
            logger->Info("SSAO shader active! Uniforms found.");
        }
    }
    
    // Update uniforms
    if(self->uid_uPixelSize >= 0) {
        float pixelSize[2] = {1.0f / 1280.0f, 1.0f / 720.0f};
        _glUniform2fv(self->uid_uPixelSize, 1, pixelSize);
        
        float intensity = 0.5f;
        _glUniform1fv(self->uid_uIntensity, 1, &intensity);
    }
}

// ES2Shader::InitializeAfterCompile (0x001cc7cc)
DECL_HOOKv(ES2Shader_InitAfterCompile, ES2ShaderEx* self) {
    ES2Shader_InitAfterCompile(self);
    
    self->uid_uPixelSize = -1;
    self->uid_uIntensity = -1;
}

// ============================================================================
// Mod Entry
// ============================================================================

extern "C" void OnModLoad() {
    logger->SetTag("SSAO");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA lib not found!");
        return;
    }
    
    // Get variables
    m_snTimeInMilliseconds = (uint32_t*)(pGTASA + 0x00953138);
    UnderWaterness = (float*)(pGTASA + 0x00a7d158);
    curShaderStateFlags = (uint32_t*)(pGTASA + 0x006b7094);
    
    // Get GL functions (from PLT)
    _glGetUniformLocation = (glGetUniformLocation_t)(*(void**)(pGTASA + 0x0019f070));
    _glUniform1fv = (glUniform1fv_t)(*(void**)(pGTASA + 0x00195944));
    _glUniform2fv = (glUniform2fv_t)(*(void**)(pGTASA + 0x001986e0));
    
    // HOOK (bukan HOOKPLT!)
    HOOK(RQShaderBuildSource, pGTASA + 0x001cfa38);
    HOOK(ES2Shader_Select, pGTASA + 0x001cd368);
    HOOK(ES2Shader_InitAfterCompile, pGTASA + 0x001cc7cc);
    
    logger->Info("SSAO v2.00 loaded!");
}
