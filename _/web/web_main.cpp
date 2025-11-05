// Web-specific entry point for DingCAD
// This file provides web-compatible implementations for filesystem operations
// and main loop management

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/fetch.h>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <limits>

// Forward declarations from main.cpp
extern "C" {
#include "quickjs.h"
}
#include "manifold/manifold.h"
#include "js_bindings.h"
#include "version.h"

// Include raylib headers for Model type (needed for forward declarations)
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

// Web-specific state
struct WebState {
  JSRuntime* runtime = nullptr;
  std::shared_ptr<manifold::Manifold> scene = nullptr;
  std::string sceneCode;
  std::string statusMessage;
  bool needsReload = false;
};

static WebState g_webState;

// Web-compatible file reading using Emscripten FS
std::string ReadFileFromVirtualFS(const std::string& path) {
  // Try to read from Emscripten's virtual filesystem
  FILE* file = fopen(path.c_str(), "r");
  if (!file) {
    return "";
  }
  
  std::string content;
  char buffer[4096];
  while (fgets(buffer, sizeof(buffer), file)) {
    content += buffer;
  }
  fclose(file);
  return content;
}

// Forward declarations
struct LoadResult {
  bool success = false;
  std::shared_ptr<manifold::Manifold> manifold;
  std::string message;
};
LoadResult LoadSceneFromCode(JSRuntime* runtime, const std::string& code);
void ReplaceScene(Model &model, const std::shared_ptr<manifold::Manifold> &scene);

// Global state forward declarations (will be defined later)
extern Model g_model;
extern bool g_windowInitialized;
extern bool g_sceneLoaded;

// Load scene from JavaScript code string
extern "C" {
EMSCRIPTEN_KEEPALIVE
void loadSceneFromCode(const char* code) {
  if (!code) {
    g_webState.statusMessage = "Error: No code provided";
    return;
  }
  
  g_webState.sceneCode = code;
  g_webState.needsReload = true;
  g_webState.statusMessage = "Loading scene...";
  
  // Load the scene immediately instead of waiting for render frame
  if (g_webState.runtime) {
    auto loadResult = LoadSceneFromCode(g_webState.runtime, g_webState.sceneCode);
    if (loadResult.success) {
      g_webState.scene = loadResult.manifold;
      g_webState.statusMessage = loadResult.message;
      // Replace the model immediately if window is initialized, otherwise it will be done in renderFrame
      if (g_windowInitialized) {
        ReplaceScene(g_model, g_webState.scene);
        g_sceneLoaded = true;
      } else {
        // Window not initialized yet, will be replaced in renderFrame
        g_sceneLoaded = true;
      }
    } else {
      g_webState.statusMessage = "Error: " + loadResult.message;
      g_sceneLoaded = false;
    }
    g_webState.needsReload = false;
  } else {
    g_webState.statusMessage = "Error: Runtime not initialized";
  }
}

EMSCRIPTEN_KEEPALIVE
const char* getStatusMessage() {
  static std::string msg;
  msg = g_webState.statusMessage.empty() ? "Ready" : g_webState.statusMessage;
  return msg.c_str();
}

EMSCRIPTEN_KEEPALIVE
void logBuildVersion() {
  // This function is called from JavaScript to log build version
  // The version is embedded in the compiled binary
  #ifdef BUILD_VERSION
  printf("DingCAD Web Build: %s\n", BUILD_VERSION);
  #else
  printf("DingCAD Web Build: unknown\n");
  #endif
}
}

// Web-compatible module loader
JSModuleDef* WebModuleLoader(JSContext* ctx, const char* module_name, void* opaque) {
  // For web, we'll load from the virtual filesystem or from provided code
  std::string content;
  
  // First try virtual filesystem
  content = ReadFileFromVirtualFS(module_name);
  
  // If not found and it's the main scene, use the provided code
  if (content.empty() && strcmp(module_name, "scene.js") == 0 && !g_webState.sceneCode.empty()) {
    content = g_webState.sceneCode;
  }
  
  if (content.empty()) {
    JS_ThrowReferenceError(ctx, "Unable to load module '%s'", module_name);
    return nullptr;
  }
  
  JSValue funcVal = JS_Eval(ctx, content.c_str(), content.size(), module_name,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(funcVal)) {
    return nullptr;
  }
  
  auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(funcVal));
  JS_FreeValue(ctx, funcVal);
  return module;
}

