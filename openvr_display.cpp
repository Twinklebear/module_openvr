#include "openvr_display.h"

GLFramebuffer::GLFramebuffer() {
	glGenFramebuffers(1, &fb);
}
GLFramebuffer::~GLFramebuffer() {
	glDeleteFramebuffers(1, &fb);
	for (auto &t : attachments) {
		glDeleteTextures(1, &t.second);
	}
}
void GLFramebuffer::attach2d(GLenum attachment, GLuint texture) {
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture, 0);
	attachments[attachment] = texture;
	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		throw std::runtime_error("Framebuffer is incomplete!");
	}
}
void GLFramebuffer::detach2d(GLenum attachment) {
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
	attachments.erase(attachment);
}

// Convert an OpenVR HmdMatrix44_t to a glm::mat4
glm::mat4 hmd44_to_mat4(const vr::HmdMatrix44_t &m) {
	return glm::mat4(
			m.m[0][0], m.m[1][0], m.m[2][0], m.m[3][0],
			m.m[0][1], m.m[1][1], m.m[2][1], m.m[3][1],
			m.m[0][2], m.m[1][2], m.m[2][2], m.m[3][2],
			m.m[0][3], m.m[1][3], m.m[2][3], m.m[3][3]);
}
glm::mat4 hmd34_to_mat4(const vr::HmdMatrix34_t &m) {
	return glm::mat4(
			m.m[0][0], m.m[1][0], m.m[2][0], 0.f,
			m.m[0][1], m.m[1][1], m.m[2][1], 0.f,
			m.m[0][2], m.m[1][2], m.m[2][2], 0.f,
			m.m[0][3], m.m[1][3], m.m[2][3], 1.f);
}

OpenVRDisplay::OpenVRDisplay() {
	vr::EVRInitError vr_error;
	system = vr::VR_Init(&vr_error, vr::VRApplication_Scene);
	if (vr_error != vr::VRInitError_None) {
		throw std::runtime_error("Failed to init OpenVR");
	}
	if (!system->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd)) {
		throw std::runtime_error("HMD is not tracking, check connection");
	}
	compositor = vr::VRCompositor();
	if (!compositor) {
		throw std::runtime_error("Failed to init VR Compositor");
	}	
	system->GetRecommendedRenderTargetSize(&render_dims[0], &render_dims[1]);

	for (auto &eye : eye_fbs) {
		std::array<GLuint, 2> texs;
		glGenTextures(texs.size(), texs.data());
		for (auto &t : texs) {
			glBindTexture(GL_TEXTURE_2D, t);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		}
		glBindTexture(GL_TEXTURE_2D, texs[0]);
		// OSPRay is already doing sRGB correction, so don't do it twice.
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, render_dims[0], render_dims[1],
				0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glBindTexture(GL_TEXTURE_2D, texs[1]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, render_dims[0], render_dims[1],
				0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

		eye.render.attach2d(GL_COLOR_ATTACHMENT0, texs[0]);
		eye.render.attach2d(GL_DEPTH_ATTACHMENT, texs[1]);
	}
	hmd_mats.projection_eyes[0] = hmd44_to_mat4(system->GetProjectionMatrix(vr::Eye_Left, 0.01f, 10.f));
	hmd_mats.projection_eyes[1] = hmd44_to_mat4(system->GetProjectionMatrix(vr::Eye_Right, 0.01f, 10.f));

	hmd_mats.head_to_eyes[0] = glm::inverse(hmd34_to_mat4(system->GetEyeToHeadTransform(vr::Eye_Left)));
	hmd_mats.head_to_eyes[1] = glm::inverse(hmd34_to_mat4(system->GetEyeToHeadTransform(vr::Eye_Right)));
}
OpenVRDisplay::~OpenVRDisplay() {
	vr::VR_Shutdown();
}
void OpenVRDisplay::begin_frame() {
	compositor->WaitGetPoses(tracked_devices.data(), tracked_devices.size(), NULL, 0);
	hmd_mats.absolute_to_device = glm::inverse(hmd34_to_mat4(
				tracked_devices[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking));
}
void OpenVRDisplay::begin_eye(size_t i, glm::mat4 &view, glm::mat4 &proj) {
	glBindFramebuffer(GL_FRAMEBUFFER, eye_fbs[i].render.fb);
	glViewport(0, 0, render_dims[0], render_dims[1]);

	view = hmd_mats.head_to_eyes[i] * hmd_mats.absolute_to_device;
	proj = hmd_mats.projection_eyes[i];
}
void OpenVRDisplay::submit() {
	vr::Texture_t left_eye = {};
	left_eye.handle = (void*)eye_fbs[0].render.attachments[GL_COLOR_ATTACHMENT0];
	left_eye.eType = vr::TextureType_OpenGL;
	left_eye.eColorSpace = vr::ColorSpace_Gamma;

	vr::Texture_t right_eye = {};
	right_eye.handle = (void*)eye_fbs[1].render.attachments[GL_COLOR_ATTACHMENT0];
	right_eye.eType = vr::TextureType_OpenGL;
	right_eye.eColorSpace = vr::ColorSpace_Gamma;

	compositor->Submit(vr::Eye_Left, &left_eye, NULL, vr::Submit_Default);
	compositor->Submit(vr::Eye_Right, &right_eye, NULL, vr::Submit_Default);
	glFlush();
}

