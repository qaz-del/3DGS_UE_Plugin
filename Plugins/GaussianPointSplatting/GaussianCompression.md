# GPU Gaussian compression

The importer and existing `.uasset` files are unchanged. When a scene proxy is constructed, its selected Gaussians are transformed and quantized into three CPU arrays, then uploaded once to three persistent GPU buffers. Both regular and paper shaders decode the same representation.

| Field | Storage | Encoding |
| --- | ---: | --- |
| Position | 64 bits | X: 22 bits, Y: 21 bits, Z: 21 bits, normalized against the selected proxy's position bounds |
| Scale | 32-bit word | Logarithmic scale, 10 bits per axis; two spare bits |
| Rotation | 32-bit word | Largest quaternion component index: 2 bits; remaining three components: 10 bits each, with the largest component reconstructed as positive |
| RGB | 32 bits | 10 bits per channel; two spare bits |
| Opacity | 8 bits | Four values packed into each 32-bit word |

Geometry is 16 bytes per Gaussian, RGB is 4 bytes, and opacity averages 1 byte: **21 bytes per Gaussian**. At 8 million Gaussians this is about 168 MB (decimal), versus 512 MB for the previous four `float4` GPU buffers. At 15 million the respective values are about 315 MB and 960 MB. Index lists, paper-mode projected data, work queues, framebuffers, RDG allocation overhead, and other UE resources are additional.

Quantization error is bounded by half a position step in each axis, roughly half a log-scale step, half an RGB step (1/1023), and half an opacity step (1/255). Logarithmic scale distributes relative precision across small and large Gaussians. Quaternion compression chooses the largest-magnitude component, flips the quaternion sign if needed, and reconstructs the omitted component from unit length; the sign flip does not change the represented rotation.

The current GPU color is the imported degree-zero SH color. The original PLY contains higher-order SH coefficients, but the imported assets do not retain them. Preserving those coefficients would require a separate asset-format and import change. GPU compression does not reduce the size of existing `.uasset` files or the memory used by their UObject sample arrays.

Depth/ID integration additionally stores a persistent 32-bit source-index mapping (4 bytes/Gaussian), separate from the 21-byte compressed attributes. Combined static GPU storage is therefore about 25 bytes/Gaussian (about 200 MB at 8 million), before temporary renderer resources. The mapping points to original asset samples across spatial reordering and visible-index compaction.
