#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <string.h>

MYMOD(net.ssao.gtasa, SSAO_Mobile, 1.0, ntyzsky)
NEEDGAME(com.rockstargames.gtasa)

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

// ============================================================================
// GLSL Shaders
// ============================================================================

const char* vertexShaderSrc = R"(
attribute vec4 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;

void main() {
    gl_Position = aPosition;
    vTexCoord = aTexCoord;
}
)";

const char* fragmentShaderSrc = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uSceneTex;
uniform vec2 uPixelSize;
uniform float uIntensity;

float getDepth(vec2 uv) {
    vec3 col = texture2D(uSceneTex, uv).rgb;
    return dot(col, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec3 sceneColor = texture2D(uSceneTex, vTexCoord).rgb;
    float centerDepth = getDepth(vTexCoord);
    
    float ao = 0.0;
    float radius = 2.0;
    
    for(int i = 0; i < 4; i++) {
        float angle = float(i) * 1.5708;
        vec2 offset = vec2(cos(angle), sin(angle)) * radius * uPixelSize;
        
        float sampleDepth = getDepth(vTexCoord + offset);
        float diff = centerDepth - sampleDepth;
        
        ao += max(0.0, diff) * 0.5;
    }
    
    ao = clamp(ao / 4.0, 0.0, 1.0);
    float occlusion = 1.0 - (ao * uIntensity);
    
    gl_FragColor = vec4(sceneColor * occlusion, 1.0);
}
)";

GLuint shaderProgram = 0;
bool bSSAOInitialized = false;

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        logger->Error("Shader error: %s", infoLog);
        return 0;
    }
    return shader;
}

bool InitSSAOShader() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexShaderSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSrc);
    
    if (!vs || !fs) return false;
    
    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vs);
    glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);
    
    GLint success;
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        logger->Error("Link error: %s", infoLog);
        return false;
    }
    
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    logger->Info("SSAO shader compiled: program=%d", shaderProgram);
    return true;
}

// ============================================================================
// Hook RwCamera Render
// ============================================================================

DECL_HOOK(void*, _rwCameraValRender, void* camera) {
    // Init shader first time OpenGL context ready
    if(!bSSAOInitialized) {
        const char* ext = (const char*)glGetString(GL_EXTENSIONS);
        if(ext) {
            logger->Info("OpenGL ready!");
            
            if(strstr(ext, "GL_OES_depth_texture")) {
                logger->Info("✓ Depth texture supported");
            } else {
                logger->Info("✗ Depth texture NOT supported - using fake AO");
            }
            
            if(InitSSAOShader()) {
                bSSAOInitialized = true;
                logger->Info("SSAO initialized!");
            }
        }
    }
    
    // Render camera normally
    void* result = _rwCameraValRender(camera);
    
    // Apply SSAO post-process here (if needed)
    // if(bSSAOInitialized && shaderProgram) {
    //     ApplySSAO();
    // }
    
    return result;
}

// ============================================================================
// Mod Entry Point
// ============================================================================

extern "C" void OnModLoad() {
    logger->SetTag("SSAO");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if (!pGTASA || !hGTASA) {
        logger->Error("GTA:SA not found!");
        return;
    }
    
    // Hook _rwCameraValRender (offset dari symbol lu)
    HOOK(_rwCameraValRender, pGTASA + 0x001d7140);
    
    logger->Info("SSAO loaded - hooked RwCamera render");
}
