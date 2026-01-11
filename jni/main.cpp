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
    int samples = 4;
    
    for(int i = 0; i < 4; i++) {
        float angle = float(i) * 1.5708;
        vec2 offset = vec2(cos(angle), sin(angle)) * radius * uPixelSize;
        
        float sampleDepth = getDepth(vTexCoord + offset);
        float diff = centerDepth - sampleDepth;
        
        ao += max(0.0, diff) * 0.5;
    }
    
    ao = clamp(ao / float(samples), 0.0, 1.0);
    float occlusion = 1.0 - (ao * uIntensity);
    
    gl_FragColor = vec4(sceneColor * occlusion, 1.0);
}
)";

// ============================================================================
// OpenGL Resources
// ============================================================================

GLuint shaderProgram = 0;
GLuint vbo = 0;
GLint uSceneTex = -1;
GLint uPixelSize = -1;
GLint uIntensity = -1;
GLint aPosition = -1;
GLint aTexCoord = -1;

int screenWidth = 1280;
int screenHeight = 720;
float aoIntensity = 0.5f;

float quadVertices[] = {
    -1.0f,  1.0f,  0.0f, 1.0f,
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f
};

// ============================================================================
// Shader Compilation
// ============================================================================

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        logger->Error("Shader compilation failed: %s", infoLog);
        return 0;
    }
    
    return shader;
}

bool InitSSAOShader() {
    GLuint vertShader = CompileShader(GL_VERTEX_SHADER, vertexShaderSrc);
    GLuint fragShader = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSrc);
    
    if (!vertShader || !fragShader) {
        logger->Error("Failed to compile shaders!");
        return false;
    }
    
    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertShader);
    glAttachShader(shaderProgram, fragShader);
    glLinkProgram(shaderProgram);
    
    GLint success;
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        logger->Error("Program linking failed: %s", infoLog);
        return false;
    }
    
    glDeleteShader(vertShader);
    glDeleteShader(fragShader);
    
    uSceneTex = glGetUniformLocation(shaderProgram, "uSceneTex");
    uPixelSize = glGetUniformLocation(shaderProgram, "uPixelSize");
    uIntensity = glGetUniformLocation(shaderProgram, "uIntensity");
    aPosition = glGetAttribLocation(shaderProgram, "aPosition");
    aTexCoord = glGetAttribLocation(shaderProgram, "aTexCoord");
    
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    
    logger->Info("SSAO Shader initialized successfully!");
    return true;
}

void ApplySSAO() {
    if (shaderProgram == 0) return;
    
    glUseProgram(shaderProgram);
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    
    glEnableVertexAttribArray(aPosition);
    glVertexAttribPointer(aPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    
    glEnableVertexAttribArray(aTexCoord);
    glVertexAttribPointer(aTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    
    glUniform1i(uSceneTex, 0);
    glUniform2f(uPixelSize, 1.0f / screenWidth, 1.0f / screenHeight);
    glUniform1f(uIntensity, aoIntensity);
    
    glDrawArrays(GL_TRIANGLES, 0, 6);
    
    glDisableVertexAttribArray(aPosition);
    glDisableVertexAttribArray(aTexCoord);
}

// ============================================================================
// Game Hooks
// ============================================================================

DECL_HOOK(void, RenderScene, bool flag) {
    if(shaderProgram == 0) InitSSAOShader();
    
    RenderScene(flag);
    
    // Apply SSAO (simplified - needs proper FBO setup for real SSAO)
    // ApplySSAO();
}

// ============================================================================
// Mod Entry Point
// ============================================================================

extern "C" void OnModPreLoad() {
    logger->SetTag("SSAO");
    logger->Info("SSAO Mod PreLoad");
}

extern "C" void OnModLoad() {
    logger->SetTag("SSAO");
    
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    
    if (!pGTASA || !hGTASA) {
        logger->Error("Failed to get GTA:SA library!");
        return;
    }
    
    const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
    logger->Info("OpenGL Extensions: %s", extensions);
    
    if (strstr(extensions, "GL_OES_depth_texture") == NULL) {
        logger->Error("GL_OES_depth_texture NOT supported!");
        logger->Error("Real SSAO won't work - using fake AO instead");
    }
    
    HOOK(RenderScene, pGTASA + 0x003f609c);
    
    logger->Info("SSAO Mod loaded successfully!");
}
