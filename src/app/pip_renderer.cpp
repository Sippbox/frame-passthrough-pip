#include "pip_renderer.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace framepip {
namespace {

const uint32_t kReprojectSpv[] = {
#include "reproject.comp.inc"
};

struct CamGpu {
  float axisX[4], axisY[4], axisZ[4], focal[4], k[4], size[4];
};
struct ParamsGpu {
  CamGpu cam[2];
  float view[4];
  float frame[4];  // highlight 0..1, border width (pixels)
};

std::vector<std::string> SplitSpaces(const std::string &s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string w;
  while (in >> w) out.push_back(w);
  return out;
}

void Put3(float *dst, const Vec3 &v) {
  dst[0] = (float)v[0];
  dst[1] = (float)v[1];
  dst[2] = (float)v[2];
  dst[3] = 0;
}

}  // namespace

PipRenderer::~PipRenderer() {
  if (!device_) {
    if (instance_) vkDestroyInstance(instance_, nullptr);
    return;
  }
  vkDeviceWaitIdle(device_);
  for (int i = 0; i < 2; i++) {
    Destroy(&camImages_[i]);
    Destroy(&staging_[i]);
    Destroy(&outImages_[i]);
    Destroy(&params_[i]);
    Destroy(&panelImages_[i]);
  }
  Destroy(&panelStaging_);
  if (fence_) vkDestroyFence(device_, fence_, nullptr);
  if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
  if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
  if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
  if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
  if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
  if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
  vkDestroyDevice(device_, nullptr);
  vkDestroyInstance(instance_, nullptr);
}

bool PipRenderer::CreateDevice(std::string *error) {
  // SteamVR names the instance/device extensions it needs to import our images.
  std::vector<std::string> instExt;
  if (vr::IVRCompositor *comp = vr::VRCompositor()) {
    uint32_t n = comp->GetVulkanInstanceExtensionsRequired(nullptr, 0);
    if (n > 1) {
      std::string s(n, '\0');
      comp->GetVulkanInstanceExtensionsRequired(s.data(), n);
      instExt = SplitSpaces(s.c_str());
    }
  }
  uint32_t availCount = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &availCount, nullptr);
  std::vector<VkExtensionProperties> avail(availCount);
  vkEnumerateInstanceExtensionProperties(nullptr, &availCount, avail.data());
  std::vector<const char *> enabled;
  for (const std::string &e : instExt)
    for (const VkExtensionProperties &a : avail)
      if (e == a.extensionName) enabled.push_back(a.extensionName);

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "framepip";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = (uint32_t)enabled.size();
  ici.ppEnabledExtensionNames = enabled.data();
  if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
    *error = "vkCreateInstance failed";
    return false;
  }

  uint64_t hmdDevice = 0;
  if (vr::VRSystem()) vr::VRSystem()->GetOutputDevice(&hmdDevice, vr::TextureType_Vulkan, instance_);
  physical_ = (VkPhysicalDevice)hmdDevice;
  if (!physical_) {
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(instance_, &n, &physical_) < 0 || !n) {
      *error = "no Vulkan device";
      return false;
    }
  }
  vkGetPhysicalDeviceMemoryProperties(physical_, &memProps_);

  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qf.data());
  bool found = false;
  for (uint32_t i = 0; i < qn && !found; i++)
    if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) queueFamily_ = i, found = true;
  for (uint32_t i = 0; i < qn && !found; i++)
    if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) queueFamily_ = i, found = true;
  if (!found) {
    *error = "no compute queue";
    return false;
  }

  std::vector<std::string> devExt;
  if (vr::IVRCompositor *comp = vr::VRCompositor()) {
    uint32_t n = comp->GetVulkanDeviceExtensionsRequired(physical_, nullptr, 0);
    if (n > 1) {
      std::string s(n, '\0');
      comp->GetVulkanDeviceExtensionsRequired(physical_, s.data(), n);
      devExt = SplitSpaces(s.c_str());
    }
  }
  uint32_t dn = 0;
  vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dn, nullptr);
  std::vector<VkExtensionProperties> davail(dn);
  vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dn, davail.data());
  std::vector<const char *> denabled;
  for (const std::string &e : devExt)
    for (const VkExtensionProperties &a : davail)
      if (e == a.extensionName) denabled.push_back(a.extensionName);

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = (uint32_t)denabled.size();
  dci.ppEnabledExtensionNames = denabled.data();
  if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
    *error = "vkCreateDevice failed";
    return false;
  }
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
  return true;
}

uint32_t PipRenderer::MemoryType(uint32_t bits, VkMemoryPropertyFlags props) const {
  for (uint32_t i = 0; i < memProps_.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props) return i;
  return UINT32_MAX;
}

bool PipRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer *out) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage;
  if (vkCreateBuffer(device_, &bci, nullptr, &out->buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, out->buffer, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &out->memory) != VK_SUCCESS) return false;
  vkBindBufferMemory(device_, out->buffer, out->memory, 0);
  out->size = size;
  return vkMapMemory(device_, out->memory, 0, size, 0, &out->mapped) == VK_SUCCESS;
}

