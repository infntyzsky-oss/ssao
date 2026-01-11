#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES2/gl2.h>
#include <string.h>

MYMOD(net.ssao.brutal, SSAO_Brutal, 1.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

#define SHADER_LEN 65536

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

// Game vars
uint32_t* m_snTimeInMilliseconds;
float* UnderWaterness;

// GL functions
typedef int (*glGetUniformLocation_t)(int, const char*);
typedef void (*glUniform1i_t)(int, int);
typedef void (*glUniform1fv_t)(int, int, const float*);
typedef void (*glUniform2fv_t)(int, int, const float*);

glGetUniformLocation_t _glGetUniformLocation;
glUniform1i_t _glUniform1i;
glUniform1fv_t _glUniform1fv;
glUniform2fv_t _glUniform2fv;

// ============================================================================
// BRUTAL SSAO SHADER (High Quality)
// ============================================================================

const char* ssaoFragmentShader = R"(
precision highp float;
varying vec2 v_texCoord0;
uniform sampler2D uColorTex;
uniform vec2 uPixelSize;
uniform float uIntensity;
uniform float uTime;

// Hash for randomization
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Pseudo-depth from luminance
float getDepth(vec2 uv) {
    vec3 col = texture2D(uColorTex, uv).rgb;
    return dot(col, vec3(0.299, 0.587, 0.114));
}

// SSAO sampling with dithering
void main() {
    vec3 color = texture2D(uColorTex, v_texCoord0).rgb;
    float depth = getDepth(v_texCoord0);
    
    // SSAO parameters
    float ao = 0.0;
    float radius = 3.0;
    int samples = 12;
    
    // Dither pattern per pixel
    float angleOffset = hash(v_texCoord0 + uTime * 0.001) * 6.28318;
    
    // Sample surrounding pixels
    for(int i = 0; i < 12; i++) {
        float angle = (float(i) / float(samples)) * 6.28318 + angleOffset;
        float dist = (float(i) / float(samples)) * radius;
        
        vec2 offset = vec2(cos(angle), sin(angle)) * dist * uPixelSize;
        float sampleDepth = getDepth(v_texCoord0 + offset);
        
        // Calculate occlusion
        float diff = depth - sampleDepth;
        float weight = smoothstep(0.0, radius * 0.5, dist);
        
        ao += max(0.0, diff) * weight;
    }
    
    // Normalize and apply
    ao = clamp(ao / float(samples), 0.0, 1.0);
    float occlusion = 1.0 - (ao * uIntensity);
    
    // Add subtle darkening in corners
    vec2 cornerDist = abs(v_texCoord0 - 0.5) * 2.0;
    float vignette = 1.0 - pow(max(cornerDist.x, cornerDist.y), 2.0) * 0.2;
    
    gl_FragColor = vec4(color * occlusion * vignette, 1.0);
}
)";

char customFragShader[SHADER_LEN];
bool bInjected = false;

// ============================================================================
// ES2Shader Extended Structure
// ============================================================================

struct ES2ShaderEx {
    char base[512];
    int uid_uPixelSize;
    int uid_uIntensity;
    int uid_uTime;
};

// ============================================================================
// Hooks
// ============================================================================

DECL_HOOK(int, RQShaderBuildSource, int flags, char** pxlsrc, char** vtxsrc) {
    int ret = RQShaderBuildSource(flags, pxlsrc, vtxsrc);
    
    static int count = 0;
    if(count < 20) {
        logger->Info("Shader flag: 0x%X", flags);
        count++;
    }
    
    // Inject SSAO into multiple shader types
    if(!bInjected) {
        if(flags == 0x10 || flags == 0x200010 || flags == 0x4000010 || 
           flags == 0x80430 || flags == 0x90430) {
            
            logger->Info("BRUTAL SSAO INJECTION! (flag=0x%X)", flags);
            strncpy(customFragShader, ssaoFragmentShader, SHADER_LEN - 1);
            customFragShader[SHADER_LEN - 1] = '\0';
            *pxlsrc = customFragShader;
            bInjected = true;
        }
    }
    
    return ret;
}

