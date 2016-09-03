/*
The MIT License (MIT)

Copyright (c) 2016 Syoyo Fujita

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#define USE_OPENGL2
#include "OpenGLWindow/OpenGLInclude.h"
#ifdef _WIN32
#include "OpenGLWindow/Win32OpenGLWindow.h"
#elif defined __APPLE__
#include "OpenGLWindow/MacOpenGLWindow.h"
#else
// assume linux
#include "OpenGLWindow/X11OpenGLWindow.h"
#endif

#ifdef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_btgui.h"

#define TINY_DNG_LOADER_IMPLEMENTATION
#include "../../tiny_dng_loader.h"

b3gDefaultOpenGLWindow* window = 0;
int gWidth = 512;
int gHeight = 512;
int gMousePosX = -1, gMousePosY = -1;
bool gMouseLeftDown = false;

typedef struct {
  int width;
  int height;
  int bits;
  int components;
  tinydng::DNGInfo dng_info;

  // Decoded RAW data.
  std::vector<unsigned char> data;

  // HDR RAW data
  std::vector<float> image;

  // Developed image.
  std::vector<float> framebuffer;

} RAWImage;

typedef struct {
  float intensity;
  bool flip_y;

  int view_offset[2];
  float display_gamma;
} UIParam;

RAWImage gRAWImage;
UIParam gUIParam;

struct Vertex {
  GLfloat pos[2];
  GLfloat texcoord[2];
};
static Vertex QUAD[4] = {{{-1.0f, -1.0f}, {0.0, 0.0}},
                         {{1.0f, -1.0f}, {1.0, 0.0}},
                         {{-1.0f, 1.0f}, {0.0, 1.0}},
                         {{1.0f, 1.0f}, {1.0, 1.0}}};

static const char gVertexShaderStr[] =
    "#version 120\n"
    "attribute vec2 pos;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 vTexcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "    vTexcoord = texcoord;\n"
    "}\n";

static const char gFragmentShaderStr[] =
    "#version 120\n"
    "varying vec2 vTexcoord;\n"
    "uniform float uGamma;\n"
    "uniform vec2  uOffset;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "    vec3 col = texture2D(tex, vTexcoord + uOffset).rgb;"
    "    col = clamp(pow(col, vec3(uGamma)), 0.0, 1.0);"
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

typedef struct {
  GLint program;

  GLuint vb;

  GLint pos_attrib;
  GLint texcoord_attrib;

  GLint gamma_loc;
  GLint uv_offset_loc;
  GLint tex_loc;

  GLuint tex_id;
} GLContext;

GLContext gGLCtx;

static inline unsigned short swap2(unsigned short val) {
  unsigned short ret;

  char* buf = reinterpret_cast<char*>(&ret);

  short x = val;
  buf[1] = x;
  buf[0] = x >> 8;

  return ret;
}

//
// Decode 12bit integer image into floating point HDR image
//
void decode12_hdr(std::vector<float>& image, unsigned char* data, int width,
                  int height, bool do_swap) {
  int offsets[2][2] = {{0, 1}, {1, 2}};

  int bit_shifts[2] = {4, 0};

  image.resize(width * height);

#pragma omp parallel for schedule(dynamic, 1)
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned char buf[3];

      // Calculate load addres for 12bit pixel(three 8 bit pixels)
      int n = int(y * width + x);

      // 24 = 12bit * 2 pixel, 8bit * 3 pixel
      int n2 = n % 2;           // used for offset & bitshifts
      int addr3 = (n / 2) * 3;  // 8bit pixel pos
      int odd = (addr3 % 2);

      int bit_shift;
      bit_shift = bit_shifts[n2];

      int offset[2];
      offset[0] = offsets[n2][0];
      offset[1] = offsets[n2][1];

      if (do_swap) {
        // load with short byte swap
        if (odd) {
          buf[0] = data[addr3 - 1];
          buf[1] = data[addr3 + 2];
          buf[2] = data[addr3 + 1];
        } else {
          buf[0] = data[addr3 + 1];
          buf[1] = data[addr3 + 0];
          buf[2] = data[addr3 + 3];
        }
      } else {
        buf[0] = data[addr3 + 0];
        buf[1] = data[addr3 + 1];
        buf[2] = data[addr3 + 2];
      }
      unsigned int b0 = (unsigned int)buf[offset[0]] & 0xff;
      unsigned int b1 = (unsigned int)buf[offset[1]] & 0xff;

      unsigned int val = (b0 << 8) | b1;
      val = 0xfff & (val >> bit_shift);

      image[y * width + x] = (float)val;
    }
  }
}

//
// Decode 14bit integer image into floating point HDR image
//
void decode14_hdr(std::vector<float>& image, unsigned char* data, int width,
                  int height, bool do_swap) {
  int offsets[4][3] = {{0, 0, 1}, {1, 2, 3}, {3, 4, 5}, {5, 5, 6}};

  int bit_shifts[4] = {2, 4, 6, 0};

  image.resize(width * height);

#pragma omp parallel for schedule(dynamic, 1)
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned char buf[7];

      // Calculate load addres for 14bit pixel(three 8 bit pixels)
      int n = int(y * width + x);

      // 56 = 14bit * 4 pixel, 8bit * 7 pixel
      int n4 = n % 4;           // used for offset & bitshifts
      int addr7 = (n / 4) * 7;  // 8bit pixel pos
      int odd = (addr7 % 2);

      int offset[3];
      offset[0] = offsets[n4][0];
      offset[1] = offsets[n4][1];
      offset[2] = offsets[n4][2];

      int bit_shift;
      bit_shift = bit_shifts[n4];

      if (do_swap) {
        // load with short byte swap
        if (odd) {
          buf[0] = data[addr7 - 1];
          buf[1] = data[addr7 + 2];
          buf[2] = data[addr7 + 1];
          buf[3] = data[addr7 + 4];
          buf[4] = data[addr7 + 3];
          buf[5] = data[addr7 + 6];
          buf[6] = data[addr7 + 5];
        } else {
          buf[0] = data[addr7 + 1];
          buf[1] = data[addr7 + 0];
          buf[2] = data[addr7 + 3];
          buf[3] = data[addr7 + 2];
          buf[4] = data[addr7 + 5];
          buf[5] = data[addr7 + 4];
          buf[6] = data[addr7 + 7];
        }
      } else {
        memcpy(buf, &data[addr7], 7);
      }
      unsigned int b0 = (unsigned int)buf[offset[0]] & 0xff;
      unsigned int b1 = (unsigned int)buf[offset[1]] & 0xff;
      unsigned int b2 = (unsigned int)buf[offset[2]] & 0xff;

      // unsigned int val = (b0 << 16) | (b1 << 8) | b2;
      // unsigned int val = (b2 << 16) | (b0 << 8) | b0;
      unsigned int val = (b0 << 16) | (b1 << 8) | b2;
      // unsigned int val = b2;
      val = 0x3fff & (val >> bit_shift);

      image[y * width + x] = (float)val;
    }
  }
}

//
// Decode 16bit integer image into floating point HDR image
//
void decode16_hdr(std::vector<float>& image, unsigned char* data, int width,
                  int height, bool do_swap) {
  image.resize(width * height);
  unsigned short* ptr = reinterpret_cast<unsigned short*>(data);

#pragma omp parallel for schedule(dynamic, 1)
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned short val = ptr[y * width + x];
      if (do_swap) {
        val = swap2(val);
      }

      // range will be [0, 65535]
      image[y * width + x] = (float)val;
    }
  }
}

void DecodeToHDR(RAWImage* raw, bool swap_endian) {
  raw->image.resize(raw->width * raw->height);

  if (raw->bits == 12) {
    decode12_hdr(raw->image, raw->data.data(), raw->width, raw->height,
                 swap_endian);
  } else if (raw->bits == 14) {
    decode14_hdr(raw->image, raw->data.data(), raw->width, raw->height,
                 swap_endian);
  } else if (raw->bits == 16) {
    decode16_hdr(raw->image, raw->data.data(), raw->width, raw->height,
                 swap_endian);
  } else {
    assert(0);
    exit(-1);
  }
}

// @todo { debayer, color correction, etc. }
void Develop(RAWImage* raw, float intensity, bool flipY) {
  if (raw->framebuffer.size() != (raw->width * raw->height * 3)) {
    raw->framebuffer.resize(raw->width * raw->height * 3);
  }

  // Simply map raw pixel value to [0.0, 1.0] range.
  const float inv_scale =
      1.0f / (raw->dng_info.white_level - raw->dng_info.black_level);

  // Assume src is grayscale image.
  for (size_t y = 0; y < raw->height; y++) {
    for (size_t x = 0; x < raw->width; x++) {
      float value =
          (raw->image[y * raw->width + x] - raw->dng_info.black_level) *
          inv_scale;

      int Y = (flipY) ? (raw->height - y - 1) : y;

      // Simply show grayscale.
      raw->framebuffer[3 * (Y * raw->width + x) + 0] = intensity * value;
      raw->framebuffer[3 * (Y * raw->width + x) + 1] = intensity * value;
      raw->framebuffer[3 * (Y * raw->width + x) + 2] = intensity * value;
    }
  }

  // Upload to GL texture.
  if (gGLCtx.tex_id > 0) {
    glBindTexture(GL_TEXTURE_2D, gGLCtx.tex_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, raw->width, raw->height, GL_RGB,
                    GL_FLOAT, raw->framebuffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);
  }
}

void CheckGLError(std::string desc) {
  GLenum e = glGetError();
  if (e != GL_NO_ERROR) {
    fprintf(stderr, "OpenGL error in \"%s\": %d (%d)\n", desc.c_str(), e, e);
    exit(20);
  }
}

bool BindUniform(GLint& loc, GLuint progId, const char* name) {
  loc = glGetUniformLocation(progId, name);
  if (loc < 0) {
    fprintf(stderr, "Cannot find uniform: %s\n", name);
    return false;
  }
  return true;
}

GLuint CreateShader(GLenum shaderType, const char* src) {
  GLuint shader = glCreateShader(shaderType);
  if (!shader) {
    CheckGLError("glCreateShader");
    return 0;
  }
  glShaderSource(shader, 1, &src, NULL);

  GLint compiled = GL_FALSE;
  glCompileShader(shader);
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    GLint infoLogLen = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLen);
    if (infoLogLen > 0) {
      GLchar* infoLog = (GLchar*)malloc(infoLogLen);
      if (infoLog) {
        glGetShaderInfoLog(shader, infoLogLen, NULL, infoLog);
        fprintf(stderr, "Could not compile %s shader:\n%s\n",
                shaderType == GL_VERTEX_SHADER ? "vertex" : "fragment",
                infoLog);
        free(infoLog);
      }
    }
    glDeleteShader(shader);
    return 0;
  }

  return shader;
}

GLuint CreateProgram(const char* vtxSrc, const char* fragSrc) {
  GLuint vtxShader = 0;
  GLuint fragShader = 0;
  GLuint program = 0;
  GLint linked = GL_FALSE;

  vtxShader = CreateShader(GL_VERTEX_SHADER, vtxSrc);
  if (!vtxShader) goto exit;

  fragShader = CreateShader(GL_FRAGMENT_SHADER, fragSrc);
  if (!fragShader) goto exit;

  program = glCreateProgram();
  if (!program) {
    CheckGLError("glCreateProgram");
    goto exit;
  }
  glAttachShader(program, vtxShader);
  glAttachShader(program, fragShader);

  glLinkProgram(program);
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    fprintf(stderr, "Could not link program");
    GLint infoLogLen = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLogLen);
    if (infoLogLen) {
      GLchar* infoLog = (GLchar*)malloc(infoLogLen);
      if (infoLog) {
        glGetProgramInfoLog(program, infoLogLen, NULL, infoLog);
        fprintf(stderr, "Could not link program:\n%s\n", infoLog);
        free(infoLog);
      }
    }
    glDeleteProgram(program);
    program = 0;
  }

exit:
  glDeleteShader(vtxShader);
  glDeleteShader(fragShader);
  return program;
}

void InitGLDisplay(GLContext* ctx, int width, int height) {
  ctx->program = CreateProgram(gVertexShaderStr, gFragmentShaderStr);
  if (!ctx->program) {
    fprintf(stderr, "Failed to create shader program.");
    exit(-1);
  }

  // Attrib
  ctx->pos_attrib = glGetAttribLocation(ctx->program, "pos");
  ctx->texcoord_attrib = glGetAttribLocation(ctx->program, "texcoord");

  // uniform
  BindUniform(ctx->gamma_loc, ctx->program, "uGamma");
  BindUniform(ctx->uv_offset_loc, ctx->program, "uOffset");

  // Init texture for display.
  {
    glGenTextures(1, &ctx->tex_id);

    glBindTexture(GL_TEXTURE_2D, ctx->tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_FLOAT,
                 0);
    CheckGLError("glTexImage2D");

    glBindTexture(GL_TEXTURE_2D, 0);

    if (!BindUniform(ctx->tex_loc, ctx->program, "tex")) {
      fprintf(stderr, "failed to bind texture.");
    }
  }

  // Vertex buffer.
  glGenBuffers(1, &ctx->vb);
  glBindBuffer(GL_ARRAY_BUFFER, ctx->vb);
  glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), &QUAD[0], GL_STATIC_DRAW);
  CheckGLError("genBuffer");
}

void keyboardCallback(int keycode, int state) {
  printf("hello key %d, state %d(ctrl %d)\n", keycode, state,
         window->isModifierKeyPressed(B3G_CONTROL));
  // if (keycode == 'q' && window && window->isModifierKeyPressed(B3G_SHIFT)) {
  if (keycode == 27) {
    if (window) window->setRequestExit();
  }

  ImGui_ImplBtGui_SetKeyState(keycode, (state == 1));

  if (keycode >= 32 && keycode <= 126) {
    if (state == 1) {
      ImGui_ImplBtGui_SetChar(keycode);
    }
  }
}

void mouseMoveCallback(float x, float y) {
  if (gMouseLeftDown) {
    int dx = (int)x - gMousePosX;
    int dy = (int)y - gMousePosY;

    gUIParam.view_offset[0] -= dx;
    gUIParam.view_offset[1] += dy;
  }

  gMousePosX = (int)x;
  gMousePosY = (int)y;
}

void mouseButtonCallback(int button, int state, float x, float y) {
  ImGui_ImplBtGui_SetMouseButtonState(button, (state == 1));

  ImGuiIO& io = ImGui::GetIO();
  if (io.WantCaptureMouse || io.WantCaptureKeyboard) {
    return;
  }

  // left button
  if (button == 0) {
    if (state) {
      gMouseLeftDown = true;
    } else
      gMouseLeftDown = false;
  }
}

void resizeCallback(float width, float height) {
  GLfloat h = (GLfloat)height / (GLfloat)width;
  GLfloat xmax, znear, zfar;

  znear = 1.0f;
  zfar = 1000.0f;
  xmax = znear * 0.5f;

  gWidth = width;
  gHeight = height;
}

inline float pesudoColor(float v, int ch) {
  if (ch == 0) {  // red
    if (v <= 0.5f)
      return 0.f;
    else if (v < 0.75f)
      return (v - 0.5f) / 0.25f;
    else
      return 1.f;
  } else if (ch == 1) {  // green
    if (v <= 0.25f)
      return v / 0.25f;
    else if (v < 0.75f)
      return 1.f;
    else
      return 1.f - (v - 0.75f) / 0.25f;
  } else if (ch == 2) {  // blue
    if (v <= 0.25f)
      return 1.f;
    else if (v < 0.5f)
      return 1.f - (v - 0.25f) / 0.25f;
    else
      return 0.f;
  } else {  // alpha
    return 1.f;
  }
}

void Display(const GLContext& ctx, const UIParam& param) {
  // glRasterPos2i(pos_x, pos_y);
  // glDrawPixels(width, height, GL_RGB, GL_FLOAT,
  //             static_cast<const GLvoid*>(rgb));

  glUseProgram(ctx.program);
  CheckGLError("use_program");

  glUniform2f(ctx.uv_offset_loc,
              (float)param.view_offset[0] / (float)gRAWImage.width,
              (float)param.view_offset[1] / (float)gRAWImage.height);
  glUniform1f(ctx.gamma_loc, param.display_gamma);
  CheckGLError("uniform");

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, ctx.tex_id);
  glUniform1i(ctx.tex_loc, 0);  // texture slot 0.

  glBindBuffer(GL_ARRAY_BUFFER, ctx.vb);
  glVertexAttribPointer(ctx.pos_attrib, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        (const GLvoid*)offsetof(Vertex, pos));
  glVertexAttribPointer(ctx.texcoord_attrib, 2, GL_FLOAT, GL_FALSE,
                        sizeof(Vertex),
                        (const GLvoid*)offsetof(Vertex, texcoord));
  glEnableVertexAttribArray(ctx.pos_attrib);
  glEnableVertexAttribArray(ctx.texcoord_attrib);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  CheckGLError("draw");

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisableVertexAttribArray(ctx.pos_attrib);
  glDisableVertexAttribArray(ctx.texcoord_attrib);
  glUseProgram(0);

  glFlush();
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cout << "Needs input.dng" << std::endl;
    return EXIT_FAILURE;
  }

  std::string input_filename = std::string(argv[1]);

  // Init UI param
  {
    gUIParam.intensity = 1.0f;
    gUIParam.flip_y = true;
    gUIParam.view_offset[0] = 0;
    gUIParam.view_offset[1] = 0;
    gUIParam.display_gamma = 1.0f;
  }

  {
    int width;
    int height;
    int bits;
    int components;
    std::string err;
    tinydng::DNGInfo dng_info;
    std::vector<unsigned char> data;
    size_t data_len;
    bool ret =
        tinydng::LoadDNG(&dng_info, &data, &data_len, &width, &height, &bits,
                         &components, &err, input_filename.c_str());

    if (!err.empty()) {
      std::cout << err << std::endl;
    }

    if (ret == false) {
      std::cout << "failed to load DNG" << std::endl;
      return EXIT_FAILURE;
    }

    gRAWImage.width = width;
    gRAWImage.height = height;
    gRAWImage.bits = bits;
    gRAWImage.data = data;
    gRAWImage.dng_info = dng_info;

    DecodeToHDR(&gRAWImage, /* endian*/ false);  // @fixme { detect endian }
  }

  window = new b3gDefaultOpenGLWindow;
  b3gWindowConstructionInfo ci;
