/*
 * Copyright © 2019 Raspberry Pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vk_util.h"

#include "v3dv_private.h"

/*
 * As anv and tu already points:
 *
 * "Pipeline layouts.  These have nothing to do with the pipeline.  They are
 * just multiple descriptor set layouts pasted together."
 */

VkResult
v3dv_CreatePipelineLayout(VkDevice _device,
                         const VkPipelineLayoutCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkPipelineLayout *pPipelineLayout)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_pipeline_layout *layout;

   assert(pCreateInfo->sType ==
          VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);

   layout = vk_alloc2(&device->alloc, pAllocator, sizeof(*layout), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (layout == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   layout->num_sets = pCreateInfo->setLayoutCount;

   for (uint32_t set = 0; set < pCreateInfo->setLayoutCount; set++) {
      V3DV_FROM_HANDLE(v3dv_descriptor_set_layout, set_layout,
                     pCreateInfo->pSetLayouts[set]);
      layout->set[set].layout = set_layout;
   }

   *pPipelineLayout = v3dv_pipeline_layout_to_handle(layout);

   return VK_SUCCESS;
}

void
v3dv_DestroyPipelineLayout(VkDevice _device,
                          VkPipelineLayout _pipelineLayout,
                          const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_pipeline_layout, pipeline_layout, _pipelineLayout);

   if (!pipeline_layout)
      return;
   vk_free2(&device->alloc, pAllocator, pipeline_layout);
}

VkResult
v3dv_CreateDescriptorPool(VkDevice _device,
                          const VkDescriptorPoolCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkDescriptorPool *pDescriptorPool)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_descriptor_pool *pool;
   uint64_t size = sizeof(struct v3dv_descriptor_pool);
   uint32_t descriptor_count = 0;

   for (unsigned i = 0; i < pCreateInfo->poolSizeCount; ++i) {
      if (pCreateInfo->pPoolSizes[i].type != VK_DESCRIPTOR_TYPE_SAMPLER)
         descriptor_count += pCreateInfo->pPoolSizes[i].descriptorCount;

      switch(pCreateInfo->pPoolSizes[i].type) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         break;
      default:
         unreachable("Unimplemented descriptor type");
         break;
      }
   }

   if (!(pCreateInfo->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)) {
      uint64_t host_size =
         pCreateInfo->maxSets * sizeof(struct v3dv_descriptor_set);
      host_size += sizeof(struct v3dv_descriptor) * descriptor_count;
      size += host_size;
   } else {
      size += sizeof(struct v3dv_descriptor_pool_entry) * pCreateInfo->maxSets;
   }

   pool = vk_alloc2(&device->alloc, pAllocator, size, 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (!pool)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(pool, 0, sizeof(*pool));

   if (!(pCreateInfo->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)) {
      pool->host_memory_base = (uint8_t*)pool + sizeof(struct v3dv_descriptor_pool);
      pool->host_memory_ptr = pool->host_memory_base;
      pool->host_memory_end = (uint8_t*)pool + size;
   }

   pool->max_entry_count = pCreateInfo->maxSets;

   *pDescriptorPool = v3dv_descriptor_pool_to_handle(pool);

   return VK_SUCCESS;
}

static void
descriptor_set_destroy(struct v3dv_device *device,
                       struct v3dv_descriptor_pool *pool,
                       struct v3dv_descriptor_set *set)
{
   assert(!pool->host_memory_base);

   for (uint32_t i = 0; i < pool->entry_count; i++) {
      if (pool->entries[i].set == set) {
         memmove(&pool->entries[i], &pool->entries[i+1],
                 sizeof(pool->entries[i]) * (pool->entry_count - i - 1));
         --pool->entry_count;
         break;
      }
   }
   vk_free2(&device->alloc, NULL, set);
}

void
v3dv_DestroyDescriptorPool(VkDevice _device,
                           VkDescriptorPool _pool,
                           const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_descriptor_pool, pool, _pool);

   if (!pool)
      return;

   if (!pool->host_memory_base) {
      for(int i = 0; i < pool->entry_count; ++i) {
         descriptor_set_destroy(device, pool, pool->entries[i].set);
      }
   }

   vk_free2(&device->alloc, pAllocator, pool);
}

VkResult
v3dv_ResetDescriptorPool(VkDevice _device,
                         VkDescriptorPool descriptorPool,
                         VkDescriptorPoolResetFlags flags)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_descriptor_pool, pool, descriptorPool);

   if (!pool->host_memory_base) {
      for(int i = 0; i < pool->entry_count; ++i) {
         descriptor_set_destroy(device, pool, pool->entries[i].set);
      }
   }

   pool->entry_count = 0;
   pool->host_memory_ptr = pool->host_memory_base;

   return VK_SUCCESS;
}

