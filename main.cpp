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
#include <sstream>

#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <GL/gl3w.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <ospray/ospray.h>

// sg stuff
#include "common/sg/SceneGraph.h"
#include "common/sg/Renderer.h"
#include "common/sg/importer/Importer.h"
#include "sg/common/TimeStamp.h"
#include "sg/common/FrameBuffer.h"
#include "ospcommon/FileName.h"
#include "ospcommon/networking/Socket.h"
#include "ospcommon/vec.h"
#include "sg/geometry/TriangleMesh.h"
#include "widgets/imguiViewerSg.h"

// TODO: Just using this for temporary model loading,
// ideally we'd use OSPRay's app loaders if we can?
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include "openvr_display.h"
#include "gldebug.h"


using namespace ospcommon;
using namespace ospray;

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


//
// sg stuff
//
std::vector<std::string> files;
std::string initialRendererType;
bool addPlane = true;
bool debug = false;
bool fullscreen = false;
bool print = false;

void parseCommandLine(int ac, const char **&av)
{
  for (int i = 1; i < ac; i++) {
    const std::string arg = av[i];
    if (arg == "-np" || arg == "--no-plane") {
      addPlane = false;
    } else if (arg == "-d" || arg == "--debug") {
      debug = true;
    } else if (arg == "-r" || arg == "--renderer") {
      initialRendererType = av[++i];
    } else if (arg == "-m" || arg == "--module") {
      ospLoadModule(av[++i]);
    } else if (arg == "--print") {
      print=true;
    } else if (arg == "--fullscreen") {
      fullscreen = true;
    } else if (arg[0] != '-') {
      files.push_back(av[i]);
    }
  }
}

//parse command line arguments containing the format:
//  -nodeName:...:nodeName=value,value,value
void parseCommandLineSG(int ac, const char **&av, sg::Node &root)
{
  for(int i=1;i < ac; i++) {
    std::string arg(av[i]);
    size_t f;
    std::string value("");
    if (arg.size() < 2 || arg[0] != '-')
      continue;

    while ((f = arg.find(":")) != std::string::npos ||
           (f = arg.find(",")) != std::string::npos) {
      arg[f] = ' ';
    }

    f = arg.find("=");
    if (f != std::string::npos)
      value = arg.substr(f+1,arg.size());

    if (value != "") {
      std::stringstream ss;
      ss << arg.substr(1,f-1);
      std::string child;
      std::reference_wrapper<sg::Node> node_ref = root;
      while (ss >> child) {
        node_ref = node_ref.get().childRecursive(child);
      }
      auto &node = node_ref.get();
      //Carson: TODO: reimplement with a way of determining type of node value
      //  currently relies on exception on value cast
      try {
        node.valueAs<std::string>();
        node.setValue(value);
      } catch(...) {};
      try {
        std::stringstream vals(value);
        float x;
        vals >> x;
        node.valueAs<float>();
        node.setValue(x);
      } catch(...) {}
      try {
        std::stringstream vals(value);
        int x;
        vals >> x;
        node.valueAs<int>();
        node.setValue(x);
      } catch(...) {}
      try {
        std::stringstream vals(value);
        bool x;
        vals >> x;
        node.valueAs<bool>();
        node.setValue(x);
      } catch(...) {}
      try {
        std::stringstream vals(value);
        float x,y,z;
        vals >> x >> y >> z;
        node.valueAs<ospcommon::vec3f>();
        node.setValue(ospcommon::vec3f(x,y,z));
      } catch(...) {}
      try {
        std::stringstream vals(value);
        int x,y;
        vals >> x >> y;
        node.valueAs<ospcommon::vec2i>();
        node.setValue(ospcommon::vec2i(x,y));
      } catch(...) {}
    }
  }
}

void addPlaneToScene(sg::Node& world)
{
  auto bbox = world.bounds();
  if (bbox.empty()) {
    bbox.lower = vec3f(-5,0,-5);
    bbox.upper = vec3f(5,10,5);
  }

  osp::vec3f *vertices = new osp::vec3f[4];
  float ps = bbox.upper.x*3.f;
  float py = bbox.lower.z-.1f;

  py = bbox.lower.y+0.01f;
  vertices[0] = osp::vec3f{-ps, py, -ps};
  vertices[1] = osp::vec3f{-ps, py, ps};
  vertices[2] = osp::vec3f{ps, py, -ps};
  vertices[3] = osp::vec3f{ps, py, ps};
  auto position = std::make_shared<sg::DataArray3f>((vec3f*)&vertices[0],
                                                    size_t(4),
                                                    false);
  osp::vec3i *triangles = new osp::vec3i[2];
  triangles[0] = osp::vec3i{0,1,2};
  triangles[1] = osp::vec3i{1,2,3};
  auto index = std::make_shared<sg::DataArray3i>((vec3i*)&triangles[0],
                                                 size_t(2),
                                                 false);
  auto &plane = world.createChild("plane", "Instance");
  auto &mesh  = plane.child("model").createChild("mesh", "TriangleMesh");

  std::shared_ptr<sg::TriangleMesh> sg_plane =
    std::static_pointer_cast<sg::TriangleMesh>(mesh.shared_from_this());
  sg_plane->vertex = position;
  sg_plane->index = index;
  auto &planeMaterial = mesh["material"];
  planeMaterial["Kd"].setValue(vec3f(0.5f));
  planeMaterial["Ks"].setValue(vec3f(0.6f));
  planeMaterial["Ns"].setValue(2.f);
}
//
// end sg stuff
//


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
  virtual void start()
  {
    render_thread = std::thread([&](){ run(); });
  }

