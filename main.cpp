// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include <string>
#include <array>
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <GL/gl3w.h>
#include <GLFW/glfw3.h>
#include <ospray/ospray.h>

#define STB_IMAGE_IMPLEMENTATION
// Just using STB image temporarily for developing
// the environment map viewer part of the code.
#include "stb_image.h"

// TODO: Just using this for temporary model loading,
// ideally we'd use OSPRay's app loaders if we can?
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

static const std::array<float, 42> CUBE_STRIP = {
	1, 1, -1,
	-1, 1, -1,
	1, 1, 1,
	-1, 1, 1,
	-1, -1, 1,
	-1, 1, -1,
	-1, -1, -1,
	1, 1, -1,
	1, -1, -1,
	1, 1, 1,
	1, -1, 1,
	-1, -1, 1,
	1, -1, -1,
	-1, -1, -1
};

const static std::string vsrc = R"(
#version 330 core
layout(location = 0) in vec3 pos;
uniform mat4 proj_view;
out vec3 vdir;
void main(void) {
	gl_Position = proj_view * vec4(pos, 1);
	vdir = pos.xyz;
}
)";

const static std::string fsrc = R"(
#version 330 core
uniform sampler2D envmap;
out vec4 color;
in vec3 vdir;
void main(void) {
	const float PI = 3.1415926535897932384626433832795;

	vec3 dir = normalize(vdir);
	// Note: The panoramic camera uses flipped theta/phi terminology
	// compared to wolfram alpha or other parametric sphere equations
	// In the map phi goes along x from [0, 2pi] and theta goes along y [0, pi]
	float u = (atan(dir.y, dir.x) + PI / 2) / (2 * PI);
	float v = acos(dir.z) / PI;
	color = texture(envmap, vec2(u, v));
}
)";


// phi/theta of the camera
float cam_phi = 0;
float cam_theta = 1.3;
std::array<bool, 4> key_down = {false, false, false, false};

const int PANORAMIC_HEIGHT = 1024;
const int PANORAMIC_WIDTH = 2 * PANORAMIC_HEIGHT;

struct AsyncRenderer {
	OSPRenderer renderer;
	OSPFrameBuffer fb;
	std::atomic<bool> should_quit, new_pixels;

	AsyncRenderer(OSPRenderer ren, OSPFrameBuffer fb);
	~AsyncRenderer();
	const uint32_t* map_fb();
	void unmap_fb();

private:
	std::mutex pixel_lock;
	std::vector<uint32_t> pixels;
	std::thread render_thread;

	void run();
};
AsyncRenderer::AsyncRenderer(OSPRenderer ren, OSPFrameBuffer fb)
	: renderer(ren), fb(fb), should_quit(false), new_pixels(false),
	pixels(PANORAMIC_WIDTH * PANORAMIC_HEIGHT, 0)
{
	render_thread = std::thread([&](){ run(); });
}
AsyncRenderer::~AsyncRenderer() {
	should_quit.store(true);
	render_thread.join();
}
const uint32_t* AsyncRenderer::map_fb() {
	pixel_lock.lock();
	new_pixels.store(false);
	return pixels.data();
}
void AsyncRenderer::unmap_fb() {
	pixel_lock.unlock();
}
void AsyncRenderer::run() {
	while (!should_quit.load()) {
		ospRenderFrame(fb, renderer, OSP_FB_COLOR);

		const uint32_t *data = static_cast<const uint32_t*>(ospMapFrameBuffer(fb, OSP_FB_COLOR));
		std::lock_guard<std::mutex> lock(pixel_lock);
		std::memcpy(pixels.data(), data, sizeof(uint32_t) * PANORAMIC_WIDTH * PANORAMIC_HEIGHT);
		new_pixels.store(true);
		ospUnmapFrameBuffer(data, fb);
	}
}

void key_callback(GLFWwindow *window, int key, int, int action, int);
GLuint load_texture(const std::string &file);
GLuint load_shader_program(const std::string &vshader_src, const std::string &fshader_src);