static int
binding_compare(const void *av, const void *bv)
{
   const VkDescriptorSetLayoutBinding *a =
      (const VkDescriptorSetLayoutBinding *) av;
   const VkDescriptorSetLayoutBinding *b =
      (const VkDescriptorSetLayoutBinding *) bv;

   return (a->binding < b->binding) ? -1 : (a->binding > b->binding) ? 1 : 0;
}

static VkDescriptorSetLayoutBinding *
create_sorted_bindings(const VkDescriptorSetLayoutBinding *bindings,
                       unsigned count,
                       struct v3dv_device *device,
                       const VkAllocationCallbacks *pAllocator)
{
   VkDescriptorSetLayoutBinding *sorted_bindings =
      vk_alloc2(&device->alloc, pAllocator,
                count * sizeof(VkDescriptorSetLayoutBinding),
                8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (!sorted_bindings)
      return NULL;

   memcpy(sorted_bindings, bindings,
          count * sizeof(VkDescriptorSetLayoutBinding));

   qsort(sorted_bindings, count, sizeof(VkDescriptorSetLayoutBinding),
         binding_compare);

   return sorted_bindings;
}

VkResult
v3dv_CreateDescriptorSetLayout(VkDevice _device,
                               const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                               const VkAllocationCallbacks *pAllocator,
                               VkDescriptorSetLayout *pSetLayout)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_descriptor_set_layout *set_layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);

   uint32_t max_binding = 0;
   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      max_binding = MAX2(max_binding, pCreateInfo->pBindings[j].binding);
   }

   uint32_t size = sizeof(struct v3dv_descriptor_set_layout) +
      (max_binding + 1) * sizeof(set_layout->binding[0]);

   set_layout = vk_alloc2(&device->alloc, pAllocator, size, 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (!set_layout)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkDescriptorSetLayoutBinding *bindings =
      create_sorted_bindings(pCreateInfo->pBindings, pCreateInfo->bindingCount,
                             device, pAllocator);

   if (!bindings) {
      vk_free2(&device->alloc, pAllocator, set_layout);
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   memset(set_layout->binding, 0,
          size - sizeof(struct v3dv_descriptor_set_layout));

   set_layout->binding_count = max_binding + 1;
   set_layout->flags = pCreateInfo->flags;
   set_layout->shader_stages = 0;

   uint32_t descriptor_count = 0;

   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *binding = bindings + i;
      uint32_t binding_number = binding->binding;

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         break;
      default:
         unreachable("Unknown descriptor type\n");
         break;
      }

      set_layout->binding[binding_number].type = binding->descriptorType;
      set_layout->binding[binding_number].array_size = binding->descriptorCount;
      set_layout->binding[binding_number].descriptor_index = descriptor_count;

      descriptor_count += binding->descriptorCount;

      /* FIXME: right now we don't use shader_stages. We could explore if we
       * could use it to add another filter to upload or allocate the
       * descriptor data.
       */
      set_layout->shader_stages |= binding->stageFlags;
   }

   vk_free2(&device->alloc, pAllocator, bindings);

   set_layout->descriptor_count = descriptor_count;

   *pSetLayout = v3dv_descriptor_set_layout_to_handle(set_layout);

   return VK_SUCCESS;
}

void
v3dv_DestroyDescriptorSetLayout(VkDevice _device,
                                VkDescriptorSetLayout _set_layout,
                                const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_descriptor_set_layout, set_layout, _set_layout);

   if (!set_layout)
      return;

   vk_free2(&device->alloc, pAllocator, set_layout);
}

static VkResult
descriptor_set_create(struct v3dv_device *device,
                      struct v3dv_descriptor_pool *pool,
                      const struct v3dv_descriptor_set_layout *layout,
                      struct v3dv_descriptor_set **out_set)
{
   struct v3dv_descriptor_set *set;
   uint32_t descriptor_count = layout->descriptor_count;
   unsigned range_offset = sizeof(struct v3dv_descriptor_set) +
      sizeof(struct v3dv_descriptor) * descriptor_count;
   unsigned mem_size = range_offset;

