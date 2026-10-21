// GPU side of the PiP: uploads the forward cameras' frames, reprojects them into a rectilinear view with a compute
// shader and hands the result to SteamVR as a Vulkan overlay texture.
#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include "calibration.hpp"
#include "openvr.h"
#include "tracking_cameras.hpp"

namespace framepip {

struct ViewParams {
  double hfovDeg = 100;  // horizontal field of view of the PiP image
  double gain = 1;       // brightness multiplier
  double highlight = 0;  // 0..1: a light frame around the image (a controller is aiming at it)
};

class PipRenderer {
 public:
  static constexpr int kWidth = 800, kHeight = 600;

  ~PipRenderer();
  // Vulkan instance/device on SteamVR's GPU. Needed by both InitCameras and UploadPanel.
  bool InitDevice(std::string *error);
  // cams: the (up to two) cameras the frames will come from, in the same order as Render's images. The first is the
  // primary camera; the second only fills in where the first can't see.
  bool InitCameras(const std::vector<CameraModel> &cams, std::string *error);
  // Renders the frames; on success fills *texture (pointing into this renderer) for IVROverlay::SetOverlayTexture.
  bool Render(const std::vector<CameraImage> &images, const ViewParams &view, vr::Texture_t *texture);
  // Uploads an RGBA8 image (the dashboard panel) into one of two alternating textures. Unlike SetOverlayRaw, the
  // overlay never shows a half-updated or missing image while it changes.
  bool UploadPanel(const uint8_t *rgba, int width, int height, vr::Texture_t *texture);

 private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
  };
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  bool CreateDevice(std::string *error);
  bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer *out);
  bool CreateImage(int w, int h, VkFormat format, VkImageUsageFlags usage, Image *out);
  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags props) const;
  void Destroy(Buffer *b);
  void Destroy(Image *i);
  // Records an image layout transition into cmd_
  void Barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
               VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
  bool SubmitAndWait();
  void FillTexture(vr::VRVulkanTextureData_t *data, VkImage image, int w, int h, vr::Texture_t *texture);

  std::vector<CameraModel> cams_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkPhysicalDeviceMemoryProperties memProps_{};

  Image camImages_[2];
  Buffer staging_[2];
  Image outImages_[2];
  Buffer params_[2];
  int next_ = 0;  // output image to render into next (the compositor may still read the other)

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet sets_[2] = {};
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;

  vr::VRVulkanTextureData_t vkTexture_{};

  Buffer panelStaging_;
  Image panelImages_[2];
  int panelWidth_ = 0, panelHeight_ = 0, panelNext_ = 0;
  vr::VRVulkanTextureData_t panelTexture_{};
};

}  // namespace framepip
