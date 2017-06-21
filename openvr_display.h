#pragma once

#ifdef OPENVR_ENABLED

#include <array>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <openvr.h>
#include <GL/gl3w.h>

struct GLFramebuffer {
	GLuint fb;
	std::unordered_map<GLenum, GLuint> attachments;

	GLFramebuffer();
	// Destroys the framebuffer and all attached textures
	~GLFramebuffer();
	void attach2d(GLenum attachment, GLuint texture);
	void detach2d(GLenum attachment);
};

struct EyeFBDesc {
	// We're not doing MSAA so we don't have a resolve
	// TODO: If we start putting in the controllers we
	// may want to have MSAA for that.
	GLFramebuffer render;
};

struct HMDMatrices {
	std::array<glm::mat4, 2> head_to_eyes;
	std::array<glm::mat4, 2> projection_eyes;
	glm::mat4 absolute_to_device;
};

struct OpenVRDisplay {
	OpenVRDisplay();
	~OpenVRDisplay();
	// Begin rendering a new frame, waits for tracked device poses and
	// updates the HMD transform
	void begin_frame();
	// Start rendering a specific eye and get back the view & projection
	// matrices to use for it
	void begin_eye(size_t i, glm::mat4 &view, glm::mat4 &proj);
	// Submit both rendered eyes to the HMD
	void submit();

	vr::IVRSystem *system;
	vr::IVRCompositor *compositor;
	std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> tracked_devices;
	std::array<EyeFBDesc, 2> eye_fbs;
	HMDMatrices hmd_mats;
	std::array<uint32_t, 2> render_dims;
};

#endif