   if (pool->host_memory_base) {
      if (pool->host_memory_end - pool->host_memory_ptr < mem_size)
         return vk_error(device->instance, VK_ERROR_OUT_OF_POOL_MEMORY);

      set = (struct v3dv_descriptor_set*)pool->host_memory_ptr;
      pool->host_memory_ptr += mem_size;
   } else {
      set = vk_alloc2(&device->alloc, NULL, mem_size, 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

      if (!set)
         return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   memset(set, 0, mem_size);
   set->pool = pool;

   set->layout = layout;

   if (!pool->host_memory_base && pool->entry_count == pool->max_entry_count) {
      vk_free2(&device->alloc, NULL, set);
      return vk_error(device->instance, VK_ERROR_OUT_OF_POOL_MEMORY);
   }

   if (!pool->host_memory_base) {
      pool->entries[pool->entry_count].set = set;
      pool->entry_count++;
   }

   *out_set = set;

   return VK_SUCCESS;
}

VkResult
v3dv_AllocateDescriptorSets(VkDevice _device,
                            const VkDescriptorSetAllocateInfo *pAllocateInfo,
                            VkDescriptorSet *pDescriptorSets)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_descriptor_pool, pool, pAllocateInfo->descriptorPool);

   VkResult result = VK_SUCCESS;
   struct v3dv_descriptor_set *set = NULL;
   uint32_t i = 0;

   for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      V3DV_FROM_HANDLE(v3dv_descriptor_set_layout, layout,
                       pAllocateInfo->pSetLayouts[i]);

      result = descriptor_set_create(device, pool, layout, &set);
      if (result != VK_SUCCESS)
         break;

      pDescriptorSets[i] = v3dv_descriptor_set_to_handle(set);
   }

   if (result != VK_SUCCESS) {
      v3dv_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool,
                              i, pDescriptorSets);
      for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
         pDescriptorSets[i] = VK_NULL_HANDLE;
      }
   }

   return result;
}

VkResult
v3dv_FreeDescriptorSets(VkDevice _device,
                        VkDescriptorPool descriptorPool,
                        uint32_t count,
                        const VkDescriptorSet *pDescriptorSets)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_descriptor_pool, pool, descriptorPool);

   for (uint32_t i = 0; i < count; i++) {
      V3DV_FROM_HANDLE(v3dv_descriptor_set, set, pDescriptorSets[i]);

      if (set && !pool->host_memory_base)
         descriptor_set_destroy(device, pool, set);
   }

   return VK_SUCCESS;
}

void
v3dv_UpdateDescriptorSets(VkDevice  _device,
                          uint32_t descriptorWriteCount,
                          const VkWriteDescriptorSet *pDescriptorWrites,
                          uint32_t descriptorCopyCount,
                          const VkCopyDescriptorSet *pDescriptorCopies)
{
   for (uint32_t i = 0; i < descriptorWriteCount; i++) {
      const VkWriteDescriptorSet *writeset = &pDescriptorWrites[i];
      V3DV_FROM_HANDLE(v3dv_descriptor_set, set, writeset->dstSet);

      const struct v3dv_descriptor_set_binding_layout *binding_layout =
         set->layout->binding + writeset->dstBinding;

      struct v3dv_descriptor *descriptor = set->descriptors;

      descriptor += binding_layout->descriptor_index;
      descriptor += writeset->dstArrayElement;
      for (uint32_t j = 0; j < writeset->descriptorCount; ++j) {

         switch(writeset->descriptorType) {

         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            const VkDescriptorBufferInfo *buffer_info = writeset->pBufferInfo + j;
            V3DV_FROM_HANDLE(v3dv_buffer, buffer, buffer_info->buffer);

            descriptor->bo = buffer->mem->bo;
            descriptor->offset = buffer_info->offset;
            break;
         }
         default:
            unreachable("unimplemented descriptor type");
            break;
         }
         descriptor++;
      }
   }

   for (uint32_t i = 0; i < descriptorCopyCount; i++) {
      const VkCopyDescriptorSet *copyset = &pDescriptorCopies[i];
      V3DV_FROM_HANDLE(v3dv_descriptor_set, src_set,
                       copyset->srcSet);
      V3DV_FROM_HANDLE(v3dv_descriptor_set, dst_set,
                       copyset->dstSet);

      const struct v3dv_descriptor_set_binding_layout *src_binding_layout =
         src_set->layout->binding + copyset->srcBinding;
      const struct v3dv_descriptor_set_binding_layout *dst_binding_layout =
         dst_set->layout->binding + copyset->dstBinding;

      assert(src_binding_layout->type == dst_binding_layout->type);

      struct v3dv_descriptor *src_descriptor = src_set->descriptors;
      struct v3dv_descriptor *dst_descriptor = dst_set->descriptors;

      src_descriptor += src_binding_layout->descriptor_index;
      src_descriptor += copyset->srcArrayElement;

      dst_descriptor += dst_binding_layout->descriptor_index;
      dst_descriptor += copyset->dstArrayElement;

      for (uint32_t j = 0; j < copyset->descriptorCount; j++) {
         *dst_descriptor = *src_descriptor;
         dst_descriptor++;
         src_descriptor++;
      }
   }
}