protected:
	std::mutex pixel_lock;
	std::vector<uint32_t> pixels;
	std::thread render_thread;

  virtual void run();
};

AsyncRenderer::AsyncRenderer(OSPRenderer ren, OSPFrameBuffer fb)
	: renderer(ren), fb(fb), should_quit(false), new_pixels(false),
	pixels(PANORAMIC_WIDTH * PANORAMIC_HEIGHT, 0)
{
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

struct AsyncRendererSg : public AsyncRenderer
{
  AsyncRendererSg(const std::shared_ptr<sg::Node> sgRenderer)
    : sgRenderer(sgRenderer), AsyncRenderer(nullptr,nullptr)
  {
  }

  virtual void run() {
    while (!should_quit.load()) {
      //      ospRenderFrame(fb, renderer, OSP_FB_COLOR);
      auto &sgFB = sgRenderer->child("frameBuffer");
      auto sgFBptr =
          std::static_pointer_cast<sg::FrameBuffer>(sgFB.shared_from_this());

      static bool once = false;  //TODO: initial commit as timestamp can not
      // be set to 0
      //      if (sgFB.children527
      //      Modified() > lastFTime || !once) {
      //        auto &size = sgFB["size"];
      //        nPixels = size.valueAs<vec2i>().x * size.valueAs<vec2i>().y;
      //        pixelBuffer[0].resize(nPixels);
      //        pixelBuffer[1].resize(nPixels);
      //      }
      if (sgRenderer->childrenLastModified() > lastRTime || !once) {
        sgRenderer->traverse("verify");
        sgRenderer->traverse("commit");
      }
      once = true;
      lastRTime = sg::TimeStamp();
      sgRenderer->traverse("render");

      //      const uint32_t *data = static_cast<const uint32_t*>(ospMapFrameBuffer(fb, OSP_FB_COLOR));
      auto *data = (uint32_t*)sgFBptr->map();
      std::lock_guard<std::mutex> lock(pixel_lock);
      std::memcpy(pixels.data(), data, sizeof(uint32_t) * PANORAMIC_WIDTH * PANORAMIC_HEIGHT);
      new_pixels.store(true);
      sgFBptr->unmap(data);
//      ospUnmapFrameBuffer(data, fb);
    }
  }
  virtual void start()
  {
    render_thread = std::thread([&](){ run(); });
  }



protected:
  const std::shared_ptr<sg::Node> sgRenderer;
  sg::TimeStamp  lastRTime;
};

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

  // scene graph stuff

  parseCommandLine(argc, argv);
  auto renderer_ptr = sg::createNode("renderer", "Renderer");
  auto &renderer = *renderer_ptr;
  /*! the renderer we use for rendering on the display wall; null if
        no dw available */
//  std::shared_ptr<sg::Node> rendererDW;
  /*! display wall service info - ignore if 'rendererDW' is null */
  //    dw::ServiceInfo dwService;

  //    const char *dwNodeName = getenv("DISPLAY_WALL");
  //    if (dwNodeName) {
  //      std::cout << "#######################################################"
  //                << std::endl;
  //      std::cout << "found a DISPLAY_WALL environment variable ...." << std::endl;
  //      std::cout << "trying to connect to display wall service on "
  //                << dwNodeName << ":2903" << std::endl;

  //      dwService.getFrom(dwNodeName,2903);
  //      std::cout << "found display wall service on MPI port "
  //                << dwService.mpiPortName << std::endl;
  //      std::cout << "#######################################################"
  //                << std::endl;
  //      rendererDW = sg::createNode("renderer", "Renderer");
  //    }

  renderer["shadowsEnabled"].setValue(true);
  renderer["aoSamples"].setValue(1);
  renderer["aoDistance"].setValue(500.f);
  renderer["autoEpsilon"].setValue(false);
  auto panoramicCamera = sg::createNode("camera", "PanoramicCamera");
  auto perspectiveCamera = renderer["camera"].shared_from_this();
  renderer.setChild("camera", panoramicCamera);
  panoramicCamera->setParent(renderer);
//  renderer["camera"]["fovy"].setValue(60.f);
  panoramicCamera->child("pos").setValue(ospcommon::vec3f{21, 242, -49});
  panoramicCamera->child("dir").setValue(ospcommon::vec3f{0, 0, 1});
  panoramicCamera->child("up").setValue(ospcommon::vec3f{0, -1, 0});
  perspectiveCamera->child("pos").setValue(ospcommon::vec3f{21, 242, -49});
  perspectiveCamera->child("dir").setValue(ospcommon::vec3f{0, 0, 1});
  perspectiveCamera->child("up").setValue(ospcommon::vec3f{0, -1, 0});

  renderer["spp"].setValue(-2);
//  auto eye = ospcommon::vec3f{463, 149, 5.4};
//  auto at = ospcommon::vec3f{-17, 110, -18};
//  auto dir = at - eye;
//  dir = normalize(dir);
//    renderer["camera"]["pos"].setValue(eye);
//    renderer["camera"]["dir"].setValue(dir);
//    renderer["camera"]["up"].setValue(ospcommon::vec3f{0, 1, 0});


  renderer["frameBuffer"]["size"].setValue(ospcommon::vec2i(PANORAMIC_WIDTH, PANORAMIC_HEIGHT));

  //    if (rendererDW.get()) {
  //      rendererDW->child("shadowsEnabled").setValue(true);
  //      rendererDW->child("aoSamples").setValue(1);
  //      rendererDW->child("camera")["fovy"].setValue(60.f);
  //    }

  if (!initialRendererType.empty()) {
    renderer["rendererType"].setValue(initialRendererType);
    //      if (rendererDW.get()) {
    //        rendererDW->child("rendererType").setValue(initialRendererType);
    //      }
  }

  auto &lights = renderer["lights"];

  auto &sun = lights.createChild("sun", "DirectionalLight");
  sun["color"].setValue(vec3f(1.f,232.f/255.f,166.f/255.f));
  sun["direction"].setValue(vec3f(0.462f,-1.f,-.1f));
  sun["intensity"].setValue(2.5f);

  auto &bounce = lights.createChild("bounce", "DirectionalLight");
  bounce["color"].setValue(vec3f(127.f/255.f,178.f/255.f,255.f/255.f));
  bounce["direction"].setValue(vec3f(-.93,-.54f,-.605f));
  bounce["intensity"].setValue(1.25f);

  auto &ambient = lights.createChild("ambient", "AmbientLight");
  ambient["intensity"].setValue(3.9f);
  ambient["color"].setValue(vec3f(174.f/255.f,218.f/255.f,255.f/255.f));

  auto &world = renderer["world"];

  for (auto file : files) {
    FileName fn = file;
    auto importerNode_ptr = sg::createNode(fn.name(), "Importer");
    auto &importerNode = *importerNode_ptr;
    importerNode["fileName"].setValue(fn.str());
    world += importerNode_ptr;
  }

  parseCommandLineSG(argc, argv, renderer);

  //    if (rendererDW.get()) {
  //      rendererDW->setChild("world",  renderer["world"].shared_from_this());
  //      rendererDW->setChild("lights", renderer["lights"].shared_from_this());

  //      auto &frameBuffer = rendererDW->child("frameBuffer");
  //      frameBuffer["size"].setValue(dwService.totalPixelsInWall);
  //      frameBuffer["displayWall"].setValue(dwService.mpiPortName);
  //    }

  if (print || debug)
    renderer.traverse("print");

  renderer.traverse("verify");
  renderer.traverse("commit");
  renderer.traverse("render");

  std::cout << "sg init finished" << std::endl;

  //
  // end sg init
  //

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

//	OSPModel world = ospNewModel();
	// Load all the objects into ospray
//	OSPData pos_data = ospNewData(attrib.vertices.size() / 3, OSP_FLOAT3,
//			attrib.vertices.data(), OSP_DATA_SHARED_BUFFER);
//	ospCommit(pos_data);
//	for (size_t s = 0; s < shapes.size(); ++s) {
//		std::cout << "Loading mesh " << shapes[s].name
//			<< ", has " << shapes[s].mesh.indices.size() << " vertices\n";
//		const tinyobj::mesh_t &mesh = shapes[s].mesh;
//		std::vector<int32_t> indices;
//		indices.reserve(mesh.indices.size());
//		for (const auto &idx : mesh.indices) {
//			indices.push_back(idx.vertex_index);
//		}
//		OSPData idx_data = ospNewData(indices.size() / 3, OSP_INT3, indices.data());
//		ospCommit(idx_data);
//		OSPGeometry geom = ospNewGeometry("triangles");
//		ospSetObject(geom, "vertex", pos_data);
//		ospSetObject(geom, "index", idx_data);
//		ospCommit(geom);
//		ospAddGeometry(world, geom);
//	}
//	ospCommit(world);

//	OSPFrameBuffer framebuffer = ospNewFrameBuffer(osp::vec2i{PANORAMIC_WIDTH, PANORAMIC_HEIGHT},
//			OSP_FB_SRGBA, OSP_FB_COLOR | OSP_FB_ACCUM);
//	ospFrameBufferClear(framebuffer, OSP_FB_COLOR);

//	OSPCamera camera = ospNewCamera("panoramic");
//	// TODO: this is hard-coded for sponza
//	ospSetVec3f(camera, "pos", osp::vec3f{21, 242, -49});
//	ospSetVec3f(camera, "dir", osp::vec3f{0, 0, 1});
//	ospSetVec3f(camera, "up", osp::vec3f{0, -1, 0});
//	ospCommit(camera);

//	OSPRenderer renderer = ospNewRenderer("ao8");
//	ospSetObject(renderer, "model", world);
//	ospSetObject(renderer, "camera", camera);
//	ospSetVec3f(renderer, "bgColor", osp::vec3f{1, 1, 1});
//	ospCommit(renderer);

//	// Render one initial frame then kick off the background rendering thread
//	ospRenderFrame(framebuffer, renderer, OSP_FB_COLOR);
//	const uint32_t *data = static_cast<const uint32_t*>(ospMapFrameBuffer(framebuffer, OSP_FB_COLOR));
  auto sgFBptr =
      std::static_pointer_cast<sg::FrameBuffer>(renderer["frameBuffer"].shared_from_this());
  const uint32_t *data = (uint32_t*)sgFBptr->map();
	GLuint tex;
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PANORAMIC_WIDTH, PANORAMIC_HEIGHT, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  sgFBptr->unmap(data);
	glActiveTexture(GL_TEXTURE0);

//	AsyncRenderer async_renderer(renderer, framebuffer);
  std::cout << "starting async renderer" << std::endl;
  AsyncRendererSg async_renderer(renderer_ptr);
  async_renderer.start();
  std::cout << "done starting async renderer" << std::endl;

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

#ifdef OPENVR_ENABLED
	OpenVRDisplay vr_display;
#else
	const glm::mat4 proj_view = glm::perspective(glm::radians(65.f), 1280.f / 720.f, 0.01f, 10.f)
		* glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0));
	glUniformMatrix4fv(proj_view_unif, 1, GL_FALSE, glm::value_ptr(proj_view));
