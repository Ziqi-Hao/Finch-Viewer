#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <cmath>
#include <iostream>

namespace {

GLuint CompileShader(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::cerr << "shader compile failed: " << log << "\n";
  }
  return shader;
}

GLuint CreateProgram() {
  const char* vertexSource = R"glsl(
    #version 330 core
    layout(location = 0) in vec2 inPos;
    layout(location = 1) in vec3 inColor;
    out vec3 color;
    void main() {
      color = inColor;
      gl_Position = vec4(inPos, 0.0, 1.0);
    }
  )glsl";

  const char* fragmentSource = R"glsl(
    #version 330 core
    in vec3 color;
    out vec4 outColor;
    void main() {
      outColor = vec4(color, 1.0);
    }
  )glsl";

  const GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexSource);
  const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
  const GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    std::cerr << "program link failed: " << log << "\n";
  }
  return program;
}

}  // namespace

int main() {
  if (!glfwInit()) {
    std::cerr << "glfwInit failed\n";
    return 1;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  GLFWwindow* window =
      glfwCreateWindow(900, 650, "tractography GLFW probe", nullptr, nullptr);
  if (!window) {
    std::cerr << "glfwCreateWindow failed\n";
    glfwTerminate();
    return 1;
  }

  glfwMakeContextCurrent(window);
  glfwSwapInterval(0);

  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
    std::cerr << "gladLoadGLLoader failed\n";
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  std::cout << "GLFW probe window created\n";
  std::cout << "OpenGL vendor  : " << glGetString(GL_VENDOR) << "\n";
  std::cout << "OpenGL renderer: " << glGetString(GL_RENDERER) << "\n";
  std::cout << "OpenGL version : " << glGetString(GL_VERSION) << "\n";
  std::cout << "Press Esc or close the window to exit.\n";

  const float vertices[] = {
      -0.72f, -0.62f, 1.0f, 0.10f, 0.18f,
       0.72f, -0.62f, 0.15f, 0.95f, 0.35f,
       0.00f,  0.68f, 0.20f, 0.55f, 1.00f,
  };

  GLuint vao = 0;
  GLuint vbo = 0;
  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
      reinterpret_cast<void*>(2 * sizeof(float)));
  glEnableVertexAttribArray(1);

  const GLuint program = CreateProgram();

  while (!glfwWindowShouldClose(window)) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    glViewport(0, 0, width, height);

    const float t = static_cast<float>(glfwGetTime());
    glClearColor(0.04f + 0.03f * std::sin(t),
                 0.055f,
                 0.08f + 0.04f * std::cos(t * 0.7f),
                 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glfwSwapBuffers(window);
    glfwPollEvents();
  }

  glDeleteProgram(program);
  glDeleteBuffers(1, &vbo);
  glDeleteVertexArrays(1, &vao);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
