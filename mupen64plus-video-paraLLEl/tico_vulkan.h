#ifndef TICO_VULKAN_IFACE_H__
#define TICO_VULKAN_IFACE_H__

/* How paraLLEl-RDP and the tico frontend share a Vulkan device. The frontend
 * creates the device through parallel_create_device() (paraLLEl picks the
 * features and extensions it needs), owns the swapchain, and hands this
 * interface back with parallel_set_vulkan_interface() before the ROM starts.
 *
 * paraLLEl renders each scanned-out frame into one of N images, N being the
 * highest bit of get_sync_index_mask() plus one. Before reusing image i it
 * calls wait_sync_index(), which returns once the frontend's last read of
 * image i has finished on the GPU. Every queue submission, paraLLEl's and the
 * frontend's, happens between lock_queue() and unlock_queue(). */

#include <volk.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tico_vk_image
{
   VkImageView image_view;
   VkImageLayout image_layout;
   VkImageViewCreateInfo create_info;
};

struct tico_vk_interface
{
   void *handle;
   /* image is the frame just scanned out, for the current sync index */
   void (*set_image)(void *handle, const struct tico_vk_image *image);
   uint32_t (*get_sync_index)(void *handle);
   uint32_t (*get_sync_index_mask)(void *handle);
   void (*wait_sync_index)(void *handle);
   void (*lock_queue)(void *handle);
   void (*unlock_queue)(void *handle);
};

struct tico_vk_context
{
   VkPhysicalDevice gpu;
   VkDevice device;
   VkQueue queue;
   uint32_t queue_family_index;
};

#ifdef __cplusplus
}
#endif

#endif