// Scene loading function (adapted from main.cpp) - implementation
LoadResult LoadSceneFromCode(JSRuntime* runtime, const std::string& code) {
  LoadResult result;
  
  if (code.empty()) {
    result.message = "No scene code provided";
    return result;
  }
  
  JSContext* ctx = JS_NewContext(runtime);
  RegisterBindings(ctx);
  
  auto captureException = [&]() {
    JSValue exc = JS_GetException(ctx);
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    const char* stackStr = JS_ToCString(ctx, JS_IsUndefined(stack) ? exc : stack);
    result.message = stackStr ? stackStr : "JavaScript error";
    JS_FreeCString(ctx, stackStr);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
  };
  
  JSValue moduleFunc = JS_Eval(ctx, code.c_str(), code.size(), "scene.js",
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(moduleFunc)) {
    captureException();
    JS_FreeContext(ctx);
    return result;
  }
  
  if (JS_ResolveModule(ctx, moduleFunc) < 0) {
    captureException();
    JS_FreeValue(ctx, moduleFunc);
    JS_FreeContext(ctx);
    return result;
  }
  
  auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(moduleFunc));
  JSValue evalResult = JS_EvalFunction(ctx, moduleFunc);
  if (JS_IsException(evalResult)) {
    captureException();
    JS_FreeContext(ctx);
    return result;
  }
  JS_FreeValue(ctx, evalResult);
  
  JSValue moduleNamespace = JS_GetModuleNamespace(ctx, module);
  if (JS_IsException(moduleNamespace)) {
    captureException();
    JS_FreeContext(ctx);
    return result;
  }
  
  JSValue sceneVal = JS_GetPropertyStr(ctx, moduleNamespace, "scene");
  if (JS_IsException(sceneVal)) {
    JS_FreeValue(ctx, moduleNamespace);
    captureException();
    JS_FreeContext(ctx);
    return result;
  }
  JS_FreeValue(ctx, moduleNamespace);
  
  if (JS_IsUndefined(sceneVal)) {
    JS_FreeValue(ctx, sceneVal);
    JS_FreeContext(ctx);
    result.message = "Scene module must export 'scene'";
    return result;
  }
  
  auto sceneHandle = GetManifoldHandle(ctx, sceneVal);
  if (!sceneHandle) {
    JS_FreeValue(ctx, sceneVal);
    JS_FreeContext(ctx);
    result.message = "Exported 'scene' is not a manifold";
    return result;
  }
  
  result.manifold = sceneHandle;
  result.success = true;
  result.message = "Scene loaded successfully";
  JS_FreeValue(ctx, sceneVal);
  JS_FreeContext(ctx);
  return result;
}

// Helper function to create Raylib model from Manifold mesh
// Duplicated from main.cpp since it's in an anonymous namespace there
constexpr float kSceneScale = 0.1f;  // convert mm scene units to renderer units

Model CreateRaylibModelFrom(const manifold::MeshGL &meshGL) {
  Model model = {0};
  const int vertexCount = meshGL.NumVert();
  const int triangleCount = meshGL.NumTri();

  if (vertexCount <= 0 || triangleCount <= 0) {
    return model;
  }

  const int stride = meshGL.numProp;
  std::vector<Vector3> positions(vertexCount);
  for (int v = 0; v < vertexCount; ++v) {
    const int base = v * stride;
    // Convert from the scene's Z-up coordinates to raylib's Y-up system.
    const float cadX = meshGL.vertProperties[base + 0] * kSceneScale;
    const float cadY = meshGL.vertProperties[base + 1] * kSceneScale;
    const float cadZ = meshGL.vertProperties[base + 2] * kSceneScale;
    positions[v] = {cadX, cadZ, -cadY};
  }

  std::vector<Vector3> accum(vertexCount, {0.0f, 0.0f, 0.0f});
  for (int tri = 0; tri < triangleCount; ++tri) {
    const int i0 = meshGL.triVerts[tri * 3 + 0];
    const int i1 = meshGL.triVerts[tri * 3 + 1];
    const int i2 = meshGL.triVerts[tri * 3 + 2];

    const Vector3 p0 = positions[i0];
    const Vector3 p1 = positions[i1];
    const Vector3 p2 = positions[i2];

    const Vector3 u = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
    const Vector3 v = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
    const Vector3 n = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                       u.x * v.y - u.y * v.x};

    accum[i0].x += n.x;
    accum[i0].y += n.y;
    accum[i0].z += n.z;
    accum[i1].x += n.x;
    accum[i1].y += n.y;
    accum[i1].z += n.z;
    accum[i2].x += n.x;
    accum[i2].y += n.y;
    accum[i2].z += n.z;
  }

  std::vector<Vector3> normals(vertexCount);
  std::vector<Color> colors(vertexCount);
  const Vector3 lightDir = Vector3Normalize({0.45f, 0.85f, 0.35f});
  for (int v = 0; v < vertexCount; ++v) {
    normals[v] = Vector3Normalize(accum[v]);
    const float dot = Vector3DotProduct(normals[v], lightDir);
    const float intensity = std::max(0.3f, dot);
    colors[v] = {(unsigned char)(210 * intensity), 
                 (unsigned char)(210 * intensity), 
                 (unsigned char)(220 * intensity), 255};
  }

  Mesh mesh = {0};
  mesh.vertexCount = vertexCount;
  mesh.triangleCount = triangleCount;
  mesh.vertices = (float*)MemAlloc(vertexCount * 3 * sizeof(float));
  mesh.normals = (float*)MemAlloc(vertexCount * 3 * sizeof(float));
  mesh.colors = (unsigned char*)MemAlloc(vertexCount * 4 * sizeof(unsigned char));

  for (int v = 0; v < vertexCount; ++v) {
    mesh.vertices[v * 3 + 0] = positions[v].x;
    mesh.vertices[v * 3 + 1] = positions[v].y;
    mesh.vertices[v * 3 + 2] = positions[v].z;
    mesh.normals[v * 3 + 0] = normals[v].x;
    mesh.normals[v * 3 + 1] = normals[v].y;
    mesh.normals[v * 3 + 2] = normals[v].z;
    mesh.colors[v * 4 + 0] = colors[v].r;
    mesh.colors[v * 4 + 1] = colors[v].g;
    mesh.colors[v * 4 + 2] = colors[v].b;
    mesh.colors[v * 4 + 3] = colors[v].a;
  }

  mesh.indices = (unsigned short*)MemAlloc(triangleCount * 3 * sizeof(unsigned short));
  for (int tri = 0; tri < triangleCount; ++tri) {
    mesh.indices[tri * 3 + 0] = meshGL.triVerts[tri * 3 + 0];
    mesh.indices[tri * 3 + 1] = meshGL.triVerts[tri * 3 + 1];
    mesh.indices[tri * 3 + 2] = meshGL.triVerts[tri * 3 + 2];
  }

  UploadMesh(&mesh, false);
  model = LoadModelFromMesh(mesh);
  return model;
}