bool PipRenderer::CreateImage(int w, int h, VkFormat format, VkImageUsageFlags usage, Image *out) {
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {(uint32_t)w, (uint32_t)h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ici, nullptr, &out->image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, out->image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &out->memory) != VK_SUCCESS) return false;
  vkBindImageMemory(device_, out->image, out->memory, 0);
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = out->image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = format;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return vkCreateImageView(device_, &vci, nullptr, &out->view) == VK_SUCCESS;
}

void PipRenderer::Destroy(Buffer *b) {
  if (b->buffer) vkDestroyBuffer(device_, b->buffer, nullptr);
  if (b->memory) vkFreeMemory(device_, b->memory, nullptr);
  *b = Buffer{};
}

void PipRenderer::Destroy(Image *i) {
  if (i->view) vkDestroyImageView(device_, i->view, nullptr);
  if (i->image) vkDestroyImage(device_, i->image, nullptr);
  if (i->memory) vkFreeMemory(device_, i->memory, nullptr);
  *i = Image{};
}

bool PipRenderer::InitDevice(std::string *error) {
  if (!CreateDevice(error)) return false;
  *error = "command buffer setup failed";
  VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cp.queueFamilyIndex = queueFamily_;
  if (vkCreateCommandPool(device_, &cp, nullptr, &cmdPool_) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = cmdPool_;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device_, &ca, &cmd_) != VK_SUCCESS) return false;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS) return false;
  error->clear();
  return true;
}

void PipRenderer::Barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                          VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = from;
  b.newLayout = to;
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool PipRenderer::SubmitAndWait() {
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  vkResetFences(device_, 1, &fence_);
  return vkQueueSubmit(queue_, 1, &si, fence_) == VK_SUCCESS &&
         vkWaitForFences(device_, 1, &fence_, VK_TRUE, 1000000000ull) == VK_SUCCESS;
}

void PipRenderer::FillTexture(vr::VRVulkanTextureData_t *data, VkImage image, int w, int h, vr::Texture_t *texture) {
  data->m_nImage = (uint64_t)image;
  data->m_pDevice = device_;
  data->m_pPhysicalDevice = physical_;
  data->m_pInstance = instance_;
  data->m_pQueue = queue_;
  data->m_nQueueFamilyIndex = queueFamily_;
  data->m_nWidth = (uint32_t)w;
  data->m_nHeight = (uint32_t)h;
  data->m_nFormat = VK_FORMAT_R8G8B8A8_UNORM;
  data->m_nSampleCount = 1;
  texture->handle = data;
  texture->eType = vr::TextureType_Vulkan;
  texture->eColorSpace = vr::ColorSpace_Gamma;
}