#ifdef USE_OPENGL2
  ci.m_openglVersion = 2;
#endif
  ci.m_width = gRAWImage.width;
  ci.m_height = gRAWImage.height;
  window->createWindow(ci);

  window->setWindowTitle("view");

#ifndef __APPLE__
#ifndef _WIN32
  // some Linux implementations need the 'glewExperimental' to be true
  glewExperimental = GL_TRUE;
#endif
  if (glewInit() != GLEW_OK) {
    fprintf(stderr, "Failed to initialize GLEW\n");
    exit(-1);
  }

  if (!GLEW_VERSION_2_1) {
    fprintf(stderr, "OpenGL 2.1 is not available\n");
    exit(-1);
  }
#endif

  CheckGLError("init");

  InitGLDisplay(&gGLCtx, gRAWImage.width, gRAWImage.height);
  CheckGLError("initDisplay");

  window->setMouseButtonCallback(mouseButtonCallback);
  window->setMouseMoveCallback(mouseMoveCallback);
  window->setKeyboardCallback(keyboardCallback);
  window->setResizeCallback(resizeCallback);

  ImGui_ImplBtGui_Init(window);

  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->AddFontDefault();

  Develop(&gRAWImage, gUIParam.intensity, gUIParam.flip_y);

  while (!window->requestedExit()) {
    window->startRendering();

    CheckGLError("begin frame");

    ImGui_ImplBtGui_NewFrame(gMousePosX, gMousePosY);
    ImGui::Begin("UI");
    {
      if (ImGui::SliderFloat("intensity", &gUIParam.intensity, 0.0f, 10.0f)) {
        Develop(&gRAWImage, gUIParam.intensity, gUIParam.flip_y);
      }
      if (ImGui::Checkbox("flip Y", &gUIParam.flip_y)) {
        Develop(&gRAWImage, gUIParam.intensity, gUIParam.flip_y);
      }
    }

    ImGui::End();

    // Draw image.
    {
      glViewport(0, 0, window->getWidth(), window->getHeight());
      glClearColor(0, 0.1, 0.2f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
              GL_STENCIL_BUFFER_BIT);

      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, window->getWidth(), 0, window->getHeight(), 0, 1);
      glMatrixMode(GL_MODELVIEW);

      CheckGLError("clear");

      Display(gGLCtx, gUIParam);

      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
    }

    // Draw ImGui
    ImGui::Render();

    CheckGLError("im render");

    window->endRendering();

    // Give some cycles to this thread.
    // std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  ImGui_ImplBtGui_Shutdown();
  delete window;

  return EXIT_SUCCESS;
}