int main(int argc, const char **argv) {
	if (argc < 2) {
		std::cout << "Usage: ./osp360 <obj file>\n";
		return 1;
	}
	if (!glfwInit()) {
		return 1;
	}

	ospInit(&argc, argv);

	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	glfwWindowHint(GLFW_DOUBLEBUFFER, GL_TRUE);

	GLFWwindow *window = glfwCreateWindow(1280, 720, "osp360", nullptr, nullptr);
	if (!window) {
		glfwTerminate();
		return 1;
	}
	glfwSetKeyCallback(window, key_callback);
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);

	if (gl3wInit()) {
		throw std::runtime_error("Failed to init gl3w");
	}

	// Load the model w/ tinyobjloader
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string err;
	bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &err, argv[1],
			nullptr, true);
	if (!err.empty()) {
		std::cerr << "Error loading model: " << err << "\n";
	}
	if (!ret) {
		return 1;
	}

	OSPModel world = ospNewModel();
	// Load all the objects into ospray
	OSPData pos_data = ospNewData(attrib.vertices.size() / 3, OSP_FLOAT3,
			attrib.vertices.data(), OSP_DATA_SHARED_BUFFER);
	ospCommit(pos_data);
	for (size_t s = 0; s < shapes.size(); ++s) {
		std::cout << "Loading mesh " << shapes[s].name
			<< ", has " << shapes[s].mesh.indices.size() << " vertices\n";
		const tinyobj::mesh_t &mesh = shapes[s].mesh;
		std::vector<int32_t> indices;
		indices.reserve(mesh.indices.size());
		for (const auto &idx : mesh.indices) {
			indices.push_back(idx.vertex_index);
		}
		OSPData idx_data = ospNewData(indices.size() / 3, OSP_INT3, indices.data());
		ospCommit(idx_data);
		OSPGeometry geom = ospNewGeometry("triangles");
		ospSetObject(geom, "vertex", pos_data);
		ospSetObject(geom, "index", idx_data);
		ospCommit(geom);
		ospAddGeometry(world, geom);
	}
	ospCommit(world);

	OSPFrameBuffer framebuffer = ospNewFrameBuffer(osp::vec2i{PANORAMIC_WIDTH, PANORAMIC_HEIGHT},
			OSP_FB_SRGBA, OSP_FB_COLOR | OSP_FB_ACCUM);
	ospFrameBufferClear(framebuffer, OSP_FB_COLOR);
	OSPCamera camera = ospNewCamera("panoramic");
	// hard-coded for sponza
	ospSetVec3f(camera, "pos", osp::vec3f{21, 242, -49});
	ospSetVec3f(camera, "dir", osp::vec3f{0, 0, 1});
	ospSetVec3f(camera, "up", osp::vec3f{0, -1, 0});
	ospSet1f(camera, "fovy", 60.f);
	ospSet1f(camera, "aspect", 2.f);
	ospCommit(camera);

	OSPRenderer renderer = ospNewRenderer("ao8");
	ospSetObject(renderer, "model", world);
	ospSetObject(renderer, "camera", camera);
	ospSetVec3f(renderer, "bgColor", osp::vec3f{1, 1, 1});
	ospCommit(renderer);
	ospRenderFrame(framebuffer, renderer, OSP_FB_COLOR);

	// Render one initial frame then kick off the background rendering thread
	const uint32_t *data = static_cast<const uint32_t*>(ospMapFrameBuffer(framebuffer, OSP_FB_COLOR));
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PANORAMIC_WIDTH, PANORAMIC_HEIGHT, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	ospUnmapFrameBuffer(data, framebuffer);

	AsyncRenderer async_renderer(renderer, framebuffer);

	glClearColor(0, 0, 0, 1);
	glClearDepth(1);

	GLuint vao, vbo;
	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vbo);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * CUBE_STRIP.size(),
			CUBE_STRIP.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);

	GLuint shader = load_shader_program(vsrc, fsrc);
	glUseProgram(shader);

	const glm::mat4 proj = glm::perspective(glm::radians(60.f), 1280.f / 720.f, 0.1f, 10.f);
	glm::vec3 target_pos(std::cos(cam_phi) * std::sin(cam_theta),
			std::sin(cam_phi) * std::sin(cam_theta), std::cos(cam_theta));
	glm::mat4 view = glm::lookAt(glm::vec3(0), target_pos, glm::vec3(0, 0, 1));
	glm::mat4 proj_view = proj * view;
	glUniformMatrix4fv(glGetUniformLocation(shader, "proj_view"), 1, GL_FALSE,
			glm::value_ptr(proj_view));

	while (!glfwWindowShouldClose(window)) {
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, CUBE_STRIP.size() / 3);

		if (key_down[0]) {
			cam_theta -= 0.01;
		} else if (key_down[1]) {
			cam_theta += 0.01;
		}
		if (key_down[2]) {
			cam_phi += 0.01;
		} else if (key_down[3]) {
			cam_phi -= 0.01;
		}
		target_pos.x = std::cos(cam_phi) * std::sin(cam_theta);
		target_pos.y = std::sin(cam_phi) * std::sin(cam_theta);
		target_pos.z = std::cos(cam_theta);
		view = glm::lookAt(glm::vec3(0), target_pos, glm::vec3(0, 0, 1));
		proj_view = proj * view;
		glUniformMatrix4fv(glGetUniformLocation(shader, "proj_view"), 1, GL_FALSE,
				glm::value_ptr(proj_view));

		if (async_renderer.new_pixels.load()) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PANORAMIC_WIDTH, PANORAMIC_HEIGHT, 0,
					GL_RGBA, GL_UNSIGNED_BYTE, async_renderer.map_fb());
			async_renderer.unmap_fb();
		}

		glfwSwapBuffers(window);
		glfwPollEvents();
	}

	glDeleteProgram(shader);
	glDeleteTextures(1, &tex);
	glDeleteBuffers(1, &vbo);
	glDeleteVertexArrays(1, &vao);
	glfwDestroyWindow(window);
	return 0;
}
void key_callback(GLFWwindow *window, int key, int, int action, int) {
	const bool released = action == GLFW_RELEASE;
	switch (key) {
		case GLFW_KEY_ESCAPE:
			glfwSetWindowShouldClose(window, true);
			break;
		case GLFW_KEY_W:
			key_down[0] = !released;
			break;
		case GLFW_KEY_S:
			key_down[1] = !released;
			break;
		case GLFW_KEY_A:
			key_down[2] = !released;
			break;
		case GLFW_KEY_D:
			key_down[3] = !released;
			break;
		default: break;
	}
}
GLuint load_texture(const std::string &file) {
	// Load the image and force it to be RGB8 format
	int x, y, n;
	uint8_t *data = stbi_load(file.c_str(), &x, &y, &n, 3);
	if (!data) {
		throw std::runtime_error("Could not load image file: " + file);
	}

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, x, y, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	stbi_image_free(data);
	return tex;
}
GLuint compile_shader(const std::string &src, GLenum type) {
	GLuint shader = glCreateShader(type);
	const char *csrc = src.c_str();
	glShaderSource(shader, 1, &csrc, 0);
	glCompileShader(shader);

	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		std::cout << "Shader compilation error:\n";
		GLint len;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
		std::vector<char> log(len, 0);
		glGetShaderInfoLog(shader, log.size(), 0, log.data());
		std::cout << log.data() << "\n";
		throw std::runtime_error("Shader compilation failed");
	}
	return shader;
}
GLuint load_shader_program(const std::string &vshader_src, const std::string &fshader_src) {
	GLuint vs = compile_shader(vshader_src, GL_VERTEX_SHADER);
	GLuint fs = compile_shader(fshader_src, GL_FRAGMENT_SHADER);
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);

	GLint status;
	glGetProgramiv(prog, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		std::cout << "Shader link error:\n";
		GLint len;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
		std::vector<char> log(len, 0);
		glGetProgramInfoLog(prog, log.size(), 0, log.data());
		std::cout << log.data() << "\n";
		throw std::runtime_error("Shader link failed");
	}
	glDetachShader(prog, vs);
	glDetachShader(prog, fs);
	glDeleteShader(vs);
	glDeleteShader(fs);
	return prog;
}