void ReplaceScene(Model &model, const std::shared_ptr<manifold::Manifold> &scene) {
  if (!scene) return;
  // Unload existing model if it has meshes
  if (model.meshCount > 0 && model.meshes != nullptr) {
    UnloadModel(model);
  }
  model = CreateRaylibModelFrom(scene->GetMeshGL());
}

// Global state for rendering
Model g_model = {0};
static Camera3D g_camera = {
  .position = {4.0f, 4.0f, 4.0f},
  .target = {0.0f, 0.5f, 0.0f},
  .up = {0.0f, 1.0f, 0.0f},
  .fovy = 45.0f,
  .projection = CAMERA_PERSPECTIVE
};
bool g_windowInitialized = false;
bool g_sceneLoaded = false;

// Render loop function for Emscripten
void renderFrame() {
  if (!g_windowInitialized) return;
  
  // Check if scene needs reload (fallback in case renderFrame is called before loadSceneFromCode)
  if (g_webState.needsReload && !g_webState.sceneCode.empty() && g_webState.runtime) {
    auto loadResult = LoadSceneFromCode(g_webState.runtime, g_webState.sceneCode);
    if (loadResult.success) {
      g_webState.scene = loadResult.manifold;
      g_webState.statusMessage = loadResult.message;
      ReplaceScene(g_model, g_webState.scene);
      g_sceneLoaded = true;
    } else {
      g_webState.statusMessage = "Error: " + loadResult.message;
      g_sceneLoaded = false;
    }
    g_webState.needsReload = false;
  }
  
  // If scene was loaded but model wasn't replaced yet (e.g., window wasn't initialized), do it now
  if (g_sceneLoaded && g_webState.scene && g_model.meshCount == 0) {
    ReplaceScene(g_model, g_webState.scene);
  }
  
  // Always render, even if no scene is loaded yet
  BeginDrawing();
  ClearBackground(RAYWHITE);
  
  BeginMode3D(g_camera);
  DrawGrid(40, 0.5f);
  
  if (g_model.meshCount > 0 && g_model.meshes != nullptr) {
    DrawModel(g_model, Vector3Zero(), 1.0f, WHITE);
  }
  
  EndMode3D();
  
  // Draw status message
  if (!g_webState.statusMessage.empty()) {
    DrawText(g_webState.statusMessage.c_str(), 10, 10, 20, DARKGRAY);
  } else {
    DrawText("Ready", 10, 10, 20, DARKGRAY);
  }
  
  EndDrawing();
}

int main() {
  // Initialize Raylib - window will be created from canvas element
  SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
  InitWindow(800, 600, "DingCAD Web");
  SetTargetFPS(60);
  g_windowInitialized = true;
  
  // Initialize QuickJS runtime
  g_webState.runtime = JS_NewRuntime();
  EnsureManifoldClass(g_webState.runtime);
  JS_SetModuleLoaderFunc(g_webState.runtime, nullptr, WebModuleLoader, nullptr);
  
  // Start Emscripten main loop
  emscripten_set_main_loop(renderFrame, 0, 1);
  
  return 0;
}

#endif // __EMSCRIPTEN__
