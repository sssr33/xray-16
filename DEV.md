# Development Notes

## Build Instructions

Windows build and setup instructions are available in the OpenXRay wiki:

https://github.com/OpenXRay/xray-16/wiki/%5BEN%5D-How-to-build-and-setup-on-Windows

## Debug/Run From Visual Studio

1. Select the engine solution `src/engine.sln` and open its properties.
2. Select `Startup Project`.
3. Select `Single startup project`.
4. Select `xr_3da`.
5. Select the `xr_3da` project and open its properties.
6. Select `Debugging`.
7. Set `Working Directory` to the game folder that contains `fsgame.ltx`, for example:

```text
C:\PATH\TO\StalkerCallOfPripyat\
```

## Runtime Shader Resources With Steam Working Directory

When `xr_3da.vcxproj` uses the installed game folder as `Working Directory`, for example:

```text
D:\SteamLibrary\steamapps\common\Stalker Call of Pripyat
```

the engine reads `fsgame.ltx` from that folder and resolves runtime resources relative to the installed game, not relative to this repository's `res` folder.

For local shader development with the usual Windows DX renderer, copy changed shader files into the installed game's `gamedata` folder:

```text
D:\SteamLibrary\steamapps\common\Stalker Call of Pripyat\gamedata\shaders\r3
```

Use real file copies. Directory symlinks/junctions can fail to participate correctly in the engine's file list/cache path scanning in this setup, so do not rely on symlinks for these runtime shader files.

For example, if editing:

```text
res\gamedata\shaders\gl\clouds_volumetric_test.s
res\gamedata\shaders\gl\clouds_volumetric_test.ps
```

and the active renderer is `renderer_r2.5`, copy the matching runtime files to:

```text
D:\SteamLibrary\steamapps\common\Stalker Call of Pripyat\gamedata\shaders\r3\clouds_volumetric_test.s
D:\SteamLibrary\steamapps\common\Stalker Call of Pripyat\gamedata\shaders\r3\clouds_volumetric_test.ps
```

The active renderer is selected from the `renderer` line in `user.ltx`, for example:

```text
renderer renderer_r2.5
```

The shader subfolder is chosen by `RImplementation.getShaderPath()` in `src/Layers/xrRender_R2/r2.h`:

- DX11 builds use `r3\`.
- OpenGL builds use `gl\`.

That is why `renderer_r2.5` on Windows looks in `gamedata\shaders\r3`, not `gamedata\shaders\gl`.

The resource loading chain is:

- `src/xrEngine/Device_create.cpp` resolves `$game_data$\shaders.xr` and calls `GEnv.Render->OnDeviceCreate(...)`.
- `src/Layers/xrRender/ResourceManager_Loader.cpp` loads blender data from `shaders.xr` into `m_blenders`.
- `LS_Load()` in `ResourceManager_Scripting.cpp` / backend-specific scripting files scans `$game_shaders$ + RImplementation.getShaderPath()` and loads `.s` files.
- `.s` shader files are Lua scripts. They define functions such as `normal(shader, ...)`, call `shader:begin(...)`, and bind pixel/vertex shader names and samplers.

If `ref_shader::create("some_shader")` reaches `CResourceManager::_GetBlender()` and returns `nullptr`, the `.s` Lua shader script was probably not found or not loaded for the active renderer path. A useful breakpoint is in `LS_Load()` near:

```cpp
const char* shaderPath = RImplementation.getShaderPath();
xr_vector<char*>* folder = FS.file_list_open("$game_shaders$", shaderPath, FS_ListFiles | FS_RootOnly);
```

Check that `shaderPath` is the folder you expect and that the `.s` file appears in `folder`.

## Start Directly From a Save

One practical debug flow is:

1. Start a new game normally.
2. Create a save.
3. In Visual Studio, open `xr_3da` project properties.
4. Go to `Debugging`.
5. Set `Command Arguments` to:

```text
-nosplash -nointro -start server(your_save_name/single/alife/load) client(localhost)
```

Replace `your_save_name` with the save name without the `.sav` extension.

The `your_save_name/single/alife/load` part is important. It is not a "choose one of these options" list. It is a positional `start server(...)` format:

```text
<game_or_spawn>/<game_type>/<alife>/<new_or_load>
```

For loading a save:

```text
your_save_name/single/alife/load
```

means:

- `your_save_name` is the save name.
- `single` selects singleplayer game type.
- `alife` starts the game with ALife.
- `load` tells the engine to load the save instead of starting a new game.

Keep the syntax exact:

- Use `server(...)`, not `server (...)`.
- Use `client(...)`, not `client (...)`.
- Use lowercase `server`, `client`, `single`, `alife`, and `load`.
- Pass the save name without `.sav`.

Example:

```text
-nosplash -nointro -start server(opensave/single/alife/load) client(localhost)
```
