# Kolos private Wayland WSI (first Vulkan milestone)

`KolosLinux/protocol/kolos-graphics-v1.xml` is the canonical protocol. Mesa's
copy at `src/vulkan/wsi/protocol/kolos-graphics-v1.xml` must stay in sync. Meson
generates its client header and C code with `wayland-scanner`.

The application uses ordinary `VK_KHR_wayland_surface` and `VK_KHR_swapchain`.
When the compositor advertises `kolos_graphics_v1`, Wayland WSI binds a
`kolos_surface_v1` to the application's existing `wl_surface`. The compositor
chooses the Android session. Mesa requests the three AHB handles over that
Wayland object, validates the observed Qualcomm layout, and imports their
pixel FDs as the swapchain's `VkImage`/`VkDeviceMemory` objects. It never opens
`graphics.sock`.

Only `VK_FORMAT_R8G8B8A8_UNORM`, FIFO and opaque composite alpha are advertised.
The fixed current extent and maximum image count come from the current AHB
pool. Color attachment and transfer destination usage are accepted, subject to
the Turnip modifier/external-memory checks during import. Unsupported image
creation parameters fail swapchain creation.

`FREE` is the only event that makes an image acquirable. `ACK` does not release
it. Mesa's common present submission waits on the application's semaphores and
then waits its completion fence before sending private `PRESENT`; the protocol
currently has no GPU completion fence. The private `PRESENT` is the content
commit for this compositor. Mesa does not attach a `wl_buffer` or send an extra
`wl_surface.commit` for each AHB frame. Applications still create an
`xdg_toplevel`, make the initial empty commit, and ack its configure normally.

`POOL_REPLACED` stages a new pool and requests all its handles. The old
swapchain stays bound to its pinned generation and returns
`VK_ERROR_OUT_OF_DATE_KHR`; recreation binds the new complete generation. A
retired pool remains alive while a swapchain pins it or Android still owns a
presented slot. The platform's `FREE` releases that ownership.

## ARM64 Debian build

From `/home/kolos/Documents/MesaKolos`:

```sh
meson setup build-kolos --prefix=/opt/mesa-kolos --libdir=lib \
  -Dplatforms=wayland -Dvulkan-drivers=freedreno \
  -Dfreedreno-kmds=kgsl -Dgallium-drivers= \
  -Degl=disabled -Dglx=disabled -Dllvm=disabled
meson compile -C build-kolos
meson install -C build-kolos
```

Install requires root for `/opt/mesa-kolos`. To run against the already-running
Kolos compositor and Android GraphicsSmokeActivity:

```sh
export XDG_RUNTIME_DIR=/run/kolos/wayland
export WAYLAND_DISPLAY=wayland-0
export VK_DRIVER_FILES=/opt/mesa-kolos/share/vulkan/icd.d/freedreno_icd.aarch64.json
export LD_LIBRARY_PATH=/opt/mesa-kolos/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
vkcube --wsi wayland
```

Some Vulkan-Tools packages install the Wayland variant as `vkcube-wayland`;
use that executable if `vkcube --wsi wayland` is unavailable.

## Initial limitations

- The handle validator supports only the observed 148-byte Qualcomm RGBA8
  linear single-plane layout with two FDs. Unknown layouts fail closed.
- The private protocol has no GPU completion fence. The per-present CPU fence
  wait is conservative and should be replaced by explicit synchronization.
- The Android graphics bridge currently exposes one active session/window.
- The compositor currently rejects a `PRESENT` for a generation as soon as it
  reports a replacement. An already-submitted old-generation present racing
  with resize may therefore be rejected. The compositor should tolerate that
  in-flight present (or introduce an explicit generation handoff) before resize
  can be considered race-free for arbitrary Vulkan applications.
