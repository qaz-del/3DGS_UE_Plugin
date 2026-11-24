# Spatial index for existing Gaussian assets

The spatial index is separate from the imported PLY assets. Building or refreshing it reads the existing root asset and its immediate child chunks. It does not reopen the PLY, reorder samples in those assets, or rewrite their packages.

After compiling a version with the component button, restart the Unreal Editor. Live Coding does not expose a newly added `CallInEditor` function in an already loaded component class.

1. Select the placed Gaussian actor or open its Blueprint, then select `GaussianPointSplatComponent` in Details.
2. Check that `Asset` points to the root Gaussian asset, not one of its child chunks.
3. Click **Create or Refresh Spatial Index** in the component's **Gaussian Splatting > Spatial Index** section. This creates an index next to the root asset and assigns it to the component.
4. Save the new index asset and the actor or Blueprint. The default leaf size is 8192.

The index can also be created through its asset factory if it appears in the Content Browser's New Asset menu. The component button does not depend on that menu.

The component falls back to its original render path if the index is missing or stale. Reimporting a Gaussian asset increments its data revision, so refresh the index after reimporting. Changing `LeafSize` also requires a refresh. Refreshing an index updates registered components that reference it.

The renderer traverses the index per view and skips nodes outside the view frustum. Fully visible nodes use their contiguous ranges; partially visible sets upload a compact list of source indices. Three packed Gaussian buffers upload once per scene proxy and remain resident on the GPU until that proxy is rebuilt or destroyed. This feature reduces splat shader work when enough nodes are outside the view, but it does not yet add streaming, LOD, or occlusion culling.

The index currently supports the root asset and one level of imported child chunks. The legacy `gps.Import.BuildSpatialBVH` option modifies Gaussian order inside imported assets; it is separate from this index and is not needed here.