DECL_HOOKv(ES2Shader_Select, ES2ShaderEx* self) {
    ES2Shader_Select(self);
    
    // Init uniforms first time
    if(self->uid_uPixelSize == -1 && _glGetUniformLocation) {
        int shaderId = *(int*)self;
        self->uid_uPixelSize = _glGetUniformLocation(shaderId, "uPixelSize");
        self->uid_uIntensity = _glGetUniformLocation(shaderId, "uIntensity");
        self->uid_uTime = _glGetUniformLocation(shaderId, "uTime");
        
        if(self->uid_uPixelSize >= 0) {
            logger->Info("SSAO SHADER ACTIVE! Uniforms: pix=%d, int=%d, time=%d", 
                        self->uid_uPixelSize, self->uid_uIntensity, self->uid_uTime);
        }
    }
    
    // Update uniforms every frame
    if(self->uid_uPixelSize >= 0) {
        // Dynamic resolution (adjust based on device)
        float pixelSize[2] = {1.0f / 1920.0f, 1.0f / 1080.0f};
        _glUniform2fv(self->uid_uPixelSize, 1, pixelSize);
        
        // High intensity for brutal effect
        float intensity = 0.85f;
        _glUniform1fv(self->uid_uIntensity, 1, &intensity);
        
        // Animated dithering
        if(m_snTimeInMilliseconds) {
            float time = (float)(*m_snTimeInMilliseconds);
            _glUniform1fv(self->uid_uTime, 1, &time);
        }
    }
}

DECL_HOOKv(ES2Shader_InitAfterCompile, ES2ShaderEx* self) {
    ES2Shader_InitAfterCompile(self);
    
    self->uid_uPixelSize = -1;
    self->uid_uIntensity = -1;
    self->uid_uTime = -1;
}

// ============================================================================
// Mod Entry
// ============================================================================

extern "C" void OnModPreLoad() {
    logger->SetTag("SSAO_BRUTAL");
    logger->Info("BRUTAL SSAO - PREPARING...");
}

extern "C" void OnModLoad() {
    logger->SetTag("SSAO_BRUTAL");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if(!pGTASA || !hGTASA) {
        logger->Error("GTA:SA not found!");
        return;
    }
    
    // Get game vars
    m_snTimeInMilliseconds = (uint32_t*)(pGTASA + 0x00953138);
    UnderWaterness = (float*)(pGTASA + 0x00a7d158);
    
    // Get GL functions
    void* hGLES = aml->GetLibHandle("libGLESv2.so");
    if(hGLES) {
        _glGetUniformLocation = (glGetUniformLocation_t)aml->GetSym(hGLES, "glGetUniformLocation");
        _glUniform1i = (glUniform1i_t)aml->GetSym(hGLES, "glUniform1i");
        _glUniform1fv = (glUniform1fv_t)aml->GetSym(hGLES, "glUniform1fv");
        _glUniform2fv = (glUniform2fv_t)aml->GetSym(hGLES, "glUniform2fv");
        logger->Info("✓ GL functions loaded from libGLESv2.so");
    } else {
        logger->Error("libGLESv2.so not found - using fallback");
    }
    
    // Hook shader system
    logger->Info("Hooking shader system...");
    
    void* addr1 = (void*)aml->GetSym(hGTASA, "_ZN8RQShader11BuildSourceEjPPKcS2_");
    if(!addr1) addr1 = (void*)(pGTASA + 0x001CFA38);
    HOOK(RQShaderBuildSource, addr1);
    logger->Info("✓ Hook 1: RQShader::BuildSource");
    
    void* addr2 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader6SelectEv");
    if(!addr2) addr2 = (void*)(pGTASA + 0x001CD368);
    HOOK(ES2Shader_Select, addr2);
    logger->Info("✓ Hook 2: ES2Shader::Select");
    
    void* addr3 = (void*)aml->GetSym(hGTASA, "_ZN9ES2Shader22InitializeAfterCompileEv");
    if(!addr3) addr3 = (void*)(pGTASA + 0x001CC7CC);
    HOOK(ES2Shader_InitAfterCompile, addr3);
    logger->Info("✓ Hook 3: ES2Shader::InitAfterCompile");
    
    logger->Info("BRUTAL SSAO LOADED!");
    logger->Info("Expect HEAVY ambient occlusion & vignette!");
}
