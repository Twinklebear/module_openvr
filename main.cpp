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
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <ospray/ospray.h>

// TODO: Just using this for temporary model loading,
// ideally we'd use OSPRay's app loaders if we can?
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include "openvr_display.h"
#include "gldebug.h"

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
	float u = (atan(dir.z, dir.x) + PI / 2) / (2 * PI);
	float v = acos(dir.y) / PI;
	color = texture(envmap, vec2(u, v));
}
)";


const int PANORAMIC_HEIGHT = 2048;
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

GLuint load_shader_program(const std::string &vshader_src, const std::string &fshader_src);

int main(int argc, const char **argv) {
	if (argc < 2) {
		std::cout << "Usage: ./osp360 <obj file>\n";
		return 1;
	}
	if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
		return 1;
	}

	ospInit(&argc, argv);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
	SDL_Window *window = SDL_CreateWindow("osp360", SDL_WINDOWPOS_CENTERED,
			SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_OPENGL);

	if (!window) {
		return 1;
	}
	SDL_GLContext ctx = SDL_GL_CreateContext(window);
	SDL_GL_SetSwapInterval(1);

	if (gl3wInit()) {
		throw std::runtime_error("Failed to init gl3w");
	}
	register_debug_callback();

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

	AsyncRenderer async_renderer(renderer, framebuffer);

	// Render one initial frame then kick off the background rendering thread
	const uint32_t *data = static_cast<const uint32_t*>(ospMapFrameBuffer(framebuffer, OSP_FB_COLOR));
	GLuint tex;
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PANORAMIC_WIDTH, PANORAMIC_HEIGHT, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	ospUnmapFrameBuffer(data, framebuffer);
	glActiveTexture(GL_TEXTURE0);

	glEnable(GL_DEPTH_TEST);
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

	glUniform1i(glGetUniformLocation(shader, "envmap"), 1);
	const GLuint proj_view_unif = glGetUniformLocation(shader, "proj_view");

	// TODO: We need to translate the sponza model so the head is at the middle of it
	// when we start using the head position for translation?

	OpenVRDisplay vr_display;

	bool quit = false;
	while (!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)){
				quit = true;
				break;
			}
		}
		if (async_renderer.new_pixels.load()) {
			glActiveTexture(GL_TEXTURE1);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PANORAMIC_WIDTH, PANORAMIC_HEIGHT, 0,
					GL_RGBA, GL_UNSIGNED_BYTE, async_renderer.map_fb());
			async_renderer.unmap_fb();
			glActiveTexture(GL_TEXTURE0);
		}

		vr_display.begin_frame();
		for (size_t i = 0; i < 2; ++i) {
			glm::mat4 proj, view;
			vr_display.begin_eye(i, view, proj);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

			// Remove translation from the view matrix
			view[3] = glm::vec4(0, 0, 0, 1);
			glm::mat4 proj_view = proj * view;
			glUniformMatrix4fv(proj_view_unif, 1, GL_FALSE, glm::value_ptr(proj_view));

			glDrawArrays(GL_TRIANGLE_STRIP, 0, CUBE_STRIP.size() / 3);
		}
		vr_display.submit();

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, 1280, 720);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, CUBE_STRIP.size() / 3);
		SDL_GL_SwapWindow(window);
	}

	glDeleteProgram(shader);
	glDeleteTextures(1, &tex);
	glDeleteBuffers(1, &vbo);
	glDeleteVertexArrays(1, &vao);

	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
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