#endif

	bool quit = false;
  bool interactiveCamera = false;
  sg::TimeStamp lastRenderTime;
  sg::TimeStamp lastUpdateTime;
	while (!quit) {
		SDL_Event e;
    bool moved = false;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)){
				quit = true;
				break;
			}
      else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_UP){
        float stepsize = 5.f;
        renderer.setChild("camera", perspectiveCamera);
        auto eye = renderer["camera"]["pos"].valueAs<ospcommon::vec3f>();
        auto dir = renderer["camera"]["dir"].valueAs<ospcommon::vec3f>();
        eye += dir*stepsize;
        renderer["camera"]["pos"].setValue(eye);
        panoramicCamera->child("pos").setValue(eye);
        moved = true;
        interactiveCamera = true;
        lastUpdateTime = sg::TimeStamp();
        break;
      }
      else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_DOWN){
        float stepsize = -5.f;
        renderer.setChild("camera", perspectiveCamera);
        auto eye = renderer["camera"]["pos"].valueAs<ospcommon::vec3f>();
        auto dir = renderer["camera"]["dir"].valueAs<ospcommon::vec3f>();
        eye += dir*stepsize;
        renderer["camera"]["pos"].setValue(eye);
        panoramicCamera->child("pos").setValue(eye);
        interactiveCamera = true;
        moved = true;
        lastUpdateTime = sg::TimeStamp();
        break;
      }
		}
    if (!moved && interactiveCamera && lastRenderTime > (lastUpdateTime+5))
    {
      renderer.setChild("camera", panoramicCamera);
      panoramicCamera->markAsModified();
      panoramicCamera->setChildrenModified(sg::TimeStamp());
      interactiveCamera = false;
    }
		if (async_renderer.new_pixels.load()) {
			glActiveTexture(GL_TEXTURE1);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PANORAMIC_WIDTH, PANORAMIC_HEIGHT, 0,
					GL_RGBA, GL_UNSIGNED_BYTE, async_renderer.map_fb());
			async_renderer.unmap_fb();
			glActiveTexture(GL_TEXTURE0);
      lastRenderTime = sg::TimeStamp();
		}

#ifdef OPENVR_ENABLED
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
#endif

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

