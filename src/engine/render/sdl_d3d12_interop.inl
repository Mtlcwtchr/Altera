// Campfire extension to SDL 3.2.8's D3D12 backend. Included after the original
// translation unit by CMake, without changing the fetched SDL checkout.
#ifdef SDL_GPU_D3D12
void *Campfire_D3D12Device(SDL_GPUDevice *device)
{
    return ((D3D12Renderer *)device->driverData)->device;
}

void *Campfire_D3D12Commands(SDL_GPUCommandBuffer *commands)
{
    return ((D3D12CommandBuffer *)commands)->graphicsCommandList;
}

void *Campfire_D3D12Texture(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *texture, bool output, bool restore)
{
    D3D12CommandBuffer *cb = (D3D12CommandBuffer *)commands;
    D3D12Texture *native = ((D3D12TextureContainer *)texture)->activeTexture;
    D3D12_RESOURCE_STATES external = output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES normal = D3D12_INTERNAL_DefaultTextureResourceState(native->container->header.info.usage);
    D3D12_INTERNAL_ResourceBarrier(cb, restore ? external : normal, restore ? normal : external,
                                 native->resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, output);
    D3D12_INTERNAL_TrackTexture(cb, native);
    return native->resource;
}

void Campfire_D3D12Restore(SDL_GPUCommandBuffer *commands)
{
    // NGX replaces descriptor heaps and root signatures. SDL must bind its own
    // state again before any subsequent blit, graphics or compute operation.
    D3D12CommandBuffer *cb = (D3D12CommandBuffer *)commands;
    if (cb->gpuDescriptorHeaps[0] && cb->gpuDescriptorHeaps[1]) {
        ID3D12DescriptorHeap *heaps[2] = {cb->gpuDescriptorHeaps[0]->handle, cb->gpuDescriptorHeaps[1]->handle};
        ID3D12GraphicsCommandList_SetDescriptorHeaps(cb->graphicsCommandList, 2, heaps);
    }
    cb->currentGraphicsPipeline = NULL;
    cb->currentComputePipeline = NULL;
}
#endif