bool PipRenderer::InitCameras(const std::vector<CameraModel> &cams, std::string *error) {
  cams_ = cams;
  if (cams_.empty() || cams_.size() > 2) {
    *error = "need one or two cameras";
    return false;
  }
  if (!device_) {
    *error = "no Vulkan device";
    return false;
  }
  auto fail = [&](const char *what) {
    *error = what;
    return false;
  };

  for (int i = 0; i < 2; i++) {
    const int w = i < (int)cams_.size() ? cams_[i].width : 1, h = i < (int)cams_.size() ? cams_[i].height : 1;
    if (!CreateImage(w, h, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &camImages_[i]))
      return fail("camera image");
    if (!CreateBuffer((VkDeviceSize)w * h, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging_[i])) return fail("staging buffer");
    memset(staging_[i].mapped, 0, (size_t)w * h);
    if (!CreateImage(kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &outImages_[i]))
      return fail("output image");
    if (!CreateBuffer(sizeof(ParamsGpu), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &params_[i])) return fail("uniform buffer");
  }

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) return fail("sampler");

  VkDescriptorSetLayoutBinding b[3] = {};
  b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  b[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dl.bindingCount = 3;
  dl.pBindings = b;
  if (vkCreateDescriptorSetLayout(device_, &dl, nullptr, &setLayout_) != VK_SUCCESS) return fail("descriptor layout");

  VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
                                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dp.maxSets = 2;
  dp.poolSizeCount = 3;
  dp.pPoolSizes = ps;
  if (vkCreateDescriptorPool(device_, &dp, nullptr, &pool_) != VK_SUCCESS) return fail("descriptor pool");
  VkDescriptorSetLayout layouts[2] = {setLayout_, setLayout_};
  VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  da.descriptorPool = pool_;
  da.descriptorSetCount = 2;
  da.pSetLayouts = layouts;
  if (vkAllocateDescriptorSets(device_, &da, sets_) != VK_SUCCESS) return fail("descriptor sets");
  for (int s = 0; s < 2; s++) {
    VkDescriptorImageInfo camInfo[2] = {{sampler_, camImages_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {sampler_, camImages_[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkDescriptorImageInfo outInfo = {VK_NULL_HANDLE, outImages_[s].view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bufInfo = {params_[s].buffer, 0, sizeof(ParamsGpu)};
    VkWriteDescriptorSet w[3] = {};
    for (int i = 0; i < 3; i++) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstSet = sets_[s];
      w[i].dstBinding = i;
      w[i].descriptorType = b[i].descriptorType;
      w[i].descriptorCount = b[i].descriptorCount;
    }
    w[0].pImageInfo = camInfo;
    w[1].pImageInfo = &outInfo;
    w[2].pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
  }

  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &setLayout_;
  if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout_) != VK_SUCCESS) return fail("pipeline layout");
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = sizeof kReprojectSpv;
  smi.pCode = kReprojectSpv;
  VkShaderModule module;
  if (vkCreateShaderModule(device_, &smi, nullptr, &module) != VK_SUCCESS) return fail("shader module");
  VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpi.stage.module = module;
  cpi.stage.pName = "main";
  cpi.layout = pipelineLayout_;
  VkResult pr = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline_);
  vkDestroyShaderModule(device_, module, nullptr);
  if (pr != VK_SUCCESS) return fail("compute pipeline");

  return true;
}

bool PipRenderer::Render(const std::vector<CameraImage> &images, const ViewParams &view, vr::Texture_t *texture) {
  if (!pipeline_ || images.size() != cams_.size()) return false;
  for (size_t i = 0; i < cams_.size(); i++) {
    if (images[i].width != cams_[i].width || images[i].height != cams_[i].height) return false;
    memcpy(staging_[i].mapped, images[i].pixels.data(), images[i].pixels.size());
  }

  const int slot = next_;
  next_ ^= 1;
  ParamsGpu p{};
  for (size_t i = 0; i < cams_.size(); i++) {
    const CameraModel &m = cams_[i];
    CamGpu &g = p.cam[i];
    Put3(g.axisX, m.axisX);
    Put3(g.axisY, m.axisY);
    Put3(g.axisZ, m.axisZ);
    g.focal[0] = (float)m.fx, g.focal[1] = (float)m.fy, g.focal[2] = (float)m.cx, g.focal[3] = (float)m.cy;
    for (int k = 0; k < 4; k++) g.k[k] = (float)m.k[k];
    g.size[0] = (float)m.width, g.size[1] = (float)m.height, g.size[2] = (float)m.maxTheta, g.size[3] = 1;
  }
  const double tanX = std::tan(view.hfovDeg * M_PI / 360);
  p.view[0] = (float)tanX;
  p.view[1] = (float)(tanX * kHeight / kWidth);
  p.view[2] = (float)(6 * M_PI / 180);  // the primary camera fades out over its last 6 degrees
  p.view[3] = (float)view.gain;
  p.frame[0] = (float)view.highlight;
  p.frame[1] = 12;
  memcpy(params_[slot].mapped, &p, sizeof p);

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkResetCommandBuffer(cmd_, 0);
  vkBeginCommandBuffer(cmd_, &bi);

  // camera frames: staging -> images (contents fully replaced, so the old layout can be dropped)
  for (int i = 0; i < 2; i++) {
    const int w = i < (int)cams_.size() ? cams_[i].width : 1, h = i < (int)cams_.size() ? cams_[i].height : 1;
    Barrier(camImages_[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cmd_, staging_[i].buffer, camImages_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barrier(camImages_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  }
  VkImage out = outImages_[slot].image;
  Barrier(out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &sets_[slot], 0, nullptr);
  vkCmdDispatch(cmd_, (kWidth + 7) / 8, (kHeight + 7) / 8, 1);
  // SteamVR expects submitted Vulkan textures in TRANSFER_SRC_OPTIMAL
  Barrier(out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkEndCommandBuffer(cmd_);
  if (!SubmitAndWait()) return false;
  FillTexture(&vkTexture_, out, kWidth, kHeight, texture);
  return true;
}

bool PipRenderer::UploadPanel(const uint8_t *rgba, int width, int height, vr::Texture_t *texture) {
  if (!device_) return false;
  if (width != panelWidth_ || height != panelHeight_) {
    vkDeviceWaitIdle(device_);
    Destroy(&panelStaging_);
    for (Image &i : panelImages_) Destroy(&i);
    panelWidth_ = panelHeight_ = 0;
    if (!CreateBuffer((VkDeviceSize)width * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &panelStaging_)) return false;
    for (Image &i : panelImages_)
      if (!CreateImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &i))
        return false;
    panelWidth_ = width;
    panelHeight_ = height;
  }
  memcpy(panelStaging_.mapped, rgba, (size_t)width * height * 4);
  VkImage img = panelImages_[panelNext_].image;
  panelNext_ ^= 1;

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkResetCommandBuffer(cmd_, 0);
  vkBeginCommandBuffer(cmd_, &bi);
  Barrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
  vkCmdCopyBufferToImage(cmd_, panelStaging_.buffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  Barrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkEndCommandBuffer(cmd_);
  if (!SubmitAndWait()) return false;
  FillTexture(&panelTexture_, img, width, height, texture);
  return true;
}

}  // namespace framepip
