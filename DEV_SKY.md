# Dynamic Sky / Volumetric Clouds Notes

## Current Stage

We have a working R4/DX11 prototype pass for procedural animated clouds.

The current experiment is intentionally simple:

- It is inserted in `CRenderTarget::phase_combine()` near the existing sky/cloud rendering path.
- It uses skybox-like geometry around the camera, based on the same half-box idea used by `dxEnvironmentRender::RenderSky()`.
- The shader currently lives under `res/gamedata/shaders/r3/`.
- The pixel shader is plain HLSL for the game's `ps_5_0` shader compiler path.
- It writes cloud color to `SV_Target0` and writes zero to `SV_Target1` to avoid feeding bloom/highlight blur during testing.
- It uses procedural noise and time to create animated cloud motion.

Important finding: `I.tc0` from the skybox shader behaves like static cubemap direction coordinates. It is useful for procedural sky/cloud lookup, but it also makes cube face seams visible when the density field bends across cube faces.

The current visual result is already useful as a "dynamic skybox" prototype: moving storm-cloud shapes, dark overcast coloring, and warm yellow breaks in the cloud layer.

## Current Direction

The main direction is to develop and integrate the **dynamic skybox** approach, not separate per-cloud volume boxes.

Reasons:

- It is already hooked into the existing Environment/Sky/Cloud rendering path.
- It is cheap compared to many local raymarched cloud volumes.
- It fits the existing weather system: sky color, cloud color, fog, wind, sun, rain, and weather transitions already live in `CEnvironment`.
- It can provide most of the perceived atmosphere: overcast fronts, storm sky, cloud motion, sunrise/sunset color, moonlit cloud layers.
- It is easier to author as weather presets and transitions.

Separate local cloud volumes remain interesting later, but they should be treated as optional hero/foreground effects rather than the primary sky system.

## Dynamic Skybox vs Cloud Boxes

### Dynamic Skybox

One or a few large sky/cloud passes around the player.

Good for:

- Global weather identity.
- Large-scale cloud masses.
- Cheap animation.
- Time-of-day color.
- Overcast/storm transitions.
- Integration with existing sky/fog/postprocess.

Limitations:

- Cube/skydome coordinate seams can be visible.
- No true world-space cloud parallax.
- Harder to attach exact world positions to cloud features.
- Shape is mostly procedural and directional rather than object-based.

### Local Cloud Volume Boxes

Individual world-space boxes or volumes, each raymarched in its own bounds.

Good for:

- Real local parallax.
- Hero cloud banks.
- Cloud chunks that can move as world objects.
- Attaching lightning or effects to specific cloud volumes.
- More convincing close-range volume.

Costs/risks:

- More expensive, especially with many boxes or large screen coverage.
- Needs sorting/blending/depth handling.
- Needs LOD, half-resolution, temporal reprojection, or other optimizations.
- More complex authoring and integration.

Possible future use: add a few local "hero" storm cloud banks on top of the global dynamic skybox, not as a replacement for it.

## RDR2 Reference

Public information points to Red Dead Redemption 2 using a complete integrated atmospheric system rather than isolated simple cloud meshes.

Useful references:

- SIGGRAPH 2019 Advances in Real-Time Rendering course page:  
  https://advances.realtimerendering.com/s2019/index.htm
- Presentation title: `Creating the Atmospheric World of Red Dead Redemption 2: A Complete and Integrated Solution`
- Graphics Programming Weekly issue mentioning the RDR2 atmospheric presentation:  
  https://www.jendrikillner.com/post/graphics-programming-weekly-issue-96/
- Shacknews RDR2 PC graphics guide, noting volumetric/cloud quality settings:  
  https://www.shacknews.com/article/115067/red-dead-redemption-2-pc-graphics-settings-guide

General takeaways from public descriptions:

- RDR2 treats sky, clouds, fog, volumetric effects, light shafts, weather, and ambient lighting as one integrated atmospheric solution.
- Its cloud rendering appears tied to volumetric quality/resolution settings.
- It likely uses a large procedural/volumetric sky field and raymarching-style evaluation rather than individual cloud boxes for every visible cloud.
- The important lesson for this project is not to copy the full system, but to aim for a unified weather-driven atmosphere pipeline.

## Near-Term Ideas

Focus on making the dynamic skybox controllable and game-integrated:

- Feed shader parameters from `CurrentEnv`:
  - cloud color
  - fog color
  - sun direction/color
  - wind direction/speed
  - rain density
  - weather transition weight
- Add explicit cloud parameters:
  - coverage
  - density
  - layer height
  - softness
  - storm darkness
  - warm break intensity
  - animation speed
- Add multiple layers:
  - low dark storm layer
  - mid cloud body
  - high thin haze/veil
- Add sun/moon response:
  - brighter edges toward sun
  - warm backlight near sunrise/sunset
  - cold moonlit night clouds
- Add lightning integration:
  - choose lightning origins from high-density cloud regions
  - briefly brighten cloud color/density around lightning direction
  - later attach to local hero volumes if such volumes are added
- Add authoring path:
  - weather `.ltx` parameters
  - console/debug controls for fast tuning
  - smooth interpolation between weather descriptors

## Important Technical Notes

- Avoid writing bright test data to `SV_Target1` unless bloom/highlight response is being tested.
- The prototype originally wrote green to both targets; this caused strong bloom/blur. Writing zero to `SV_Target1` fixed that.
- The shader file must be valid HLSL for the R4 `ps_5_0` compiler path. GLSL-style declarations such as `out vec4` and `layout(...)` fail.
- The `.s` file should not bind unused samplers. An unused `s_tonemap` binding caused loader/runtime issues during the prototype.
- The current skybox geometry path is more reliable than a fullscreen quad for this prototype because it matches the existing sky rendering path.

## Open Questions

- Should the skybox geometry remain a cube/half-box, or should it become a smoother dome to reduce visible face bending?
- Should cloud density be generated in cubemap-direction space, world direction space, or projected atmospheric coordinates?
- Where should fog be applied: inside the cloud shader, later in combine, or both?
- How should cloud parameters be exposed to weather designers without making `.ltx` files too noisy?
- Can lightning sample the procedural density field cheaply enough to pick convincing cloud origins?
