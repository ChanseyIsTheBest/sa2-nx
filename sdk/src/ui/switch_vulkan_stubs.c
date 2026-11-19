/*
 * banjo-nx (2026-10-20): the Switch build without the Vulkan driver (REXGLUE_SWITCH_NVK OFF,
 * sdk/cmake/rexglue_switch.cmake). Mesa's static NVK (libvulkan.a) is not linked; these are the entry points the
 * code still references, all answering "no Vulkan":
 *   vkGetInstanceProcAddr, vkDestroyInstance       rex::ui::vulkan::VulkanInstance::Create (the native renderer and
 *                                                  ReXGlue's emulated Xenos GPU): every lookup is NULL, so Create
 *                                                  logs "failed to get ..." and returns nullptr
 *   vk_icdNegotiateLoaderICDInterfaceVersion,      plume::VulkanInterface (Renut on Vulkan): incompatible, every
 *   vk_icdGetInstanceProcAddr                      lookup NULL; the interface comes back invalid (no instance)
 * Every other Vulkan call goes through those lookups, so nothing else is needed. The deko3d renderer never calls
 * any of them; the first call is reported on stderr (switch_crash_hooks.c puts it on the SD) once.
 *
 * posix_memalign: Mesa's rust_switch_stubs.c (inside libvulkan.a) defined it and FFmpeg's av_malloc uses it;
 * devkitA64's newlib has memalign but not posix_memalign.
 */
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef void (*BanjoVkVoidFunction)(void);

static int g_banjo_vk_reported;

static void BanjoVkReport(const char *entry, const char *name) {
  if (g_banjo_vk_reported)
    return;
  g_banjo_vk_reported = 1;
  fprintf(stderr, "[vulkan] %s(%s): this build has no Vulkan driver (REXGLUE_SWITCH_NVK OFF); use banjo_renderer = "
                  "\"renut\" with renut_gpu_backend = \"deko3d\"\n",
          entry, name ? name : "");
}

BanjoVkVoidFunction vkGetInstanceProcAddr(void *instance, const char *name) {
  (void)instance;
  BanjoVkReport("vkGetInstanceProcAddr", name);
  return NULL;
}

void vkDestroyInstance(void *instance, const void *allocator) {
  (void)instance;
  (void)allocator;
}

/* VK_ERROR_INCOMPATIBLE_DRIVER */
int32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *supported_version) {
  (void)supported_version;
  BanjoVkReport("vk_icdNegotiateLoaderICDInterfaceVersion", NULL);
  return -9;
}

BanjoVkVoidFunction vk_icdGetInstanceProcAddr(void *instance, const char *name) {
  (void)instance;
  BanjoVkReport("vk_icdGetInstanceProcAddr", name);
  return NULL;
}

int posix_memalign(void **result, size_t alignment, size_t size) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment % sizeof(void *) != 0)
    return EINVAL;
  void *p = memalign(alignment, size);
  if (!p && size != 0)
    return ENOMEM;
  *result = p;
  return 0;
}
